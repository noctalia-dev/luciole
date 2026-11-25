#pragma once

#include "fs/cancel_token.h"
#include "fs/transfer_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace luciole {
  struct CopyStorage;
  class HistoryRevisions;
  class HistoryMount;
  namespace transfer {
    struct Tree;
  }
  class CopyReceipt;
  struct CopyReplayResult {
    std::vector<TransferResult> results;
    std::shared_ptr<const CopyReceipt> receipt;
    bool retireHistory = false;
  };
  // Worker-only proof captured from a private, unpublished copy, then checked after publication.
  class CopyEffect {
  public:
    static std::shared_ptr<const CopyEffect> prepare(
        const Location& destination, int parent, int stage, bool replacing, std::size_t& remaining,
        const CancelToken& token, std::shared_ptr<HistoryRevisions> revisions = {}
    );
    std::shared_ptr<const CopyEffect> published(int from, int retentionParent = -1) const;
    bool replacing() const { return m_previous != nullptr; }
    std::vector<Location> retainForRecovery() const;
    bool usesMount(const HistoryMount& mount) const;

  private:
    friend class CopiedMoveEffect;
    friend class CopiedMoveReceipt;
    friend class CopyReceipt;
    friend class MoveEffect;
    friend class MoveFolder;
    friend class MoveReceipt;
    int item() const;
    std::shared_ptr<HistoryRevisions> revisions() const;
    void verifyAt(const CopyEffect& actual, const CancelToken& token) const;
    Location currentLocation() const;
    int parent() const;
    std::mutex& mutex() const;
    std::shared_ptr<const CopyEffect> retainedFrom(int from, const std::string& name, int retentionParent = -1) const;
    std::shared_ptr<const CopyEffect> restoreTo(const Location& destination, int target, bool& changed) const;
    std::shared_ptr<const CopyEffect>
    storeTo(const Location& folder, int target, bool& changed, int retentionParent = -1) const;
    std::shared_ptr<const CopyEffect> refreshFolder(bool empty, std::size_t& remaining, const CancelToken& token) const;
    CopyEffect(
        Location destination, std::shared_ptr<CopyStorage> storage, std::shared_ptr<const transfer::Tree> tree,
        bool hidden, std::shared_ptr<const CopyEffect> previous = {}
    );
    static std::shared_ptr<const CopyEffect> prepareAt(
        const Location& destination, int parent, int from, const std::string& name, std::size_t& remaining,
        const CancelToken& token, std::shared_ptr<HistoryRevisions> revisions,
        const std::map<std::string, int>& parents = {}
    );
    std::shared_ptr<const CopyEffect> withTree(
        std::shared_ptr<const transfer::Tree> tree, const CancelToken& token, const std::map<std::string, int>& parents
    ) const;
    bool ownsProof() const;
    void validate(const CancelToken& token, const std::map<std::string, int>& parents = {}) const;
    void vacant() const;
    std::shared_ptr<const CopyEffect>
    afterMove(bool hidden, const std::map<std::string, int>& parents = {}, bool exchanged = false) const;
    Location m_destination;
    std::shared_ptr<CopyStorage> m_storage;
    std::shared_ptr<const transfer::Tree> m_tree;
    bool m_hidden;
    std::uint64_t m_generation;
    std::shared_ptr<const CopyEffect> m_previous;
  };
  // One bounded selection. Undo retains unchanged copies privately; Redo publishes the same items.
  class CopyReceipt {
  public:
    CopyReceipt() = default;
    virtual ~CopyReceipt() = default;
    static std::shared_ptr<const CopyReceipt> capture(
        const std::vector<std::shared_ptr<const CopyEffect>>& effects, TransferAction action, const CancelToken& token
    );
    // Application history reverses incoming effects without retaining overwritten originals.
    static std::shared_ptr<const CopyReceipt> withoutPredecessors(std::shared_ptr<const CopyReceipt> receipt);
    static std::vector<std::shared_ptr<const CopyEffect>> checkpoint(
        const std::vector<std::shared_ptr<const CopyEffect>>& effects, const Location& destination,
        TransferAction action, const CancelToken& token
    );
    virtual CopyReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const;
    virtual std::vector<TransferResult> unconfirmed(const FileError& error) const;
    virtual std::size_t size() const { return m_effects.size(); }
    virtual bool usesMount(const HistoryMount& mount) const;
    TransferAction action() const { return m_action; }
    static bool cleanupPending();
    static std::vector<FileError> takeCleanupErrors();

  protected:
    explicit CopyReceipt(TransferAction action) : m_action(action) {}

  private:
    friend class CopiedMoveReceipt;
    static std::shared_ptr<const CopyEffect> restoreParent(
        const std::shared_ptr<const CopyEffect>& effect, const std::vector<std::shared_ptr<const CopyEffect>>& children,
        const CancelToken& token
    );
    CopyReplayResult reverseImpl(
        const CancelToken& token, std::atomic<std::size_t>& completed, const std::map<std::string, int>& parents,
        int retentionParent, bool captureInverse
    ) const;
    void retainIncomplete(std::vector<TransferResult>& results) const;
    std::vector<std::shared_ptr<const CopyEffect>> m_effects;
    TransferAction m_action = TransferAction::Copy;
    std::shared_ptr<HistoryRevisions> m_revisions;
    std::uint64_t m_generation = 0;
  };
} // namespace luciole
