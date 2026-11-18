#pragma once

#include "fs/cancel_token.h"
#include "fs/location.h"

#include <memory>

namespace luciole {
  struct RenameIdentity;
  class HistoryMount;
  // Immutable proof of one completed rename. Worker-only capture/reversal; no traversal or overwrite.
  class RenameReceipt {
  public:
    RenameReceipt(Location source, Location destination, std::shared_ptr<const RenameIdentity> identity);
    const Location& source() const { return m_source; }
    const Location& destination() const { return m_destination; }
    static std::shared_ptr<const RenameReceipt>
    capture(const Location& source, const Location& destination, int parent, int item);
    // Returns the inverse receipt. A null result means the rename committed but could not arm another reversal.
    std::shared_ptr<const RenameReceipt> reverse(const CancelToken& token) const;
    bool usesMount(const HistoryMount& mount) const;

  private:
    Location m_source, m_destination;
    std::shared_ptr<const RenameIdentity> m_identity;
  };
} // namespace luciole
