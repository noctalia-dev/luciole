#include "fs/posix_fd.h"

#include "fs/file_error.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>

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
