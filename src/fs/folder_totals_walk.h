#pragma once

#include "fs/cancel_token.h"
#include "fs/folder_totals.h"

namespace luciole {
  struct FolderTotalsLimits {
    // Items bound the runaway walk, not ordinary disks: counting is one stat
    // per entry and off-thread, so even millions of files complete. Depth
    // still guards pathological nesting.
    std::size_t items = 50000000, depth = 128;
  };
  // Worker entry point. Progress is a bounded value, never a retained per-item result list.
  FolderTotalsSnapshot walkFolderTotals(
      std::vector<DirectoryEntry> roots, const CancelToken& token,
      const std::function<void(const FolderTotalsSnapshot&)>& progress, FolderTotalsLimits limits = {}
  );
} // namespace luciole
