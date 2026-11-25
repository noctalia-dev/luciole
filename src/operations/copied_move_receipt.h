#pragma once

#include "operations/copy_receipt.h"
#include "operations/move_receipt.h"

namespace luciole {
  // Native copy/remove Move history keeps verified destinations and copies back only on Undo.
  class CopiedMoveEffect {
  public:
    static std::shared_ptr<const CopiedMoveEffect> prepare(
        const Location& source, int from, const Location& destination, int target, int stage, bool replacing,
        std::size_t& remaining, const CancelToken& token, const std::shared_ptr<HistoryRevisions>& revisions,
        std::shared_ptr<const MoveHistoryRoot> root = {}
    );
    std::shared_ptr<const CopyEffect> destinationProof() const;
    void published(int stage) const;
    void beforeSourceRemoval(const CancelToken& token) const;
    void sourceRemoved(const CancelToken& token) const;
    std::vector<Location> retainForRecovery() const;

  private:
    friend class CopiedMoveReceipt;
    struct State;
    explicit CopiedMoveEffect(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    std::shared_ptr<State> m_state;
  };
  class CopiedMoveReceipt final : public MoveReceipt {
  public:
    std::shared_ptr<const MoveReceipt> withoutPredecessors() const;
    static std::shared_ptr<const MoveReceipt> capture(
        const std::vector<std::shared_ptr<const CopiedMoveEffect>>& effects, const CancelToken& token,
        std::shared_ptr<const MoveReceipt> native = {}
    );
    MoveReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const override;
    std::vector<TransferResult> unconfirmed(const FileError& error) const override;
    std::size_t size() const override;
    bool usesMount(const HistoryMount& mount) const override;

  private:
    struct State;
    CopiedMoveReceipt(std::shared_ptr<State> state, std::uint64_t generation, bool undone);
    static void preflight(const CopyReceipt& receipt, const CancelToken& token, const MoveParents& parents = {});
    MoveParents destinationParents(const CancelToken& token) const;
    void refreshDestinations(const CancelToken& token) const;
    void retain(std::vector<TransferResult>& values) const;
    void retainAll(std::vector<TransferResult>& values) const;
    std::vector<TransferResult>
    nativeResults(TransferOutcome outcome, const std::optional<FileError>& error = {}) const;
    void preflight(const CancelToken& token, const MoveParents& parents = {}) const;
    MoveReplayResult reverseCopied(const CancelToken& token, bool& applied, const MoveParents& parents = {}) const;
    void restoreSources(const CancelToken& token, MoveReplayResult& reply, bool& applied) const;
    void removeSources(const CancelToken& token, MoveReplayResult& reply, bool& applied) const;
    void reverseDestinations(const CancelToken& token, MoveReplayResult& reply, bool& applied) const;
    std::vector<TransferResult> results(TransferOutcome outcome, const std::optional<FileError>& error = {}) const;
    std::shared_ptr<State> m_state;
    std::uint64_t m_generation;
    bool m_undone;
  };
} // namespace luciole
