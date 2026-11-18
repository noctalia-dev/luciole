#pragma once

#include "fs/directory_session.h"
#include "fs/file_error.h"
#include "operations/transfer_service.h"

#include <optional>

namespace luciole {
  enum class RestoreOutcome { Restored, Skipped, Failed, Cancelled, Unconfirmed };
  struct RestoreResult {
    Location source;
    std::optional<Location> destination;
    std::string displayName;
    RestoreOutcome outcome = RestoreOutcome::Cancelled;
    std::optional<FileError> error;
  };
  class RestoreService {
  public:
    RestoreService();
    ~RestoreService();
    RestoreService(const RestoreService&) = delete;
    RestoreService& operator=(const RestoreService&) = delete;
    bool start(std::vector<DirectoryEntry> selection);
    // Moves top-level Trash items into a chosen native folder, retaining provider metadata cleanup.
    bool move(std::vector<Location> sources, const Location& destination);
    bool moving() const;
    void cancel();
    bool busy() const;
    std::size_t total() const;
    std::size_t completed() const;
    const std::vector<RestoreResult>& results() const;
    // Shared completion effects for pane refresh, cut-clipboard reconciliation and drop acknowledgement.
    std::vector<TransferResult> transferResults() const;
    std::optional<TransferConflict> conflict() const;
    bool resolve(std::uint64_t id, TransferConflictChoice choice);
    void setChangedCallback(std::function<void()> callback);

  private:
    bool startSelection(std::vector<DirectoryEntry> selection, std::optional<Location> destination);
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
