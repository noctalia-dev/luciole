#pragma once

#include "fs/cancel_token.h"
#include "fs/folder_totals.h"
#include "fs/network_process.h"

namespace luciole {
  struct FolderTotalsLimits {
    std::size_t items = 1000000, depth = 128;
  };
  NetworkMessage networkFolderTotals(const NetworkMessage& request, const NetworkEvent& event);
  // Worker entry point. Progress is a bounded value, never a retained per-item result list.
  FolderTotalsSnapshot walkFolderTotals(
      std::vector<DirectoryEntry> roots, const CancelToken& token,
      const std::function<void(const FolderTotalsSnapshot&)>& progress, FolderTotalsLimits limits = {}
  );
} // namespace luciole
