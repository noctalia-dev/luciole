#pragma once

#include "fs/file_error.h"
#include "fs/location.h"
#include "fs/mount_interaction.h"

#include <expected>

namespace luciole {
  // One asynchronous enclosing-volume or mountable-item request for an explicit browsing context.
  class LocationMount {
  public:
    LocationMount();
    ~LocationMount();
    LocationMount(const LocationMount&) = delete;
    LocationMount& operator=(const LocationMount&) = delete;
    void start(
        const Location& location, std::function<void(std::expected<Location, FileError>)> done, bool mountable = false
    );
    // Unlocks and mounts a device through its GIO provider, with the same prompt/cancellation lifecycle.
    void startVolume(GVolume* volume, std::function<void(std::expected<Location, FileError>)> done);
    // Mounts a user-mountable fstab entry at its configured local path.
    void startFstabMount(const Location& target, std::function<void(std::expected<Location, FileError>)> done);
    void cancel();
    void setChangedCallback(std::function<void()> callback);
    MountInteraction& interaction();

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
