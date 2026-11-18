#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/location.h"

#include <memory>
#include <optional>

namespace luciole {
  enum class CreateKind;
  struct CreationIdentity;
  class HistoryMount;
  class CreateReceipt;
  struct CreateReplayResult {
    std::shared_ptr<const CreateReceipt> receipt;
    std::optional<FileError> error;
    bool applied = false, removed = false, retireHistory = false;
  };
  // Worker-only proof and original data for one native creation. No recursive removal or overwrite.
  class CreateReceipt {
  public:
    CreateReceipt(CreateKind kind, Location destination, std::shared_ptr<const CreationIdentity> identity);
    CreateKind kind() const { return m_kind; }
    const Location& destination() const { return m_destination; }
    static std::shared_ptr<const CreateReceipt>
    capture(CreateKind kind, const Location& destination, int parent, int item, bool removed = false);
    CreateReplayResult reverse(const CancelToken& token) const;
    bool usesMount(const HistoryMount& mount) const;
    static bool cleanupPending();

  private:
    CreateKind m_kind;
    Location m_destination;
    std::shared_ptr<const CreationIdentity> m_identity;
  };
} // namespace luciole
