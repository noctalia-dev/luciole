#pragma once

#include "fs/cancel_token.h"
#include "fs/location.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace luciole {
  struct SourceEstimate {
    std::uint64_t bytes = 0;
    std::size_t files = 0;
    bool partial = false;
  };

  std::optional<SourceEstimate> estimateSource(
      const Location& source, const CancelToken& token, const std::function<void(const SourceEstimate&)>& publish
  );
} // namespace luciole
