#include "fs/folder_totals_walk.h"

#include "fs/file_query.h"
#include "fs/gio_interop.h"
#include "fs/network_info.h"
#include "fs/network_types.h"
#include "fs/posix_fd.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <unistd.h>

namespace {
  struct CloseDirectory {
    void operator()(DIR* value) const { ::closedir(value); }
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
        const std::function<void(const luciole::FolderTotalsSnapshot&)>& progress, luciole::FolderTotalsLimits limits,
        const std::vector<luciole::MountConfiguration>& mounts
    )
        : m_roots(std::move(roots)), m_token(token), m_progress(progress), m_limits(limits), m_mounts(mounts) {
      for (const auto& mount : mounts) {
        if (mount.automount) {
          m_boundaries.emplace(luciole::Location::fromPath(mount.path).uri(), &mount);
        }
      }
      std::ranges::stable_sort(m_roots, {}, [](const auto& entry) { return entry.location().uri().size(); });
      for (const auto& root : m_roots) {
        m_selected.emplace(root.location().uri(), &root);
      }
      m_result.phase = luciole::FolderTotalsPhase::Running;
    }
    luciole::FolderTotalsSnapshot run() {
      m_progress(m_result);
      for (const auto& root : m_roots) {
        if (stopped()) {
          break;
        }
        const auto location = root.location();
        if (m_seenRoots.contains(location.uri())) {
          continue;
        }
        try {
          if (!location.isNative() || location.isTrash()) {
            ++m_result.unsupported;
            continue;
          }
          const auto expected = luciole::automountFor(location, m_mounts);
          const auto guard = expected ? luciole::openSystemMount(location, m_token, expected) : luciole::Fd{};
          const auto fd = luciole::checkedFd(::open(location.nativePath()->c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
          const auto value = luciole::statFd(fd.get());
          visit(fd.get(), value, location, 0);
          struct stat current{};
          if (::lstat(location.nativePath()->c_str(), &current) != 0 || !sameStamp(value, current)) {
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
    void validateRoot(const luciole::DirectoryEntry& root, const struct stat& value) const {
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
    void visit(int fd, const struct stat& value, const luciole::Location& location, std::size_t depth) {
      if (stopped()) {
        return;
      }
      if (const auto found = m_selected.find(location.uri()); found != m_selected.end()) {
        if (!m_seenRoots.insert(location.uri()).second) {
          return;
        }
        validateRoot(*found->second, value);
      }
      ++m_items;
      if (S_ISDIR(value.st_mode)) {
        if (!m_directories.emplace(value.st_dev, value.st_ino).second) {
          ++m_result.unavailable; // Directory alias/cycle: no duplicate subtree totals.
        } else {
          m_result.folders += depth != 0; // Contents exclude the selected root folders.
          if (depth >= m_limits.depth) {
            m_result.limited = true;
          } else {
            children(fd, value, location, depth);
          }
        }
      } else {
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
      if (m_items % 64 == 0) {
        m_progress(m_result);
      }
    }
    void children(int fd, const struct stat& baseline, const luciole::Location& location, std::size_t depth) {
      const int descriptor = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (descriptor < 0) {
        luciole::throwErrno(errno);
      }
      const std::unique_ptr<DIR, CloseDirectory> directory(::fdopendir(descriptor));
      if (!directory) {
        const int code = errno;
        ::close(descriptor);
        luciole::throwErrno(code);
      }
      while (!stopped()) {
        errno = 0;
        const auto* item = ::readdir(directory.get());
        if (!item) {
          if (errno) {
            failed(luciole::errnoError(errno));
          }
          break;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..") {
          continue;
        }
        try {
          const auto childLocation = location.child(name);
          const auto boundary = m_boundaries.find(childLocation.uri());
          const auto guard = boundary != m_boundaries.end()
              ? luciole::openSystemMount(childLocation, m_token, *boundary->second)
              : luciole::Fd{};
          const auto child =
              luciole::checkedFd(::openat(::dirfd(directory.get()), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
          const auto value = luciole::statFd(child.get());
          visit(child.get(), value, childLocation, depth + 1);
          struct stat current{};
          if (::fstatat(::dirfd(directory.get()), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
              || !sameStamp(value, current)) {
            ++m_result.changed;
          }
        } catch (const luciole::FileErrorException& error) {
          failed(error.error());
        }
      }
      if (!sameStamp(baseline, luciole::statFd(fd))) {
        ++m_result.changed;
      }
    }
    std::vector<luciole::DirectoryEntry> m_roots;
    const luciole::CancelToken& m_token;
    const std::function<void(const luciole::FolderTotalsSnapshot&)>& m_progress;
    luciole::FolderTotalsLimits m_limits;
    const std::vector<luciole::MountConfiguration>& m_mounts;
    std::map<std::string, const luciole::MountConfiguration*> m_boundaries;
    luciole::FolderTotalsSnapshot m_result;
    std::size_t m_items = 0;
    std::map<std::string, const luciole::DirectoryEntry*> m_selected;
    std::set<std::string> m_seenRoots;
    std::set<std::pair<dev_t, ino_t>> m_directories;
  };
} // namespace

luciole::FolderTotalsSnapshot luciole::walkFolderTotals(
    std::vector<DirectoryEntry> roots, const CancelToken& token,
    const std::function<void(const FolderTotalsSnapshot&)>& progress, FolderTotalsLimits limits
) {
  const auto lane =
      std::ranges::any_of(roots, [](const auto& root) { return ioLane(root.location()) == IoLane::Network; })
      ? IoLane::Network
      : IoLane::Native;
  bool known = false;
  const auto mounts = configuredSystemMounts(&known);
  FolderTotalsSnapshot last;
  try {
    return NetworkProcess::acquire(lane)
        ->call(
            {{"action", "totals"},
             {"roots", roots},
             {"items", limits.items},
             {"depth", limits.depth},
             {"mounts", known ? NetworkMessage(mounts) : NetworkMessage(nullptr)}},
            token,
            [&](const NetworkMessage& value) {
              last = value.get<FolderTotalsSnapshot>();
              progress(last);
              return NetworkMessage{};
            }
        )
        .get<FolderTotalsSnapshot>();
  } catch (const FileErrorException& error) {
    if (error.error().kind != FileErrorKind::Cancelled) {
      throw;
    }
    last.phase = FolderTotalsPhase::Cancelled;
    return last;
  }
}
luciole::NetworkMessage luciole::networkFolderTotals(const NetworkMessage& request, const NetworkEvent& event) {
  const CancelToken token;
  const auto& snapshot = request.at("mounts");
  const auto mounts =
      snapshot.is_null() ? discoverSystemMounts(token) : snapshot.get<std::vector<MountConfiguration>>();
  const std::function<void(const FolderTotalsSnapshot&)> progress = [&](const auto& value) { (void)event(value); };
  Walker walk(
      request.at("roots").get<std::vector<DirectoryEntry>>(), token, progress,
      {request.at("items").get<std::size_t>(), request.at("depth").get<std::size_t>()}, mounts
  );
  return walk.run();
}
