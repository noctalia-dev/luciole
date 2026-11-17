#pragma once

#include "fs/gio_ptr.h"

#include <cstdint>
#include <functional>
#include <gio/gio.h>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  struct MountPrompt {
    std::uint64_t id;
    std::string message, user, domain;
    GAskPasswordFlags flags{};
    std::vector<std::string> choices;
    bool password = true;
  };
  struct MountCredentials {
    std::string user, domain, password;
    bool anonymous = false;
  };
  // Native GIO interaction bridge. Consumers render prompts with their own controls.
  class MountInteraction {
  public:
    MountInteraction();
    ~MountInteraction();
    MountInteraction(const MountInteraction&) = delete;
    MountInteraction& operator=(const MountInteraction&) = delete;
    GMountOperation* operation() const;
    const std::optional<MountPrompt>& prompt() const;
    const std::string& problem() const;
    bool cancelled() const;
    void setChangedCallback(std::function<void()> callback);
    bool answer(std::uint64_t id, const MountCredentials& credentials);
    bool choose(std::uint64_t id, std::size_t choice);
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
