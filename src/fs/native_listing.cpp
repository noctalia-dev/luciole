#include "fs/native_listing.h"

#include "fs/gio_interop.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <grp.h>
#include <linux/magic.h>
#include <optional>
#include <pwd.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
  // Filesystems whose per-file calls may block on a network or a userspace server keep GIO's enumeration, which
  // reads every fact on its worker; deferred access checks would otherwise run on the main thread.
  bool remoteFilesystem(const struct statfs& info) {
    constexpr std::uint32_t kCifs = 0xFF534D42;
    constexpr std::uint32_t kSmb2 = 0xFE534D42;
    constexpr std::uint32_t kFuse = 0x65735546;
    constexpr std::uint32_t kAfs = 0x5346414F;
    constexpr std::uint32_t kCoda = 0x73757245;
    constexpr std::uint32_t kCeph = 0x00C36400;
    constexpr std::uint32_t kNineP = 0x01021997;
    const auto type = static_cast<std::uint32_t>(info.f_type);
    return type == NFS_SUPER_MAGIC
        || type == SMB_SUPER_MAGIC
        || type == kCifs
        || type == kSmb2
        || type == kFuse
        || type == kAfs
        || type == kCoda
        || type == kCeph
        || type == kNineP;
  }

  // GLib's convert_pwd_string_to_utf8.
  std::optional<std::string> passwdText(const char* value) {
    if (value == nullptr || *value == '\0') {
      return std::nullopt;
    }
    if (g_utf8_validate(value, -1, nullptr)) {
      return value;
    }
    if (const luciole::GString converted(g_locale_to_utf8(value, -1, nullptr, nullptr, nullptr)); converted) {
      return converted.get();
    }
    const luciole::GString valid(g_utf8_make_valid(value, -1));
    return valid.get();
  }

  class Names {
  public:
    luciole::InternedText user(uid_t uid) {
      const auto [found, inserted] = m_users.try_emplace(uid);
      if (inserted) {
        passwd entry{};
        passwd* result = nullptr;
        std::vector<char> buffer(4096);
        getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &result);
        found->second = intern(result != nullptr ? passwdText(result->pw_name) : std::nullopt);
      }
      return found->second;
    }

    luciole::InternedText group(gid_t gid) {
      const auto [found, inserted] = m_groups.try_emplace(gid);
      if (inserted) {
        struct group entry{};
        struct group* result = nullptr;
        std::vector<char> buffer(4096);
        getgrgid_r(gid, &entry, buffer.data(), buffer.size(), &result);
        found->second = intern(result != nullptr ? passwdText(result->gr_name) : std::nullopt);
      }
      return found->second;
    }

  private:
    static luciole::InternedText intern(const std::optional<std::string>& text) {
      return text ? luciole::InternedText(*text) : luciole::InternedText();
    }
    std::unordered_map<uid_t, luciole::InternedText> m_users;
    std::unordered_map<gid_t, luciole::InternedText> m_groups;
  };

  // GLib's read_hidden_file: one name per line of the folder's .hidden file.
  std::unordered_set<std::string> hiddenNames(int folder) {
    std::unordered_set<std::string> names;
    const int fd = ::openat(folder, ".hidden", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      return names;
    }
    std::string contents;
    char buffer[4096];
    for (;;) {
      const auto count = ::read(fd, buffer, sizeof(buffer));
      if (count <= 0) {
        break;
      }
      contents.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    std::size_t start = 0;
    while (start <= contents.size()) {
      const auto end = contents.find('\n', start);
      names.emplace(contents.substr(start, end == std::string::npos ? std::string::npos : end - start));
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return names;
  }

  struct Folder {
    std::string path;
    dev_t device = 0;
    uid_t owner = 0;
    bool writable = false;
    bool sticky = false;
    bool mountRoot = false;
    bool utf8Names = true;
    uid_t user = 0;
    std::optional<bool> trashDirectory; // Taken from GIO once, for the first entry that may be trashed.
  };

  std::string childPath(const std::string& folder, std::string_view name) {
    std::string path = folder;
    if (!path.ends_with('/')) {
      path.push_back('/');
    }
    path.append(name);
    return path;
  }

  // GIO's local display name: the filename-charset conversion, marked when it had to replace bytes.
  std::string displayName(const Folder& folder, const char* name, const std::string& path) {
    if (folder.utf8Names && g_utf8_validate(name, -1, nullptr)) {
      return name;
    }
    std::string display = luciole::GString(g_filename_display_basename(path.c_str())).get();
    if (display.contains("\357\277\275")) {
      display += g_dgettext("glib20", " (invalid encoding)");
    }
    return display;
  }

  // The name-based type; `uncertain` is set when GIO would sniff the contents instead.
  std::string contentType(const struct statx& info, const char* name, bool& uncertain) {
    uncertain = false;
    const auto mode = info.stx_mode;
    if (S_ISLNK(mode)) {
      return "inode/symlink";
    }
    if (S_ISDIR(mode)) {
      return "inode/directory";
    }
    if (S_ISCHR(mode)) {
      return "inode/chardevice";
    }
    if (S_ISBLK(mode)) {
      return "inode/blockdevice";
    }
    if (S_ISFIFO(mode)) {
      return "inode/fifo";
    }
    if (S_ISREG(mode) && info.stx_size == 0) {
      return "application/x-zerosize";
    }
    if (S_ISSOCK(mode)) {
      return "inode/socket";
    }
    gboolean guessUncertain = FALSE;
    const luciole::GString guess(g_content_type_guess(name, nullptr, 0, &guessUncertain));
    uncertain = guessUncertain != FALSE;
    return guess ? guess.get() : "";
  }

  GFileType gioFileType(std::uint16_t mode) {
    if (S_ISREG(mode)) {
      return G_FILE_TYPE_REGULAR;
    }
    if (S_ISDIR(mode)) {
      return G_FILE_TYPE_DIRECTORY;
    }
    if (S_ISLNK(mode)) {
      return G_FILE_TYPE_SYMBOLIC_LINK;
    }
    if (S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode)) {
      return G_FILE_TYPE_SPECIAL;
    }
    return G_FILE_TYPE_UNKNOWN;
  }

  bool canTrash(Folder& folder, const std::string& path) {
    if (!folder.trashDirectory) {
      const luciole::GioPtr<GFile> file(g_file_new_for_path(path.c_str()));
      const luciole::GioPtr<GFileInfo> info(g_file_query_info(
          file.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
      ));
      if (!info) {
        return false;
      }
      folder.trashDirectory = luciole::infoBoolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH).value_or(false);
    }
    return *folder.trashDirectory;
  }

  struct DirCloser {
    void operator()(DIR* dir) const { ::closedir(dir); }
  };
} // namespace

luciole::NativeListing
luciole::listNativeFolder(const std::string& path, std::size_t entryLimit, GCancellable* cancellable) {
  NativeListing result;
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    result.unsupported = true;
    return result;
  }
  const std::unique_ptr<DIR, DirCloser> dir(::fdopendir(fd));
  if (!dir) {
    ::close(fd);
    result.unsupported = true;
    return result;
  }
  struct statfs filesystem{};
  struct stat folderInfo{};
  if (::fstatfs(fd, &filesystem) != 0 || remoteFilesystem(filesystem) || ::fstat(fd, &folderInfo) != 0) {
    result.unsupported = true;
    return result;
  }
  Folder folder{
      .path = path,
      .device = folderInfo.st_dev,
      .owner = folderInfo.st_uid,
      .writable = ::faccessat(AT_FDCWD, path.c_str(), W_OK, 0) == 0,
      .sticky = (folderInfo.st_mode & S_ISVTX) != 0,
      .mountRoot = false,
      .utf8Names = g_get_filename_charsets(nullptr) != FALSE,
      .user = ::geteuid(),
      .trashDirectory = std::nullopt,
  };
  struct stat parentInfo{};
  folder.mountRoot = path == "/" || (::fstatat(fd, "..", &parentInfo, 0) == 0 && parentInfo.st_dev != folder.device);
  const auto hidden = hiddenNames(fd);
  Names names;
  FileIconPool icons;
  icons.setLocalFolder(path.c_str());
  const auto folderLocation = std::make_shared<const Location>(Location::fromPath(path));
  constexpr unsigned kRequired = STATX_TYPE
      | STATX_MODE
      | STATX_NLINK
      | STATX_UID
      | STATX_GID
      | STATX_MTIME
      | STATX_CTIME
      | STATX_INO
      | STATX_SIZE
      | STATX_BLOCKS;

  for (std::size_t count = 0;; ++count) {
    if (count % 256 == 0 && g_cancellable_is_cancelled(cancellable) != FALSE) {
      return {};
    }
    errno = 0;
    const dirent* item = ::readdir(dir.get());
    if (item == nullptr) {
      if (errno != 0) {
        result.unsupported = true;
      }
      break;
    }
    const char* name = item->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      continue;
    }
    if (entryLimit != 0 && result.entries.size() >= entryLimit) {
      result.limited = true;
      break;
    }
    struct statx info{};
    if (::statx(fd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_BASIC_STATS | STATX_BTIME, &info) != 0) {
      if (errno == ENOENT) {
        continue; // Removed since readdir; the folder monitor reports it.
      }
      result.unsupported = true;
      break;
    }
    if ((info.stx_mask & kRequired) != kRequired) {
      result.unsupported = true;
      break;
    }
    const auto full = childPath(folder.path, name);
    const auto mode = info.stx_mode;
    const auto device = makedev(info.stx_dev_major, info.stx_dev_minor);
    const bool writable = folder.writable
        && (!folder.sticky || folder.user == info.stx_uid || folder.user == folder.owner || folder.user == 0);
    bool uncertain = false;
    const auto type = contentType(info, name, uncertain);
    DirectoryEntry entry{
        .id = 0,
        .place = EntryPlace::inFolder(folderLocation),
        .name = name,
        .type = luciole::fileType(gioFileType(mode)),
        .hidden = name[0] == '.'
            || hidden.contains(name)
            || (folder.mountRoot
                && std::strcmp(name, "lost+found") == 0
                && S_ISDIR(mode)
                && info.stx_uid == 0
                && info.stx_gid == 0),
        .symlink = S_ISLNK(mode),
        .size = info.stx_size <= G_MAXINT64 ? std::optional<std::uint64_t>(info.stx_size) : std::nullopt,
        .modifiedSeconds = static_cast<std::uint64_t>(info.stx_mtime.tv_sec),
        .access = {
            .read = std::nullopt,
            .write = std::nullopt,
            .execute = std::nullopt,
            .rename = writable,
            .trash = writable && canTrash(folder, full),
            .remove = writable
        },
    };
    entry.setDisplayName(displayName(folder, name, full));
    entry.accessDeferred = true;
    // GIO's local etag::value and id::filesystem/id::file, kept as numbers.
    entry.tags = LocalTags{
        .etagSeconds = static_cast<std::uint64_t>(info.stx_mtime.tv_sec),
        .device = static_cast<std::uint64_t>(device),
        .inode = info.stx_ino,
        .etagNanoseconds = info.stx_mtime.tv_nsec,
        .etag = true,
        .identity = true,
    };
    entry.contentType = type.empty() ? InternedText() : InternedText(type);
    entry.details = EntryMetadata{
        .accessedSeconds = (info.stx_mask & STATX_ATIME) != 0
            ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(info.stx_atime.tv_sec))
            : std::nullopt,
        .createdSeconds = (info.stx_mask & STATX_BTIME) != 0
            ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(info.stx_btime.tv_sec))
            : std::nullopt,
        .owner = names.user(info.stx_uid),
        .group = names.group(info.stx_gid),
        .mode = mode & 07777U,
    };
    entry.contentRevision = ChangeRevision{
        .seconds = static_cast<std::uint64_t>(info.stx_ctime.tv_sec), .nanoseconds = info.stx_ctime.tv_nsec
    };
    entry.icon = icons.getLocal(
        name, gioFileType(mode), entry.contentType ? entry.contentType->c_str() : nullptr, *folderLocation
    );
    if (uncertain) {
      result.uncertain.push_back(static_cast<std::uint32_t>(result.entries.size()));
    }
    result.entries.push_back(std::move(entry));
  }
  return result;
}

std::optional<std::string> luciole::sniffContentType(const std::string& path) {
  const GioPtr<GFile> file(g_file_new_for_path(path.c_str()));
  const GioPtr<GFileInfo> sniffed(g_file_query_info(
      file.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
  ));
  const char* type = sniffed ? infoString(sniffed.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE) : nullptr;
  return type != nullptr ? std::optional<std::string>(type) : std::nullopt;
}
