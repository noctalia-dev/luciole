#pragma once

#include "fs/cancel_token.h"
#include "fs/folder_totals.h"

namespace luciole {
  struct FolderTotalsLimits {
    std::size_t items = 1000000, depth = 128;
    // Same-device scope: the walk never descends into a child filesystem; such children count as unsupported.
    bool sameDevice = false;
  };
  // Worker entry point. Progress is a bounded value, never a retained per-item result list.
  FolderTotalsSnapshot walkFolderTotals(
      std::vector<DirectoryEntry> roots, const CancelToken& token,
      const std::function<void(const FolderTotalsSnapshot&)>& progress, FolderTotalsLimits limits = {}
  );
} // namespace luciole
