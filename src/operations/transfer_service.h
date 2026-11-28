#pragma once

#include "fs/location.h"
#include "fs/transfer_types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace luciole {
  namespace transfer {
    struct WritebackOps;
  }
  class MoveReceipt;
  class CopyReceipt;
  enum class TransferConflictChoice { Replace, Skip, KeepBoth, Cancel, Merge };
  struct TransferConflict {
    std::uint64_t id;
    Location source;
    Location destination;
    std::string name;
    bool sourceDirectory;
    bool destinationDirectory;
    std::optional<std::uint64_t> sourceSize;
    std::optional<std::uint64_t> destinationSize;
    std::optional<std::uint64_t> sourceModified;
    std::optional<std::uint64_t> destinationModified;
    bool canMerge = false;
    bool canApplyToAll = false;
    std::string scopeLabel;
  };
  struct TransferRequest {
    std::vector<Location> sources;
    Location destination;
    TransferAction action = TransferAction::Move;
    bool allowMerge = true; // Archive publication keeps its separate all-or-nothing folder policy.
  };
  // Byte and item measurement of one running transfer, read on the main loop.
  struct TransferProgress {
    std::uint64_t bytes = 0;
    std::size_t completed = 0, total = 0;
    std::optional<std::uint64_t> bytesTotal, bytesPerSecond;
    std::optional<std::size_t> files, filesTotal;
    std::string current;
    bool estimating = false; // The source size scan still runs.
    bool partial = false;    // Totals are lower bounds of a bounded scan.
  };

  // One serial native-filesystem or verified-provider job. Workers never access browser models or UI.
  class TransferService {
  public:
    TransferService();
    explicit TransferService(transfer::WritebackOps ops);
    TransferFlush flushPhase() const;
    ~TransferService();
    TransferService(const TransferService&) = delete;
    TransferService& operator=(const TransferService&) = delete;
    // Application-owned jobs replay ordinary operations rather than retaining file bodies for Redo.
    void useOperationHistory();
    bool start(TransferRequest request);
    bool replay(const std::shared_ptr<const MoveReceipt>& receipt);
    bool replayCopy(const std::shared_ptr<const CopyReceipt>& receipt);
    void cancel();
    // Cancels worker work unconditionally during service or application teardown, including post-flush history capture.
    void shutdown();
    bool busy() const;
    // Duration of the attempt that just finished; unknown while running and for history loads.
    std::optional<std::chrono::milliseconds> elapsed() const;
    std::size_t total() const;
    std::size_t completed() const;
    std::uint64_t bytes() const;
    TransferProgress progress() const; // Byte, file and item measurement of the running transfer.
    TransferAction action() const;
    const std::vector<TransferResult>& results() const;
    std::optional<TransferConflict> conflict() const;
    bool resolve(std::uint64_t id, TransferConflictChoice choice, bool applyToAll = false);
    bool retryFailed();
    std::size_t retryable() const;
    const std::string& report() const;
    std::uint64_t jobId() const;
    std::shared_ptr<const MoveReceipt> receipt() const;
    // Hand optional history to its application owner without retaining it in finished job reports.
    std::shared_ptr<const MoveReceipt> takeReceipt();
    std::shared_ptr<const CopyReceipt> copyReceipt() const;
    std::shared_ptr<const CopyReceipt> takeCopyReceipt();
    bool historyRetired() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    bool launch(TransferRequest request, bool retry);
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
