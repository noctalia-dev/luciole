#pragma once

#include "fs/file_error.h"
#include "fs/location.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  class LocationMount;
  struct DriveUsage {
    std::uint64_t used = 0;
    std::uint64_t size = 0;
    bool operator==(const DriveUsage&) const = default;
  };

  // How a mounted drive is removed, chosen as GNOME Files does: power off a hot-plugged drive that supports it,
  // else eject removable media, else unmount.
  enum class DriveRemoval : std::uint8_t { None, Unmount, Eject, PowerOff };

  // A process GIO reports as keeping a drive busy.
  struct DriveBlocker {
    int pid = 0;
    std::string name;
  };

  struct DriveRemoveResult {
    // Empty on success.
    std::string error;
    // Set when open files kept the drive busy; nothing was unmounted then.
    bool busy = false;
    std::vector<DriveBlocker> blockers;
  };

  // The root filesystem ("/" as File System), then each mountable volume or mount reported by GIO's volume
  // monitor (removable media, extra partitions, network mounts), as Nautilus lists them under "Other Locations".
  struct Drive {
    // Stable while the device stays attached; identifies the drive across change notifications.
    std::string key;
    std::string label;
    std::string glyph;
    // Set while mounted.
    std::optional<Location> root;
    // Set while mounted and GIO allows removing the drive.
    DriveRemoval removal = DriveRemoval::None;
    // Filesystem usage of the mounted root; null until queried, when unmounted, or when the size is unknown.
    std::optional<DriveUsage> usage;
    // A system-owned automount can be opened while its filesystem is idle. This is not a mounted root.
    std::optional<Location> activation = std::nullopt;
    // The GIO icon name, without "-symbolic", when it names no standard device kind. A udev rule can set it
    // through ID_MEDIA_PLAYER_ICON_NAME; the UI shows the glyph of that name, if any, in place of `glyph`.
    std::string iconName;
  };

  // The glyph for a freedesktop device icon name such as "phone" or "camera-photo", tried with trailing
  // "-part"s removed so "phone-samsung-galaxy-s" maps as "phone". Empty for any other name.
  std::string_view deviceGlyph(std::string_view iconName);

  // Main-context view of GIO's volume monitor. Callbacks run on the main context and must not throw.
  class DriveMonitor {
  public:
    DriveMonitor();
    ~DriveMonitor();
    DriveMonitor(const DriveMonitor&) = delete;
    DriveMonitor& operator=(const DriveMonitor&) = delete;

    // Starts GIO's volume monitor; drives() holds only the root filesystem before. Blocks while GIO starts its
    // monitor services, so the application calls it after its first frame. Idempotent.
    void start();

    // The root filesystem, then mounted drives, then unmounted ones.
    const std::vector<Drive>& drives() const;
    const Drive* find(const std::string& key) const;
    void setChangedCallback(std::function<void()> callback);
    // Re-queries usage of every mounted drive asynchronously; a drive with a query in flight is skipped.
    void refreshUsage();
    // Runs on the main context when a drive's usage changes. Must not throw.
    void setUsageChangedCallback(std::function<void()> callback);
    // Mounts the drive if needed; done receives the root, or an error message.
    void
    mount(const std::string& key, LocationMount& request, std::function<void(std::expected<Location, FileError>)> done);
    // Removes the drive as its `removal` says, without forcing: when files on it are open, nothing is unmounted
    // and the result lists the processes GIO found.
    void remove(const std::string& key, std::function<void(DriveRemoveResult)> done);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
