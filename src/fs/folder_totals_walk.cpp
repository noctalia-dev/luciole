#include "fs/folder_totals_walk.h"

#include "fs/file_query.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace {
  struct CloseDirectory {
    void operator()(DIR* value) const {
      if (value != nullptr) {
        ::closedir(value);
      }
    }
  };
  struct DevIno {
    dev_t device = 0;
    ino_t inode = 0;
    bool operator==(const DevIno&) const = default;
  };
  struct DevInoHash {
    std::size_t operator()(const DevIno& value) const noexcept {
      return std::hash<dev_t>{}(value.device) * 31 + std::hash<ino_t>{}(value.inode);
    }
  };
  bool sameStamp(const struct stat& a, const struct stat& b) {
    return luciole::sameIdentity(a, b)
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec
        && a.st_size == b.st_size;
  }
  class Walker {
  public:
    Walker(
        std::vector<luciole::DirectoryEntry> roots, const luciole::CancelToken& token,
        const std::function<void(const luciole::FolderTotalsSnapshot&)>& progress, luciole::FolderTotalsLimits limits
    )
        : m_roots(std::move(roots)), m_token(token), m_progress(progress), m_limits(limits) {
      // Selected roots by identity, so a root met again inside another root's
      // subtree is validated and counted once. Keys come from one lstat each
      // up front; the walk itself then compares bare device/inode pairs.
      for (const auto& root : m_roots) {
        const auto location = root.location();
        if (location.isNative() && !location.isTrash()) {
          if (const auto path = location.nativePath()) {
            struct stat value{};
            if (::lstat(path->c_str(), &value) == 0) {
              m_selected.try_emplace(DevIno{value.st_dev, value.st_ino}, &root);
            }
          }
        }
      }
      m_result.phase = luciole::FolderTotalsPhase::Running;
    }
    luciole::FolderTotalsSnapshot run() {
      m_progress(m_result);
      for (const auto& root : m_roots) {
        if (stopped()) {
          break;
        }
        try {
          const auto location = root.location();
          if (!location.isNative() || location.isTrash()) {
            ++m_result.unsupported;
            continue;
          }
          const auto path = location.nativePath();
          struct stat value{};
          if (!path || ::lstat(path->c_str(), &value) != 0) {
            failed(luciole::errnoError(errno));
            continue;
          }
          ++m_items;
          const DevIno identity{value.st_dev, value.st_ino};
          if (!m_seenRoots.insert(identity).second) {
            continue;
          }
          m_rootDev = value.st_dev;
          validateRoot(root, value);
          if (S_ISDIR(value.st_mode)) {
            if (!m_directories.emplace(DevIno{value.st_dev, value.st_ino}).second) {
              ++m_result.unavailable; // Directory alias/cycle: no duplicate subtree totals.
            } else if (m_limits.depth <= 0) {
              m_result.limited = true;
            } else {
              walkRoot(*path, value);
            }
          } else {
            countFile(value);
          }
          struct stat current{};
          if (::lstat(path->c_str(), &current) != 0 || !sameStamp(value, current)) {
            ++m_result.changed;
          }
        } catch (const luciole::FileErrorException& error) {
          failed(error.error());
        }
      }
      m_result.phase =
          m_token.cancelled() ? luciole::FolderTotalsPhase::Cancelled : luciole::FolderTotalsPhase::Complete;
      m_progress(m_result);
      return m_result;
    }

  private:
    bool stopped() {
      if (m_token.cancelled()) {
        return true;
      }
      if (m_items >= m_limits.items) {
        m_result.limited = true;
        return true;
      }
      return false;
    }
    void failed(const luciole::FileError& error) {
      if (error.kind == luciole::FileErrorKind::Cancelled) {
        return;
      }
      if (error.kind == luciole::FileErrorKind::Changed) {
        ++m_result.changed;
      } else {
        ++m_result.unavailable;
      }
      // Retain only one bounded error regardless of the tree size.
      m_result.error = error.message.substr(0, 256);
    }
    // One anchor check per selected root, matched by device and inode so the
    // walk itself never builds a URI per item.
    void validateRoot(const luciole::DirectoryEntry& root, const struct stat& value) {
      const auto location = root.location();
      const auto details = luciole::readFileDetails(location, m_token);
      const luciole::GioPtr<GFileInfo> info(g_file_query_info(
          luciole::gioFile(location).get(), "unix::device,unix::inode", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
          luciole::gioCancellable(m_token), nullptr
      ));
      if (m_token.cancelled()) {
        luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
      }
      if (!info
          || !g_file_info_has_attribute(info.get(), "unix::device")
          || !g_file_info_has_attribute(info.get(), "unix::inode")
          || g_file_info_get_attribute_uint32(info.get(), "unix::device") != value.st_dev
          || g_file_info_get_attribute_uint64(info.get(), "unix::inode") != value.st_ino
          || (root.identity() && root.identity() != details.identity)
          || (root.etag() && root.etag() != details.etag)
          || (root.contentRevision && root.contentRevision != details.contentRevision)) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "A selected item changed. Reopen Properties.");
      }
    }
    void countFile(const struct stat& value) {
      if (S_ISLNK(value.st_mode)) {
        ++m_result.links;
      } else if (S_ISREG(value.st_mode)) {
        ++m_result.files;
      } else {
        ++m_result.other;
      }
      // Special files have no meaningful content length and are never opened.
      if (S_ISREG(value.st_mode) || S_ISLNK(value.st_mode)) {
        if (value.st_size < 0) {
          ++m_result.unavailable;
        } else if (
            static_cast<std::uint64_t>(value.st_size) > std::numeric_limits<std::uint64_t>::max() - m_result.bytes
        ) {
          m_result.overflow = true;
        } else {
          m_result.bytes += static_cast<std::uint64_t>(value.st_size);
        }
      }
    }
    // Opens one directory for listing; the handle owns it from here.
    std::unique_ptr<DIR, CloseDirectory> openDirAt(int parent, const char* name) {
      const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        luciole::throwErrno(errno);
      }
      std::unique_ptr<DIR, CloseDirectory> directory(::fdopendir(fd));
      if (!directory) {
        const int code = errno;
        ::close(fd);
        luciole::throwErrno(code);
      }
      return directory;
    }
    std::unique_ptr<DIR, CloseDirectory> openDir(const std::string& path) {
      const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        luciole::throwErrno(errno);
      }
      std::unique_ptr<DIR, CloseDirectory> directory(::fdopendir(fd));
      if (!directory) {
        const int code = errno;
        ::close(fd);
        luciole::throwErrno(code);
      }
      return directory;
    }
    void walkRoot(const std::string& path, const struct stat& baseline) {
      auto directory = openDir(path);
      const int self = ::dirfd(directory.get());
      walkEntries(directory.get(), self, 0);
      struct stat current{};
      if (::fstat(self, &current) != 0 || !sameStamp(baseline, current)) {
        ++m_result.changed;
      }
    }
    void walkEntries(DIR* directory, int self, std::size_t depth) {
      while (!stopped()) {
        errno = 0;
        const auto* item = ::readdir(directory);
        if (!item) {
          if (errno) {
            failed(luciole::errnoError(errno));
          }
          break;
        }
        const std::string_view name(item->d_name);
        if (name == "." || name == "..") {
          continue;
        }
        // One stat per entry, never opened: symlinks are counted, never followed.
        struct stat value{};
        if (::fstatat(self, item->d_name, &value, AT_SYMLINK_NOFOLLOW) != 0) {
          failed(luciole::errnoError(errno));
          continue;
        }
        ++m_items;
        if ((m_items & 511) == 0) {
          m_progress(m_result);
        }
        const DevIno identity{value.st_dev, value.st_ino};
        if (const auto found = m_selected.find(identity); found != m_selected.end()) {
          if (!m_seenRoots.insert(identity).second) {
            continue;
          }
          validateRoot(*found->second, value);
        }
        if (S_ISDIR(value.st_mode)) {
          if (value.st_dev != m_rootDev) {
            // Another filesystem mounted inside the tree (a drive, a phone, a
            // container layer): count the mountpoint, never descend. Like du -x,
            // so a stray mount can't blow up the total.
            ++m_result.folders;
            continue;
          }
          if (!m_directories.emplace(DevIno{value.st_dev, value.st_ino}).second) {
            ++m_result.unavailable; // Directory alias/cycle: no duplicate subtree totals.
            continue;
          }
          ++m_result.folders;
          const std::size_t childDepth = depth + 1;
          if (childDepth >= m_limits.depth) {
            m_result.limited = true;
            continue;
          }
          try {
            auto child = openDirAt(self, item->d_name);
            const int childFd = ::dirfd(child.get());
            walkEntries(child.get(), childFd, childDepth);
            struct stat current{};
            if (::fstat(childFd, &current) != 0 || !sameStamp(value, current)) {
              ++m_result.changed;
            }
          } catch (const luciole::FileErrorException& error) {
            failed(error.error());
          }
        } else {
          countFile(value);
        }
      }
    }
    std::vector<luciole::DirectoryEntry> m_roots;
    const luciole::CancelToken& m_token;
    const std::function<void(const luciole::FolderTotalsSnapshot&)>& m_progress;
    luciole::FolderTotalsLimits m_limits;
    luciole::FolderTotalsSnapshot m_result;
    std::size_t m_items = 0;
    std::unordered_set<DevIno, DevInoHash> m_directories;
    std::unordered_map<DevIno, const luciole::DirectoryEntry*, DevInoHash> m_selected;
    std::unordered_set<DevIno, DevInoHash> m_seenRoots;
    // Device of the walked root: entries on other filesystems are mountpoints and never descended into.
    dev_t m_rootDev = 0;
  };
} // namespace

luciole::FolderTotalsSnapshot luciole::walkFolderTotals(
    std::vector<DirectoryEntry> roots, const CancelToken& token,
    const std::function<void(const FolderTotalsSnapshot&)>& progress, FolderTotalsLimits limits
) {
  return Walker(std::move(roots), token, progress, limits).run();
}
