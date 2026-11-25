#include "fs/posix_fd.h"

#include "fs/file_error.h"
#include "fs/gio_ptr.h"

#include <cerrno>
#include <fcntl.h>
#include <glib.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <string>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
  // NFS and FUSE filesystems without rename flag support report EINVAL; ENOSYS covers kernels without renameat2.
  bool flagsRejected(int error) { return error == EINVAL || error == ENOSYS; }

  int renameVacant(int fromDir, const char* from, int toDir, const char* to) {
    struct stat existing{};
    if (::fstatat(toDir, to, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
      errno = EEXIST;
      return -1;
    }
    if (errno != ENOENT) {
      return -1;
    }
    return ::renameat(fromDir, from, toDir, to);
  }

  int failWith(int error) {
    errno = error;
    return -1;
  }
} // namespace

luciole::Fd::~Fd() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

luciole::Fd::Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}

luciole::Fd& luciole::Fd::operator=(Fd&& other) noexcept {
  if (this != &other) {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
    m_fd = std::exchange(other.m_fd, -1);
  }
  return *this;
}

int luciole::Fd::close() { return ::close(std::exchange(m_fd, -1)); }

luciole::Fd luciole::checkedFd(int fd) {
  if (fd < 0) {
    throwErrno(errno);
  }
  return Fd(fd);
}

struct stat luciole::statFd(int fd) {
  struct stat value{};
  if (::fstat(fd, &value) != 0) {
    throwErrno(errno);
  }
  return value;
}

bool luciole::sameMount(int first, int second) {
  const auto mount = [](int fd) {
    struct statx value{};
    if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &value) != 0
        || (value.stx_mask & STATX_MNT_ID) == 0) {
      throwFileError(FileErrorKind::Unsupported, "Cannot identify the filesystem mount boundary.");
    }
    return value.stx_mnt_id;
  };
  return mount(first) == mount(second);
}

bool luciole::sameIdentity(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
}

bool luciole::sameFileRevision(const struct stat& a, const struct stat& b) {
  return sameIdentity(a, b)
      && a.st_size == b.st_size
      && a.st_mode == b.st_mode
      && a.st_uid == b.st_uid
      && a.st_gid == b.st_gid
      && a.st_nlink == b.st_nlink
      && a.st_mtim.tv_sec == b.st_mtim.tv_sec
      && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
      && a.st_ctim.tv_sec == b.st_ctim.tv_sec
      && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

struct stat luciole::statUnlinked(int parent, int pinned, const struct stat& before) {
  struct statfs filesystem{};
  const bool nfs = ::fstatfs(parent, &filesystem) == 0 && filesystem.f_type == NFS_SUPER_MAGIC;
  struct stat after{};
  if (::fstat(pinned, &after) != 0) {
    // The NFS server already dropped a removed folder.
    if (!nfs || errno != ESTALE || !S_ISDIR(before.st_mode)) {
      throwErrno(errno);
    }
    after = before;
    after.st_nlink = 0;
    return after;
  }
  if (nfs && after.st_nlink == before.st_nlink && after.st_nlink > 0) {
    after.st_nlink = S_ISDIR(after.st_mode) ? 0 : after.st_nlink - 1;
  }
  return after;
}

int luciole::renameNoReplace(int fromDir, const char* from, int toDir, const char* to) {
  if (::syscall(SYS_renameat2, fromDir, from, toDir, to, RENAME_NOREPLACE) == 0) {
    return 0;
  }
  if (!flagsRejected(errno)) {
    return -1;
  }
  return renameVacant(fromDir, from, toDir, to);
}

int luciole::renameExchange(int fromDir, const char* from, int toDir, const char* to) {
  if (::syscall(SYS_renameat2, fromDir, from, toDir, to, RENAME_EXCHANGE) == 0) {
    return 0;
  }
  if (!flagsRejected(errno)) {
    return -1;
  }
  struct stat present{};
  if (::fstatat(fromDir, from, &present, AT_SYMLINK_NOFOLLOW) != 0
      || ::fstatat(toDir, to, &present, AT_SYMLINK_NOFOLLOW) != 0) {
    return -1;
  }
  // The swap renames `from` before it can learn that `to` cannot move; restoring it still changes its ctime, which
  // callers treat as a changed item. Refuse up front where renameat2 would refuse without touching either name.
  if (::faccessat(fromDir, ".", W_OK | X_OK, AT_EACCESS) != 0
      || ::faccessat(toDir, ".", W_OK | X_OK, AT_EACCESS) != 0) {
    return -1;
  }
  const GString uuid(g_uuid_string_random());
  const auto aside = ".luciole-exchange-" + std::string(uuid.get());
  if (::renameat(fromDir, from, fromDir, aside.c_str()) != 0) {
    return -1;
  }
  if (renameVacant(toDir, to, fromDir, from) != 0) {
    const int error = errno;
    (void)::renameat(fromDir, aside.c_str(), fromDir, from);
    return failWith(error);
  }
  if (renameVacant(fromDir, aside.c_str(), toDir, to) != 0) {
    const int error = errno;
    if (renameVacant(fromDir, from, toDir, to) == 0) {
      (void)::renameat(fromDir, aside.c_str(), fromDir, from);
    }
    return failWith(error);
  }
  return 0;
}

bool luciole::sameRenamedRevision(const struct stat& before, const struct stat& after, bool exchanged) {
  auto expected = before;
  expected.st_ctim = after.st_ctim;
  if (exchanged && S_ISDIR(before.st_mode)) {
    expected.st_mtim = after.st_mtim;
  }
  return sameFileRevision(expected, after);
}

void luciole::raiseFileDescriptorLimit() {
  struct rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < limit.rlim_max) {
    limit.rlim_cur = limit.rlim_max;
    (void)::setrlimit(RLIMIT_NOFILE, &limit);
  }
}
