#pragma once

#include "fs/location.h"
#include "fs/posix_fd.h"

namespace luciole {
  // Worker-only mount attribution. Native proofs use the held descriptor's mount ID, including bind mounts.
  // Provider proofs contain locations rather than native descriptors.
  class HistoryMount {
  public:
    explicit HistoryMount(Location root);
    HistoryMount(Location root, Fd directory) : m_root(std::move(root)), m_directory(std::move(directory)) {}
    const Location& root() const { return m_root; }
    int descriptor() const { return m_directory.get(); }
    bool contains(int fd) const;
    bool contains(const Location& location) const;

  private:
    Location m_root;
    Fd m_directory;
  };
} // namespace luciole
