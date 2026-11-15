#pragma once

#include "fs/location.h"
#include "fs/posix_fd.h"
#include "fs/transfer_types.h"
#include "operations/transfer_copy.h"
#include "operations/transfer_service.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

// Staged publication of transfer items: each item is copied into a private folder
// beside its destination, verified, and only then renamed into place. Merges recurse
// into per-child commits.
namespace luciole {
  class CopyEffect;
  class MoveEffect;
} // namespace luciole
namespace luciole::transfer {
  struct Source {
    std::size_t index;
    Location location;
    Fd parent;
    std::string name;
    struct stat metadata;
    std::string parentPath;
    struct stat parentMetadata;
  };

  struct Destination {
    Location location;
    struct stat metadata;
    std::shared_ptr<const Destination> ancestor;
  };

  struct Node;

  // A moved item whose copy is published but whose source waits for the next durability flush.
  struct PendingMove {
    Node* node;
    Tree tree;
    Fd destination;
    // The node's outcome once its source is removed.
    TransferOutcome outcome = TransferOutcome::Moved;
  };
  // Cross-filesystem moves sync the destination filesystem once per this many items, not once per item.
  constexpr std::size_t kMoveSyncBatch = 64;

  struct Work : CopyWork {
    // Asks how to resolve a name conflict; forcePrompt bypasses remembered policies.
    using Choose = std::function<TransferConflictChoice(
        const Source& source, const Location& destination, const std::string& target, const struct stat& existing,
        bool forcePrompt
    )>;
    Work(CancelToken cancel, std::atomic<std::uint64_t>& copied, Choose ask)
        : CopyWork(std::move(cancel), copied), choose(std::move(ask)) {}
    Choose choose;
    // Optional successful same-filesystem rename capture. False stops further optional capture.
    std::function<bool(const Source&, const Location&, int, int)> recordMove;
    std::function<std::shared_ptr<const MoveEffect>(const Source&, const Location&, int)> prepareMove;
    std::function<void(const std::shared_ptr<const MoveEffect>&)> recordPreparedMove;
    std::function<bool(const Source&, const Location&, int)> recordMerge;
    std::function<void(const Location&)> prepareMerge;
    std::function<void(const Location&, bool)> checkpointCopy;
    std::function<std::shared_ptr<const CopyEffect>(std::size_t, const Location&, int, int, bool)> prepareCopy;
    std::function<void(const std::shared_ptr<const CopyEffect>&, int)> recordCopy;
    bool allowMerge = true;
    std::size_t mergedItems = 0;
    // Owned by flushMoves(); nodes outlive the job's final flush.
    std::vector<PendingMove> pendingMoves;
  };

  struct Node {
    Node(Source captured, std::shared_ptr<const Destination> target, TransferResult outcome, unsigned level = 0)
        : source(std::move(captured)), destination(std::move(target)), result(std::move(outcome)), depth(level) {}
    Source source;
    std::shared_ptr<const Destination> destination;
    TransferResult result;
    std::vector<std::shared_ptr<Node>> children;
    bool merging = false;
    unsigned depth = 0;
    bool attempted = false;
    std::shared_ptr<const Destination> mergeTarget;
  };

  // Private staging folder inside a destination; removed on destruction unless retained.
  class Stage {
  public:
    Stage(int parent, CopyWork& work);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    bool clear() noexcept;
    int fd() const { return m_fd.get(); }
    const std::string& name() const { return m_name; }
    void retain() { m_cleaned = true; }
    void keepContents() { m_keepContents = true; }

  private:
    int m_parent;
    CopyWork& m_work;
    std::string m_name;
    Fd m_fd;
    bool m_cleaned = false;
    bool m_keepContents = false;
  };

  Fd openSourceParent(const Source& source);
  // Runs node once, or on retry only its retryable parts.
  void execute(Node& node, TransferAction action, Work& work, bool retry);
  // Makes pending moved copies durable with one syncfs per destination filesystem, then removes their sources.
  // Every job flushes before it reports outcomes.
  void flushMoves(Work& work);
  std::size_t retryCount(const Node& node);
  TransferResult snapshot(const Node& node);
} // namespace luciole::transfer
