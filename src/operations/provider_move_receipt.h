#pragma once

#include "operations/move_receipt.h"

namespace luciole {
  class ProviderCopyEffect;
  // Same-namespace SFTP Move history. Shares provider proof/storage with Copy, not native rename assumptions.
  class ProviderMoveReceipt final : public MoveReceipt {
  public:
    static std::shared_ptr<const MoveReceipt>
    capture(const std::vector<std::shared_ptr<const ProviderCopyEffect>>& effects, const CancelToken& token);
    MoveReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const override;
    std::vector<TransferResult> unconfirmed(const FileError& error) const override;
    std::size_t size() const override;
    bool usesMount(const HistoryMount& mount) const override;

  private:
    struct State;
    ProviderMoveReceipt(std::shared_ptr<State> state, std::uint64_t generation, bool atOrigin);
    std::vector<TransferResult> failureResults(const FileError& error) const;
    std::shared_ptr<State> m_state;
    std::uint64_t m_generation;
    bool m_atOrigin;
  };
} // namespace luciole
