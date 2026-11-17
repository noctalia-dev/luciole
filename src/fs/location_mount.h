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
    void cancel();
    void setChangedCallback(std::function<void()> callback);
    MountInteraction& interaction();

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
