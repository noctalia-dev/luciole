#include "fs/native_listing.h"

#include "fs/gio_interop.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <grp.h>
#include <limits>
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
  constexpr std::uint32_t kFuse = 0x65735546;

  // Filesystems whose per-file calls may block on a network or a userspace server keep GIO's enumeration, which
  // reads every fact on its worker; deferred access checks would otherwise run on the main thread.
  bool remoteFilesystem(const struct statfs& info) {
    constexpr std::uint32_t kCifs = 0xFF534D42;
    constexpr std::uint32_t kSmb2 = 0xFE534D42;
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
luciole::listNativeFolder(const std::string& path, std::size_t entryLimit, GCancellable* cancellable, bool catalogue) {
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
  if (::fstatfs(fd, &filesystem) != 0 || ::fstat(fd, &folderInfo) != 0) {
    result.unsupported = true;
    return result;
  }
  const bool remote = remoteFilesystem(filesystem);
  result.network = networkFilesystem(filesystem, folderInfo.st_dev);
  if (remote && (!catalogue || !result.network)) {
    result.unsupported = true;
    return result;
  }
  Folder folder{
      .path = path,
      .device = folderInfo.st_dev,
      .owner = folderInfo.st_uid,
      .writable = !catalogue && ::faccessat(AT_FDCWD, path.c_str(), W_OK, 0) == 0,
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
  if (!catalogue) {
    icons.setLocalFolder(path.c_str());
  }
  const auto folderLocation = std::make_shared<const Location>(Location::fromPath(path));
  BrowseCatalogue browse{
      .folder = folderLocation,
      .device = static_cast<std::uint64_t>(folderInfo.st_dev),
      .inode = folderInfo.st_ino,
      .names = {},
      .items = {},
      .displayNames = {}
  };
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
    if (entryLimit != 0 && (catalogue ? browse.items.size() : result.entries.size()) >= entryLimit) {
      result.limited = true;
      break;
    }
    struct statx info{};
    if (catalogue && item->d_type != DT_UNKNOWN && (!folder.mountRoot || std::strcmp(name, "lost+found") != 0)) {
      // Linux dirent types use the same kind bits as st_mode, without permissions or access claims.
      info.stx_mode = static_cast<std::uint16_t>(DTTOIF(item->d_type));
    } else if (result.network && catalogue && item->d_type == DT_UNKNOWN) {
      // Network servers that omit dirent kinds fall back to GIO enumeration instead of issuing per-entry statx calls.
      result.unsupported = true;
      break;
    } else {
      if (::statx(fd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_BASIC_STATS | STATX_BTIME, &info) != 0) {
        if (errno == ENOENT) {
          continue; // Removed since readdir; the folder monitor reports it.
        }
        result.unsupported = true;
        break;
      }
    }
    if (catalogue) {
      const auto length = std::strlen(name);
      if (length > UINT16_MAX || browse.names.size() > UINT32_MAX - length - 1) {
        result.unsupported = true;
        break;
      }
      if (!folder.utf8Names || g_utf8_validate(name, -1, nullptr) == FALSE) {
        const auto display = displayName(folder, name, childPath(path, name));
        if (display != name) {
          browse.displayNames.emplace(browse.items.size(), display);
        }
      }
      browse.items.push_back({
          .id = 0,
          .inodeHint = item->d_ino,
          .offset = static_cast<std::uint32_t>(browse.names.size()),
          .length = static_cast<std::uint16_t>(length),
          .type = fileType(gioFileType(info.stx_mode)),
          .hidden = name[0] == '.'
              || hidden.contains(name)
              || (folder.mountRoot
                  && std::strcmp(name, "lost+found") == 0
                  && S_ISDIR(info.stx_mode)
                  && info.stx_uid == 0
                  && info.stx_gid == 0),
          .symlink = S_ISLNK(info.stx_mode),
      });
      browse.names.append(name, length);
      browse.names.push_back('\0');
      continue;
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
  if (catalogue && !result.unsupported) {
    // Small folders retain immediate complete facts. A second short pass costs less than repeated row-worker
    // publications and preserves immediate selection/history behavior there.
    if (result.network) {
      if (browse.items.size() <= 64 || result.limited) {
        result.unsupported = true;
        return result;
      }
    } else if (browse.items.size() <= 512 || result.limited) {
      return listNativeFolder(path, entryLimit, cancellable, false);
    }
    browse.items.shrink_to_fit();
    browse.names.shrink_to_fit();
    result.catalogue = std::move(browse);
  }
  return result;
}

luciole::CatalogueValidation luciole::validateCatalogueRows(
    const BrowseCatalogue& catalogue, std::span<const std::size_t> rows, GCancellable* cancellable
) {
  CatalogueValidation result;
  const auto path = catalogue.folder->nativePath();
  const std::unique_ptr<DIR, DirCloser> directory(path ? ::opendir(path->c_str()) : nullptr);
  if (!directory) {
    result.parentChanged = errno == ENOENT || errno == ENOTDIR;
    result.unavailable = !result.parentChanged;
    return result;
  }
  const auto sameParent = [&](const struct stat& info) {
    return static_cast<std::uint64_t>(info.st_dev) == catalogue.device && info.st_ino == catalogue.inode;
  };
  struct stat info{};
  if (::fstat(::dirfd(directory.get()), &info) != 0) {
    result.unavailable = true;
    return result;
  }
  if (!sameParent(info)) {
    result.parentChanged = true;
    return result;
  }
  std::unordered_map<std::string_view, std::size_t> pending;
  for (const auto index : rows) {
    pending.emplace(catalogue.brief(index).name, index);
  }
  std::size_t count = 0;
  while (!pending.empty()) {
    if (++count % 256 == 0 && g_cancellable_is_cancelled(cancellable) != FALSE) {
      result.unavailable = true;
      return result;
    }
    errno = 0;
    const auto* item = ::readdir(directory.get());
    if (!item) {
      if (errno != 0) {
        result.unavailable = true;
        return result;
      }
      break;
    }
    if (const auto found = pending.find(item->d_name); found != pending.end()) {
      if (item->d_ino != catalogue.items[found->second].inodeHint) {
        result.changed.push_back(found->second);
      }
      pending.erase(found);
    }
  }
  for (const auto& [name, index] : pending) {
    result.changed.push_back(index);
  }
  if (::stat(path->c_str(), &info) != 0) {
    result.parentChanged = errno == ENOENT || errno == ENOTDIR;
    result.unavailable = !result.parentChanged;
  } else {
    result.parentChanged = !sameParent(info);
  }
  return result;
}

bool luciole::networkFilesystem(const std::string& path) {
  struct statfs filesystem{};
  struct stat info{};
  if (::statfs(path.c_str(), &filesystem) != 0 || ::stat(path.c_str(), &info) != 0) {
    return true;
  }
  return networkFilesystem(filesystem, info.st_dev);
}

bool luciole::networkFilesystem(const struct statfs& filesystem, dev_t device) {
  // Block-backed FUSE mounts such as ntfs-3g have a disk's device number; rclone and sshfs mounts have none.
  if (static_cast<std::uint32_t>(filesystem.f_type) == kFuse) {
    return major(device) == 0;
  }
  return remoteFilesystem(filesystem);
}

std::optional<std::string> luciole::sniffContentType(const std::string& path) {
  const GioPtr<GFile> file(g_file_new_for_path(path.c_str()));
  const GioPtr<GFileInfo> sniffed(g_file_query_info(
      file.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
  ));
  const char* type = sniffed ? infoString(sniffed.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE) : nullptr;
  return type != nullptr ? std::optional<std::string>(type) : std::nullopt;
}

bool luciole::linksToDirectory(const std::string& path) {
  struct statx target{};
  return ::statx(AT_FDCWD, path.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE, &target) == 0 && S_ISDIR(target.stx_mode);
}
