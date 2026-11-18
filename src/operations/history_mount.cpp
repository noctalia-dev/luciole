#include "operations/history_mount.h"

#include <fcntl.h>
#include <utility>

luciole::HistoryMount::HistoryMount(Location root) : m_root(std::move(root)) {
  if (m_root.isNative()) {
    m_directory = checkedFd(::open(m_root.nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    // Refuse an unverifiable boundary even when the current history has no native descriptors.
    (void)sameMount(m_directory.get(), m_directory.get());
  }
}

bool luciole::HistoryMount::contains(int fd) const {
  return fd >= 0 && m_directory.valid() && sameMount(fd, m_directory.get());
}

bool luciole::HistoryMount::contains(const Location& location) const {
  return location == m_root || location.uri().starts_with(m_root.uri() + (m_root.uri().ends_with('/') ? "" : "/"));
}
