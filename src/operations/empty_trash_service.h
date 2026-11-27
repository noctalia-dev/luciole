#pragma once

#include "fs/directory_session.h"
#include "fs/file_error.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  enum class EmptyTrashStatus { Idle, Preparing, Ready, Emptying, Finished, Error, Cancelled };
  enum class EmptyTrashOutcome { Deleted, Skipped, Failed, Cancelled, Unconfirmed };
  struct EmptyTrashResult {
    Location location;
    std::string displayName;
    EmptyTrashOutcome outcome = EmptyTrashOutcome::Cancelled;
    std::optional<FileError> error;
  };
  struct EmptyTrashSnapshot {
    EmptyTrashStatus status = EmptyTrashStatus::Idle;
    std::vector<EmptyTrashResult> results;
    std::optional<FileError> error;
    std::optional<std::uint64_t> size = std::nullopt;
    bool sizeIncomplete = false;
    bool selection = false;
    bool outsideTrash = false;
  };

  // Permanent deletion of selected native/administrator/Trash entries and confirmed Empty Trash.
  class EmptyTrashService {
  public:
    EmptyTrashService();
    ~EmptyTrashService();
    EmptyTrashService(const EmptyTrashService&) = delete;
    EmptyTrashService& operator=(const EmptyTrashService&) = delete;
    bool prepare();
    bool prepareSelection(std::vector<DirectoryEntry> selection);
    bool prepareDeletion(std::vector<DirectoryEntry> selection);
    // Deletes supported selections immediately after worker-side validation.
    bool deleteSelection(std::vector<DirectoryEntry> selection);
    bool empty();
    void cancel();
    bool busy() const;
    bool pending() const;
    bool canEmpty() const;
    std::size_t completed() const;
    const EmptyTrashSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    bool prepareTargets(
        std::optional<std::vector<DirectoryEntry>> selection, bool outsideTrash = false, bool immediate = false
    );
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
