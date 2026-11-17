#pragma once

#include "fs/cancel_token.h"
#include "fs/ws_discovery_protocol.h"

#include <string>

namespace luciole::wsd {
  // Worker-only HTTP lookup. The caller supplies a validated, sender-pinned metadataTarget.
  std::string
  metadata(const MetadataTarget& target, const std::string& endpoint, bool modern, const CancelToken& token);
} // namespace luciole::wsd
