#pragma once

#include "fs/location.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  // One mountable volume or mount reported by GIO's volume monitor (removable media, extra partitions,
  // network mounts), as Nautilus lists them under "Other Locations".
  struct Drive {
    // Stable while the device stays attached; identifies the drive across change notifications.
    std::string key;
    std::string label;
    std::string glyph;
    // Set while mounted.
    std::optional<Location> root;
    // Unmount or eject is available (the drive is mounted and GIO allows it).
    bool removable = false;
    // Ejecting, not just unmounting, is what removal does (removable media).
    bool ejectable = false;
  };

  // Main-context view of GIO's volume monitor. Callbacks run on the main context and must not throw.
  class DriveMonitor {
  public:
    DriveMonitor();
    ~DriveMonitor();
    DriveMonitor(const DriveMonitor&) = delete;
    DriveMonitor& operator=(const DriveMonitor&) = delete;

    // Starts GIO's volume monitor; drives() stays empty before. Blocks while GIO starts its monitor services,
    // so the application calls it after its first frame. Idempotent.
    void start();

    const std::vector<Drive>& drives() const;
    const Drive* find(const std::string& key) const;
    void setChangedCallback(std::function<void()> callback);
    // Mounts the drive if needed; done receives the root, or an error message.
    void mount(const std::string& key, std::function<void(std::optional<Location>, std::string)> done);
    // Ejects removable media, otherwise unmounts; done receives an error message, empty on success.
    void remove(const std::string& key, std::function<void(std::string)> done);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
