#include "browser/chooser_policy.h"

#include <algorithm>
#include <sys/stat.h>

namespace {
  using luciole::FileType;

  luciole::ChooserDecision unavailable(std::string_view reason) {
    return {.kind = luciole::ChooserDecision::Kind::Unavailable, .locations = {}, .reason = reason};
  }

  luciole::ChooserDecision accept(std::span<const luciole::DirectoryEntry> entries) {
    luciole::ChooserDecision decision{.kind = luciole::ChooserDecision::Kind::Accept, .locations = {}, .reason = {}};
    decision.locations.reserve(entries.size());
    for (const auto& entry : entries) {
      decision.locations.push_back(entry.location());
    }
    return decision;
  }
} // namespace

bool luciole::isLink(const DirectoryEntry& entry) { return entry.symlink || entry.type == FileType::Symlink; }

// Listings report links without following them, so a link counts as its native target's type.
luciole::FileType luciole::chosenType(const DirectoryEntry& entry) {
  if (!isLink(entry)) {
    return entry.type;
  }
  const auto path = entry.location().nativePath();
  struct stat info{};
  if (!path || ::stat(path->c_str(), &info) != 0) {
    return FileType::Symlink;
  }
  if (S_ISDIR(info.st_mode)) {
    return FileType::Directory;
  }
  return S_ISREG(info.st_mode) ? FileType::Regular : FileType::Special;
}

luciole::ChooserDecision luciole::chooserDecision(
    ChooserMode mode, const std::optional<Location>& folder, std::span<const DirectoryEntry> entries
) {
  if (!folder || !folder->isNative() || std::ranges::any_of(entries, [](const DirectoryEntry& entry) {
        return !entry.location().isNative();
      })) {
    return unavailable("Only local files can be chosen.");
  }
  if (std::ranges::any_of(entries, &DirectoryEntry::metadataPending)) {
    return unavailable("Reading item metadata…");
  }
  std::vector<FileType> types;
  types.reserve(entries.size());
  for (const auto& entry : entries) {
    types.push_back(entry.type);
  }
  const auto every = [&](FileType type) {
    return std::ranges::all_of(types, [type](FileType t) { return t == type; });
  };
  if (!mode.directory) {
    if (entries.empty()) {
      return unavailable({});
    }
    if (entries.size() == 1 && types.front() == FileType::Directory) {
      return {.kind = ChooserDecision::Kind::Enter, .locations = {entries.front().location()}, .reason = {}};
    }
    if (every(FileType::Regular) && (mode.multiple || entries.size() == 1)) {
      return accept(entries);
    }
    if (entries.size() > 1 && !mode.multiple) {
      return unavailable("Choose one file.");
    }
    return unavailable("Only files can be chosen.");
  }
  if (entries.empty()) {
    return {.kind = ChooserDecision::Kind::Accept, .locations = {*folder}, .reason = {}};
  }
  if (every(FileType::Directory) && (mode.multiple || entries.size() == 1)) {
    return accept(entries);
  }
  return unavailable("Only folders can be chosen.");
}

bool luciole::offersSaveName(FileType chosen) { return chosen != FileType::Directory; }

std::vector<luciole::DirectoryEntry> luciole::chooserSelection(const DirectoryModel& model) {
  std::vector<DirectoryEntry> entries;
  for (const auto id : model.selectedIds()) {
    if (const auto* entry = model.entry(id)) {
      entries.push_back(*entry);
    }
  }
  return entries;
}

std::string_view luciole::fileNameProblem(std::string_view name) {
  if (name.empty()) {
    return "Enter a file name.";
  }
  if (name == "." || name == "..") {
    return "Choose another name.";
  }
  if (name.contains('/') || name.contains('\0')) {
    return "A file name cannot contain “/”.";
  }
  return {};
}
