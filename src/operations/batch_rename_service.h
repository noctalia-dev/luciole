#pragma once

#include "fs/directory_session.h"
#include "fs/file_error.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  struct BatchRenameReceipt;
  class HistoryMount;
  bool batchRenameUsesMount(const BatchRenameReceipt& receipt, const HistoryMount& mount);
  enum class BatchRenameMode { Numbering, Replace };
  struct BatchRenameOptions {
    BatchRenameMode mode = BatchRenameMode::Numbering;
    std::string pattern = "{name}_{number}", find, replacement;
    int start = 1, digits = 3;
    bool keepExtensions = true;
  };
  enum class BatchRenamePhase { Idle, Preparing, Previewing, Ready, Applying, Finished, Error, Cancelled };
  enum class BatchRenameOutcome { Renamed, Unchanged, Failed, Cancelled, NeedsAttention, Unconfirmed };
  struct BatchRenameRow {
    std::string before, after, name;
    std::string error; // Preview validation or the applied result's message.
    bool changed = false;
  };
  struct BatchRenameResult {
    Location source;
    std::optional<Location> actual;
    BatchRenameOutcome outcome;
    std::optional<FileError> error;
  };
  struct BatchRenameSnapshot {
    BatchRenamePhase phase = BatchRenamePhase::Idle;
    std::shared_ptr<const std::vector<BatchRenameRow>> rows;
    std::shared_ptr<const std::vector<BatchRenameResult>> results;
    std::optional<Location> parent;
    std::optional<FileError> error;
    std::string report;
    bool canApply = false;
    std::shared_ptr<const BatchRenameReceipt> receipt;
  };
  // One native containing folder, up to 4096 captured entries, in their displayed selection order.
  class BatchRenameService {
  public:
    BatchRenameService();
    ~BatchRenameService();
    BatchRenameService(const BatchRenameService&) = delete;
    BatchRenameService& operator=(const BatchRenameService&) = delete;
    bool prepare(std::vector<DirectoryEntry> entries);
    bool preview(BatchRenameOptions options);
    bool apply();
    bool replay(std::shared_ptr<const BatchRenameReceipt> receipt);
    void cancel();
    bool busy() const;
    bool applying() const;
    std::size_t completed() const;
    const BatchRenameSnapshot& snapshot() const;
    // Hand optional history to its application owner without retaining descriptors in the report.
    std::shared_ptr<const BatchRenameReceipt> takeReceipt();
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
  std::string
  batchRenameName(const std::string& name, bool directory, std::size_t index, const BatchRenameOptions& options);
} // namespace luciole
