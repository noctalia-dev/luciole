#pragma once

#include "fs/system_mounts.h"

#include <functional>
#include <memory>

namespace luciole {
  // Main-context observer. Configuration and mount-table reads run on a worker without probing mount paths.
  class SystemMountMonitor {
  public:
    SystemMountMonitor();
    ~SystemMountMonitor();
    SystemMountMonitor(const SystemMountMonitor&) = delete;
    SystemMountMonitor& operator=(const SystemMountMonitor&) = delete;
    void start(std::function<void()> changed);
    const std::vector<SystemMount>& mounts() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
