#include "browser/chooser_policy.h"

#include <algorithm>
#include <format>
#include <limits>
#include <sys/stat.h>

namespace {
  using luciole::FileType;

  std::size_t trailingExtensionLength(std::string_view name, std::span<const luciole::FileFilter> filters) {
    if (name.empty() || name == "." || name == "..") {
      return 0;
    }
    std::size_t best = 0;
    for (const auto& filter : filters) {
      best = std::max(best, filter.matchedExtensionLength(name));
    }
    if (best > 0) {
      return best;
    }
    return luciole::knownExtensionLength(name);
  }

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
  // As GTK's local-only chooser: remote GVfs locations count when gvfsd-fuse gives them a path.
  if (!folder || !folder->nativePath() || std::ranges::any_of(entries, [](const DirectoryEntry& entry) {
        return !entry.location().nativePath();
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

std::size_t luciole::chooserStemLength(std::string_view name, std::span<const FileFilter> filters) {
  if (const std::size_t extLen = trailingExtensionLength(name, filters); extLen > 0) {
    return name.size() - extLen;
  }
  const auto dot = name.rfind('.');
  return dot != std::string_view::npos && dot > 0 && dot + 1 < name.size() ? dot : name.size();
}

std::string luciole::applyChooserFilterExtension(
    std::string_view name, const FileFilter& active, std::span<const FileFilter> filters
) {
  if (name.empty() || name == "." || name == "..") {
    return std::string(name);
  }
  const std::string ext = active.primaryExtension();
  if (ext.empty() || (active.matchedExtensionLength(name) > 0 && active.matches(name, nullptr))) {
    return std::string(name);
  }
  const std::size_t strip = name.ends_with('.') ? 1 : trailingExtensionLength(name, filters);
  return std::format("{}{}", name.substr(0, name.size() - strip), ext);
}

std::string luciole::completeChooserSaveName(std::string_view name, const FileFilter& active) {
  if (name.empty() || name == "." || name == ".." || (!name.ends_with('.') && trailingExtensionLength(name, {}) > 0)) {
    return std::string(name);
  }
  const std::string ext = active.primaryExtension();
  if (ext.empty()) {
    return std::string(name);
  }
  const std::string_view stem = name.ends_with('.') ? name.substr(0, name.size() - 1) : name;
  return std::format("{}{}", stem, ext);
}

std::optional<std::size_t> luciole::matchingChooserFilter(
    std::string_view name, std::span<const FileFilter> filters, std::size_t active, bool allowWildcard
) {
  if (trailingExtensionLength(name, filters) == 0) {
    return std::nullopt;
  }
  if (active < filters.size()
      && filters[active].matches(name, nullptr)
      && !filters[active].primaryExtension().empty()) {
    return active;
  }
  std::optional<std::size_t> bestSpecific;
  std::optional<std::size_t> firstWildcard;
  std::size_t bestPatternCount = std::numeric_limits<std::size_t>::max();
  for (std::size_t i = 0; i < filters.size(); ++i) {
    if (!filters[i].matches(name, nullptr)) {
      continue;
    }
    if (!filters[i].primaryExtension().empty()) {
      if (const std::size_t count = filters[i].patternCount(); count < bestPatternCount) {
        bestPatternCount = count;
        bestSpecific = i;
      }
    } else if (allowWildcard && !firstWildcard) {
      firstWildcard = i;
    }
  }
  return bestSpecific ? bestSpecific : firstWildcard;
}
