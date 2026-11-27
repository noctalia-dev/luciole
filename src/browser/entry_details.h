#pragma once

#include "fs/directory_session.h"
#include "fs/folder_totals.h"
#include "model/directory_options.h"
#include "model/size_format.h"

#include <array>
#include <string>
#include <string_view>

namespace luciole {
  std::uint8_t entryColumnDetails(DetailsColumn column);
  std::string entryColumnText(
      const DirectoryEntry& entry, DetailsColumn column, std::string_view typeLabel = {}, const SizeUnits& units = {}
  );
  // Size cell text for a folder row; ≥ marks partial totals
  std::string entryFolderSizeText(const FolderTotalsSnapshot& totals, const SizeUnits& units = {});
  std::string metadataTime(std::optional<std::uint64_t> seconds);
  std::optional<std::uint64_t> entrySortNumber(const DirectoryEntry& entry, SortField field);
} // namespace luciole
