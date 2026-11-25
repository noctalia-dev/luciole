#pragma once

#include "browser/directory_model.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace luciole {
  struct ChooserMode {
    bool directory = false;
    bool multiple = false;
  };
  struct ChooserDecision {
    enum class Kind : std::uint8_t { Unavailable, Enter, Accept };
    Kind kind = Kind::Unavailable;
    std::vector<Location> locations;
    std::string_view reason; // empty when nothing needs explaining
  };
  // Decides based on each entry's resolved type; callers resolve selected links asynchronously via chosenType().
  ChooserDecision
  chooserDecision(ChooserMode mode, const std::optional<Location>& folder, std::span<const DirectoryEntry> entries);
  // Whether choosing `entry` depends on a link target that chosenType() has to read.
  bool isLink(const DirectoryEntry& entry);
  // The kind a chooser treats `entry` as: a link counts as its native target's kind, or stays a link when that cannot
  // be read. Following a link can block on a slow or hung mount.
  FileType chosenType(const DirectoryEntry& entry);
  // Whether selecting an item of the chosen kind while saving offers its name to save over; folders are entered.
  bool offersSaveName(FileType chosen);
  // The selected entries of `model`, in row order.
  std::vector<DirectoryEntry> chooserSelection(const DirectoryModel& model);
  // Why `name` (raw bytes) cannot name a new item in a folder; empty when it can.
  std::string_view fileNameProblem(std::string_view name);
} // namespace luciole
