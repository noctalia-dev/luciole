#include "operations/creation_stage.h"

#include "fs/file_error.h"
#include "fs/gio_ptr.h"
#include "fs/network_process.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <glib.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
  std::string namedDirectory(int fd) {
    std::array<char, 8192> buffer{};
    const auto count = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buffer.data(), buffer.size());
    if (count <= 0 || static_cast<std::size_t>(count) == buffer.size() || buffer[0] != '/') {
      return {};
    }
    std::string path(buffer.data(), static_cast<std::size_t>(count));
    const luciole::Fd current(::open(path.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat named{}, held{};
    if (!current.valid()
        || ::fstat(current.get(), &named) != 0
        || ::fstat(fd, &held) != 0
        || !luciole::sameIdentity(named, held)) {
      return {};
    }
    return path;
  }
} // namespace

luciole::CreationStage::CreationStage(int parent, bool discardOnClose)
    : m_parent(checkedFd(::fcntl(parent, F_DUPFD_CLOEXEC, 0))), m_discardOnClose(discardOnClose) {
  const GString uuid(g_uuid_string_random());
  m_name = ".luciole-create-" + std::string(uuid.get());
  if (::mkdirat(m_parent.get(), m_name.c_str(), 0700) != 0) {
    throwErrno(errno);
  }
  const Fd created(::openat(m_parent.get(), m_name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!created.valid()) {
    const int code = errno;
    (void)::unlinkat(m_parent.get(), m_name.c_str(), AT_REMOVEDIR);
    throwErrno(code);
  }
  // Private staging must remain usable even when the process umask denies owner access.
  const auto held = "/proc/self/fd/" + std::to_string(created.get());
  const auto mode = statFd(created.get()).st_mode;
  if ((mode & 0777) != 0700 && ::chmod(held.c_str(), 0700 | (mode & S_ISGID)) != 0) {
    throwErrno(errno);
  }
  m_directory = checkedFd(::open(held.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  m_originalPath = namedDirectory(fd());
}

luciole::CreationStage::~CreationStage() { (void)clear(); }

void luciole::CreationStage::adopt(Fd item) { m_item = std::move(item); }

void luciole::CreationStage::requireCurrent() const {
  struct stat current{};
  if (::fstatat(m_parent.get(), m_name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
      || !sameIdentity(current, statFd(fd()))) {
    throwFileError(FileErrorKind::Changed, "The private creation folder changed.");
  }
}

void luciole::CreationStage::requireItem() const {
  requireCurrent();
  struct stat current{};
  if (!m_item.valid()
      || ::fstatat(fd(), "item", &current, AT_SYMLINK_NOFOLLOW) != 0
      || !sameIdentity(current, statFd(item()))) {
    throwFileError(FileErrorKind::Changed, "The staged item changed.");
  }
}

void luciole::CreationStage::publish(const std::string& name, int parent) {
  requireItem();
  if (::syscall(SYS_renameat2, fd(), "item", parent >= 0 ? parent : m_parent.get(), name.c_str(), RENAME_NOREPLACE)
      != 0) {
    throwErrno(errno);
  }
}

void luciole::CreationStage::removeItem() {
  requireItem();
  if (::unlinkat(fd(), "item", S_ISDIR(statFd(item()).st_mode) ? AT_REMOVEDIR : 0) != 0) {
    throwErrno(errno);
  }
}

void luciole::CreationStage::exchange(const std::string& name, int parent, int visibleItem) {
  requireItem();
  struct stat current{};
  if (::fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
      || !sameIdentity(current, statFd(visibleItem))) {
    throwFileError(FileErrorKind::Changed, "The item to exchange changed.");
  }
  if (::syscall(SYS_renameat2, fd(), "item", parent, name.c_str(), RENAME_EXCHANGE) != 0) {
    throwErrno(errno);
  }
}

bool luciole::CreationStage::clear() {
  try {
    requireCurrent();
    if (m_discardOnClose && m_item.valid()) {
      struct stat current{};
      if (::fstatat(fd(), "item", &current, AT_SYMLINK_NOFOLLOW) == 0) {
        removeItem();
      } else if (errno != ENOENT) {
        return false;
      }
    }
    return ::unlinkat(m_parent.get(), m_name.c_str(), AT_REMOVEDIR) == 0;
  } catch (const FileErrorException&) {
    return false;
  }
}

std::string luciole::CreationStage::recoveryPath() const {
  const auto current = namedDirectory(fd());
  if (!current.empty()) {
    return current + "/item";
  }
  // Detached mounts can produce root-relative procfs names. Keep the last verified creation path instead.
  return !m_originalPath.empty() ? m_originalPath + "/item"
                                 : "/proc/" + std::to_string(::getpid()) + "/fd/" + std::to_string(fd()) + "/item";
}

void luciole::copyCreationData(int source, int output, std::uint64_t size, const CancelToken& token) {
  std::array<char, 65536> buffer{};
  std::uint64_t offset = 0;
  while (offset < size) {
    if (token.cancelled()) {
      throwFileError(FileErrorKind::Cancelled, "Creation cancelled.");
    }
    const auto count = ::pread(
        source, buffer.data(), std::min<std::uint64_t>(buffer.size(), size - offset), static_cast<off_t>(offset)
    );
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count == 0) {
      throwFileError(FileErrorKind::Changed, "The creation source changed while copying.");
    }
    if (count < 0) {
      throwErrno(errno);
    }
    std::size_t written = 0;
    while (written < static_cast<std::size_t>(count)) {
      if (token.cancelled()) {
        throwFileError(FileErrorKind::Cancelled, "Creation cancelled.");
      }
      const auto amount = ::write(output, buffer.data() + written, static_cast<std::size_t>(count) - written);
      if (amount < 0 && errno == EINTR) {
        continue;
      }
      if (amount <= 0) {
        throwErrno(amount < 0 ? errno : EIO);
      }
      written += static_cast<std::size_t>(amount);
    }
    offset += static_cast<std::uint64_t>(count);
    noteWorkerIoProgress();
  }
}
