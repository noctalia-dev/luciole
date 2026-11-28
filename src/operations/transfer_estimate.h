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

  // Local items, and items on a phone or camera; other providers move bytes nobody can measure up front.
  bool measurableSource(const Location& source);

  // Measures any GIO location by listing it: regular files and their bytes. Links, special files, unreadable folders,
  // cancellation and the provider transfer's limits leave a partial total.
  SourceEstimate estimateGioSource(
      const Location& source, const CancelToken& token, const std::function<void(const SourceEstimate&)>& publish
  );

  std::optional<SourceEstimate> estimateSource(
      const Location& source, const CancelToken& token, const std::function<void(const SourceEstimate&)>& publish
  );
} // namespace luciole
