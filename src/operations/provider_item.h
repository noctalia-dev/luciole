#pragma once

#include "fs/cancel_token.h"
#include "fs/gio_interop.h"
#include "fs/location.h"

#include <functional>

namespace luciole {
  // Worker-only revision checks for administrator operations. These are ordinary provider revalidation,
  // not descriptor-relative protection against hostile concurrent path changes.
  class ProviderItem {
  public:
    ProviderItem(Location location, const CancelToken& token, bool follow = false);
    const Location& location() const { return m_location; }
    bool directory() const;
    void requireCurrent(const CancelToken& token, bool content = true) const;

  private:
    Location m_location;
    GioPtr<GFileInfo> m_info;
    bool m_follow;
  };

  // No overwrite. File data stays private until write and close have succeeded.
  Location createProviderItem(
      const ProviderItem& parent, const std::string& name, bool folder, const CancelToken& token,
      const std::function<void(GOutputStream*)>& write
  );
  Location renameProviderItem(const ProviderItem& source, const std::string& name, const CancelToken& token);
  // Captures and revalidates a non-following tree before deletion. Cancellation leaves completed effects intact.
  void deleteProviderTree(const ProviderItem& root, const CancelToken& token, bool& changed);
} // namespace luciole
