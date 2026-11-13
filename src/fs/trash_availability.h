#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace luciole {
  // On-demand provider count, independent of the folder currently being browsed.
  class TrashAvailability {
  public:
    TrashAvailability();
    ~TrashAvailability();
    TrashAvailability(const TrashAvailability&) = delete;
    TrashAvailability& operator=(const TrashAvailability&) = delete;
    // Replies on the calling main context; nullopt means unavailable/unsupported.
    // Replacing, cancelling or destroying the request suppresses its callback.
    void query(std::function<void(std::optional<std::uint32_t>)> completed);
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
