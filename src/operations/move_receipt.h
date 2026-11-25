#pragma once

#include "fs/cancel_token.h"
#include "fs/posix_fd.h"
#include "fs/transfer_types.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace luciole {
  struct MoveIdentity;
  struct MoveTrees;
  struct MoveHistoryRoot {
    Location location;
    Fd parent;
  };
  class CopyEffect;
  class HistoryRevisions;
  class HistoryMount;
  using MoveParents = std::map<std::string, int>;
  class MoveReceipt;
  struct MoveReplayResult {
    std::vector<TransferResult> results;
    std::shared_ptr<const MoveReceipt> receipt;
    bool retireHistory = false;
  };
  // One removed merge container, retained empty until Undo restores its original inode.
  class MoveFolder {
  public:
    static std::shared_ptr<const MoveFolder> retain(
        const Location& source, int from, const Location& folder, int target, const CancelToken& token,
        std::shared_ptr<HistoryRevisions> revisions = {}, const std::shared_ptr<const MoveHistoryRoot>& root = {}
    );
    std::vector<Location> retainForRecovery() const;
    bool usesMount(const HistoryMount& mount) const;

  private:
    friend class MoveReceipt;
    friend class CopiedMoveReceipt;
    MoveFolder(Location sourceValue, Location folderValue)
        : m_source(std::move(sourceValue)), m_retainingFolder(std::move(folderValue)) {}
    Location m_source, m_retainingFolder;
    std::shared_ptr<const CopyEffect> m_item;
    std::shared_ptr<const Fd> m_from, m_target;
  };
  // Proof of one committed native rename, optionally paired with its displaced destination.
  class MoveEffect {
  public:
    static std::shared_ptr<const MoveEffect> capture(
        const Location& source, const Location& destination, int from, int to, int item,
        std::shared_ptr<HistoryRevisions> revisions = {}, std::shared_ptr<const MoveHistoryRoot> root = {}
    );
    static std::shared_ptr<const MoveEffect> prepareReplacement(
        const Location& source, const Location& destination, int from, int to, std::size_t& remaining,
        const CancelToken& token, std::shared_ptr<HistoryRevisions> revisions = {},
        std::shared_ptr<const MoveHistoryRoot> root = {}
    );
    std::shared_ptr<const MoveEffect> publishedReplacement() const;
    std::shared_ptr<const MoveEffect>
    captureContents(std::size_t& remaining, const CancelToken& token, const MoveParents& parents = {}) const;
    bool encloses(const Location& location) const;
    bool hasContents() const { return m_trees != nullptr; }
    const Location& destination() const { return m_destination; }
    bool usesMount(const HistoryMount& mount) const;
    const std::shared_ptr<const MoveHistoryRoot>& retentionRoot() const;
    std::vector<Location> retainForRecovery() const;

  private:
    friend class MoveReceipt;
    MoveEffect(
        Location source, Location destination, std::shared_ptr<const MoveIdentity> identity, struct stat revision,
        bool reversed, std::shared_ptr<const CopyEffect> previous = {}, std::shared_ptr<const CopyEffect> retained = {},
        std::shared_ptr<const MoveTrees> trees = {}, std::shared_ptr<const MoveTrees> previousTrees = {}
    );
    int retentionParent() const;
    int sourceParent() const;
    int destinationParent() const;
    void validate(
        const struct stat& revision, const MoveParents& parents = {}, bool moving = false, const CancelToken& token = {}
    ) const;
    void requireVacant(const MoveParents& parents = {}) const;
    Location m_source, m_destination;
    std::shared_ptr<const MoveIdentity> m_identity;
    struct stat m_revision;
    bool m_reversed;
    std::shared_ptr<const CopyEffect> m_previous, m_retained;
    std::shared_ptr<const MoveTrees> m_trees, m_previousTrees;
  };
  // One application-wide Move record; the default implementation retains native rename/merge identities.
  class MoveReceipt {
  public:
    virtual ~MoveReceipt() = default;
    // Returns no history when discarding overwritten data prevents complete source restoration.
    static std::shared_ptr<const MoveReceipt> withoutPredecessors(std::shared_ptr<const MoveReceipt> receipt);
    using Revisions = std::map<std::pair<dev_t, ino_t>, struct stat>;
    static std::shared_ptr<const MoveReceipt> capture(
        const std::vector<std::shared_ptr<const MoveEffect>>& effects,
        const std::vector<std::shared_ptr<const MoveFolder>>& folders = {}, bool undone = false, bool sequence = false,
        const CancelToken& token = {}, const std::vector<std::shared_ptr<const CopyEffect>>& copies = {}
    );
    static std::vector<std::shared_ptr<const MoveEffect>> checkpoint(
        const std::vector<std::shared_ptr<const MoveEffect>>& effects, const Location& destination,
        std::size_t& remaining, const CancelToken& token, const std::vector<std::shared_ptr<const MoveFolder>>& folders
    );
    virtual MoveReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const;
    virtual std::vector<TransferResult> unconfirmed(const FileError& error) const;
    virtual std::size_t size() const { return m_effects.size() + m_folders.size(); }
    virtual bool usesMount(const HistoryMount& mount) const;
    static bool cleanupPending();

  private:
    friend class CopiedMoveReceipt;
    // Compose native and copied phases without exposing independently replayable component receipts.
    struct ReplayHooks {
      std::function<void(const MoveParents&)> preflight;
      std::function<void(bool&)> beforeMoves;
      std::function<void(bool&)> apply;
    };
    MoveReplayResult
    reverseImpl(const CancelToken& token, std::atomic<std::size_t>& completed, const ReplayHooks* hooks) const;
    static std::vector<std::shared_ptr<const MoveEffect>>
    coalesce(const std::vector<std::shared_ptr<const MoveEffect>>& effects);
    static std::vector<std::shared_ptr<const MoveEffect>> compose(
        const std::vector<std::shared_ptr<const MoveEffect>>& effects, bool refresh = false,
        const CancelToken& token = {}, const std::vector<std::shared_ptr<const CopyEffect>>& copies = {}
    );
    static bool needsSequence(const std::vector<std::shared_ptr<const MoveEffect>>& effects, const CancelToken& token);
    static std::vector<std::shared_ptr<const MoveEffect>>
    prepareSequence(const std::vector<std::shared_ptr<const MoveEffect>>& effects, const CancelToken& token);
    MoveParents sequenceParents(const MoveEffect& effect, const CancelToken& token) const;
    void validateSequence(const CancelToken& token) const;
    std::shared_ptr<const MoveEffect>
    replayEffect(const std::shared_ptr<const MoveEffect>& effect, const CancelToken& token) const;
    std::shared_ptr<const CopyEffect> sequencePrevious(
        const MoveEffect& effect, bool before, const CancelToken& token, const MoveParents& bindings
    ) const;
    MoveParents parents(const CancelToken& token = {}, const std::vector<std::pair<Location, int>>& extra = {}) const;
    void validateFolders(const MoveParents& bindings, const CancelToken& token) const;
    bool restoreFolders(
        std::vector<std::shared_ptr<const MoveFolder>>& folders, const CancelToken& token, MoveReplayResult& reply,
        std::atomic<std::size_t>& completed, bool& applied
    ) const;
    bool ownsProof() const;
    void retainIncomplete(std::vector<TransferResult>& results) const;
    std::vector<std::shared_ptr<const MoveEffect>> m_effects;
    std::vector<std::shared_ptr<const MoveFolder>> m_folders;
    bool m_undone = false, m_sequence = false;
    std::shared_ptr<HistoryRevisions> m_revisions;
    std::uint64_t m_generation = 0;
    std::shared_ptr<std::mutex> m_mutex = std::make_shared<std::mutex>();
  };
  // Optional proof retention must leave descriptor headroom for the transfer itself.
  std::size_t transferHistoryBudget(std::size_t descriptorsPerRoot);
} // namespace luciole
