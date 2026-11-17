#pragma once

#include "fs/file_error.h"
#include "fs/location.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  enum class TrashStatus { Idle, Preparing, Ready, Trashing, Finished, Error, Cancelled };
  enum class TrashOutcome { Trashed, Failed, Cancelled };
  struct TrashResult {
    Location source;
    TrashOutcome outcome = TrashOutcome::Cancelled;
    std::optional<FileError> error;
  };
  struct TrashSnapshot {
    TrashStatus status = TrashStatus::Idle;
    std::vector<TrashResult> results;
    std::optional<FileError> error;
  };

  // Preparation pins native entries and their containing folders before any move.
  class TrashService {
  public:
    TrashService();
    ~TrashService();
    TrashService(const TrashService&) = delete;
    TrashService& operator=(const TrashService&) = delete;
    bool prepare(const Location& parent, std::vector<Location> sources);
    // Native drag payloads can span containing folders; preparation pins each one before any move.
    bool prepare(std::vector<Location> sources);
    bool trash();
    void cancel();
    bool busy() const;
    bool canTrash() const;
    std::size_t completed() const;
    const TrashSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    bool prepareSources(const std::optional<Location>& parent, std::vector<Location> sources);
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
