#include "browser/directory_model.h"

#include "browser/entry_details.h"
#include "browser/location_presentation.h"
#include "browser/selection_tools.h"
#include "fs/file_visibility.h"
#include "fs/name_matching.h"

#include <algorithm>
#include <gio/gio.h>
#include <limits>
#include <utility>

namespace {
  bool needsTextSortKey(luciole::SortField field) {
    return field == luciole::SortField::Type
        || field == luciole::SortField::Owner
        || field == luciole::SortField::Group
        || field == luciole::SortField::Location;
  }

  std::string textSortKey(const std::string& text) {
    const luciole::GString collated(g_utf8_collate_key_for_filename(luciole::foldedDisplayText(text).c_str(), -1));
    return collated.get();
  }
} // namespace

bool luciole::DirectoryModel::applySnapshot(
    DirectorySnapshot snapshot, std::unordered_map<EntryId, EntryId> parents, bool preserveRootOrder
) {
  if (snapshot.generation < m_snapshot.generation) {
    return false;
  }
  if (snapshot.location != m_snapshot.location) {
    m_selection = {};
    m_scrollAnchor.reset();
    m_selectionAvailable = false;
  }
  if (snapshot.status == DirectoryStatus::Idle || snapshot.status == DirectoryStatus::Cancelled) {
    m_selectionAvailable = false;
  } else if (snapshot.status == DirectoryStatus::Loading && snapshot.incrementalResults) {
    m_selectionAvailable = !snapshot.entries.empty();
  } else if (snapshot.status == DirectoryStatus::Ready || snapshot.status == DirectoryStatus::Error) {
    m_selectionAvailable = true;
  }
  m_preserveRootOrder = preserveRootOrder;
  m_treeParents = std::move(parents);
  m_alternateSortKeys.clear();
  m_preparedSortField.reset();
  const bool sameLocation = snapshot.location == m_snapshot.location;
  m_presentations.reserve(snapshot.entries.size());
  std::unordered_map<std::string, std::string> typeLabels;
  for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
    const auto& entry = snapshot.entries[i];
    auto [found, inserted] = m_presentations.try_emplace(entry.id);
    auto& presentation = found->second;
    if (inserted || !sameLocation || m_snapshot.entries[presentation.sourceIndex].displayName != entry.displayName) {
      presentation.filterKey = foldedDisplayText(entry.displayName);
      const GString key(g_utf8_collate_key_for_filename(presentation.filterKey.c_str(), -1));
      presentation.sortKey = key.get();
    }
    presentation.sourceIndex = i;
    presentation.alternateSortKey = nullptr;
    const bool regular = entry.type == FileType::Regular && !entry.symlink;
    if (regular && entry.contentType) {
      auto [label, inserted] = typeLabels.try_emplace(*entry.contentType);
      if (inserted) {
        label->second = fileTypeDescription(entry.type, entry.symlink, entry.contentType);
      }
      presentation.typeLabel = label->second;
    } else {
      presentation.typeLabel = fileTypeDescription(entry.type, entry.symlink, entry.contentType);
    }
    presentation.knownType = entry.symlink
        || entry.type == FileType::Symlink
        || (regular ? entry.contentType && !g_content_type_is_unknown(entry.contentType->c_str())
                    : entry.type != FileType::Unknown);
  }
  // Surviving IDs now point into the incoming snapshot; remove rows that disappeared.
  std::erase_if(m_presentations, [&](const auto& item) {
    const auto index = item.second.sourceIndex;
    return index >= snapshot.entries.size() || snapshot.entries[index].id != item.first;
  });
  m_snapshot = std::move(snapshot);
  rebuildProjection();
  notify();
  return true;
}

void luciole::DirectoryModel::setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

void luciole::DirectoryModel::setSort(SortOptions options) {
  if (m_sort == options) {
    return;
  }
  m_sort = options;
  rebuildProjection();
  notify();
}

void luciole::DirectoryModel::setFilter(FilterOptions options) {
  if (m_filter == options) {
    return;
  }
  m_filterKey = foldedDisplayText(options.text);
  m_filter = std::move(options);
  rebuildProjection();
  notify();
}

const luciole::DirectoryEntry* luciole::DirectoryModel::entry(EntryId id) const {
  const auto it = m_presentations.find(id);
  return it == m_presentations.end() ? nullptr : &m_snapshot.entries[it->second.sourceIndex];
}

std::string_view luciole::DirectoryModel::typeLabel(EntryId id) const {
  const auto it = m_presentations.find(id);
  return it == m_presentations.end() ? std::string_view{} : it->second.typeLabel;
}

bool luciole::DirectoryModel::less(EntryId left, EntryId right) const {
  const auto& leftPresentation = m_presentations.at(left);
  const auto& rightPresentation = m_presentations.at(right);
  const auto& a = m_snapshot.entries[leftPresentation.sourceIndex];
  const auto& b = m_snapshot.entries[rightPresentation.sourceIndex];
  if (m_sort.directoriesFirst && (a.type == luciole::FileType::Directory) != (b.type == luciole::FileType::Directory)) {
    return a.type == luciole::FileType::Directory;
  }
  if (needsTextSortKey(m_sort.field)) {
    const auto* first = leftPresentation.alternateSortKey;
    const auto* second = rightPresentation.alternateSortKey;
    if ((first != nullptr) != (second != nullptr)) {
      return first != nullptr;
    }
    if (first && second && first != second && *first != *second) {
      return m_sort.descending ? *first > *second : *first < *second;
    }
  } else if (m_sort.field != SortField::Name) {
    const auto first = entrySortNumber(a, m_sort.field);
    const auto second = entrySortNumber(b, m_sort.field);
    // Unknown metadata stays last in both directions; zero is a real value.
    if (first.has_value() != second.has_value()) {
      return first.has_value();
    }
    if (first && first != second) {
      return m_sort.descending ? first > second : first < second;
    }
  }
  int comparison = leftPresentation.sortKey.compare(rightPresentation.sortKey);
  if (comparison == 0) {
    comparison = a.location.uri().compare(b.location.uri());
  }
  if (comparison == 0) {
    return left < right;
  }
  return m_sort.descending ? comparison > 0 : comparison < 0;
}

void luciole::DirectoryModel::prepareSortKeys() {
  if (!needsTextSortKey(m_sort.field) || m_preparedSortField == m_sort.field) {
    return;
  }
  m_alternateSortKeys.clear();
  for (auto& [id, presentation] : m_presentations) {
    (void)id;
    presentation.alternateSortKey = nullptr;
    const auto& entry = m_snapshot.entries[presentation.sourceIndex];
    if (m_sort.field == SortField::Location) {
      if (const auto parent = entry.location.parent()) {
        auto [key, inserted] = m_alternateSortKeys.try_emplace(parent->uri());
        if (inserted) {
          key->second = textSortKey(locationEditText(*parent));
        }
        presentation.alternateSortKey = &key->second;
      }
      continue;
    }
    const std::string* value = nullptr;
    if (m_sort.field == SortField::Type && presentation.knownType) {
      value = &presentation.typeLabel;
    } else if (m_sort.field == SortField::Owner && entry.details.owner) {
      value = &*entry.details.owner;
    } else if (m_sort.field == SortField::Group && entry.details.group) {
      value = &*entry.details.group;
    }
    if (value) {
      auto [key, inserted] = m_alternateSortKeys.try_emplace(*value);
      if (inserted) {
        key->second = textSortKey(*value);
      }
      presentation.alternateSortKey = &key->second;
    }
  }
  m_preparedSortField = m_sort.field;
}

void luciole::DirectoryModel::rebuildProjection() {
  prepareSortKeys();
  m_rows.clear();
  m_rows.reserve(m_snapshot.entries.size());
  for (const auto& entry : m_snapshot.entries) {
    if ((!entry.hidden || m_filter.showHidden)
        && (m_filter.showBackups || !isEditorBackup(entry.name, entry.type))
        && m_presentations.at(entry.id).filterKey.contains(m_filterKey)) {
      m_rows.push_back(entry.id);
    }
  }
  std::ranges::sort(m_rows, [this](EntryId left, EntryId right) { return less(left, right); });
  if (!m_treeParents.empty() || m_preserveRootOrder) {
    std::unordered_map<EntryId, std::vector<EntryId>> children;
    std::vector<EntryId> roots;
    for (const auto id : m_rows) {
      if (const auto parent = treeParent(id)) {
        children[*parent].push_back(id);
      } else {
        roots.push_back(id);
      }
    }
    if (m_preserveRootOrder) {
      std::ranges::sort(roots, {}, [&](EntryId id) { return m_presentations.at(id).sourceIndex; });
    }
    m_rows.clear();
    std::unordered_set<EntryId> visited;
    const auto append = [&](this auto&& self, EntryId id, std::size_t depth) -> void {
      if (depth > 32 || !visited.insert(id).second) {
        return;
      }
      m_rows.push_back(id);
      if (const auto found = children.find(id); found != children.end()) {
        for (const auto child : found->second) {
          self(child, depth + 1);
        }
      }
    };
    for (const auto id : roots) {
      append(id, 0);
    }
  }
  m_positions.clear();
  m_positions.reserve(m_rows.size());
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    m_positions.emplace(m_rows[i], i);
  }
  reconcileSelection();
}

std::optional<luciole::EntryId> luciole::DirectoryModel::treeParent(EntryId id) const {
  const auto found = m_treeParents.find(id);
  return found == m_treeParents.end() ? std::nullopt : std::optional(found->second);
}
std::size_t luciole::DirectoryModel::treeDepth(EntryId id) const {
  std::size_t depth = 0;
  while (const auto parent = treeParent(id)) {
    id = *parent;
    if (++depth >= 32) {
      break;
    }
  }
  return depth;
}

bool luciole::DirectoryModel::canSelect() const { return m_selectionAvailable; }

void luciole::DirectoryModel::reconcileSelection() {
  if (!canSelect()) {
    m_selection = {};
    m_scrollAnchor.reset();
    return;
  }
  std::erase_if(m_selection.selected, [this](EntryId id) { return !m_positions.contains(id); });
  for (auto* id : {&m_selection.focused, &m_selection.anchor, &m_scrollAnchor}) {
    if (*id && !m_positions.contains(**id)) {
      id->reset();
    }
  }
}

std::vector<luciole::EntryId> luciole::DirectoryModel::selectedIds() const {
  std::vector<EntryId> ids;
  ids.reserve(m_selection.selected.size());
  for (const auto id : m_rows) {
    if (m_selection.selected.contains(id)) {
      ids.push_back(id);
    }
  }
  return ids;
}

luciole::SelectionSummary luciole::DirectoryModel::selectionSummary() const {
  SelectionSummary summary;
  for (const auto id : m_selection.selected) {
    const auto* item = entry(id);
    if (item == nullptr) {
      continue;
    }
    if (!item->symlink && item->type == luciole::FileType::Directory) {
      ++summary.folders;
    } else if (!item->symlink && item->type == luciole::FileType::Regular) {
      ++summary.files;
      if (!item->size) {
        ++summary.unknownSizes;
      } else if (*item->size > std::numeric_limits<std::uint64_t>::max() - summary.bytes) {
        summary.bytes = std::numeric_limits<std::uint64_t>::max();
        summary.overflow = true;
      } else {
        summary.bytes += *item->size;
      }
    } else {
      ++summary.others;
    }
  }
  return summary;
}

bool luciole::DirectoryModel::select(EntryId id, SelectionMode mode) {
  if (!canSelect() || !m_positions.contains(id)) {
    return false;
  }
  if (mode == SelectionMode::Replace) {
    m_selection.selected = {id};
    m_selection.anchor = id;
  } else if (mode == SelectionMode::Toggle) {
    if (m_selection.selected.contains(id)) {
      m_selection.selected.erase(id);
    } else {
      m_selection.selected.insert(id);
    }
    m_selection.anchor = id;
  } else {
    const auto anchor = m_selection.anchor.value_or(m_selection.focused.value_or(id));
    m_selection.anchor = anchor;
    if (mode == SelectionMode::Range) {
      m_selection.selected.clear();
    }
    const auto [first, last] = std::minmax(m_positions.at(anchor), m_positions.at(id));
    for (std::size_t i = first; i <= last; ++i) {
      m_selection.selected.insert(m_rows[i]);
    }
  }
  m_selection.focused = id;
  notify();
  return true;
}

void luciole::DirectoryModel::selectAll() {
  if (!canSelect()) {
    return;
  }
  m_selection.selected.clear();
  m_selection.selected.insert(m_rows.begin(), m_rows.end());
  if (!m_rows.empty() && !m_selection.focused) {
    m_selection.focused = m_rows.front();
  }
  notify();
}

bool luciole::DirectoryModel::invertSelection() {
  if (!canSelect()) {
    return false;
  }
  std::unordered_set<EntryId> selected;
  for (const auto id : m_rows) {
    if (!m_selection.selected.contains(id)) {
      selected.insert(id);
    }
  }
  m_selection.selected = std::move(selected);
  notify();
  return true;
}
std::optional<std::string> luciole::DirectoryModel::selectPattern(const std::string& pattern) {
  if (const auto error = selectionPatternProblem(pattern)) {
    return error;
  }
  if (!canSelect()) {
    return "The listing is not available for selection.";
  }
  const auto folded = foldedDisplayText(pattern);
  const std::unique_ptr<GPatternSpec, decltype(&g_pattern_spec_free)> spec(
      g_pattern_spec_new(folded.c_str()), g_pattern_spec_free
  );
  std::unordered_set<EntryId> selected;
  for (const auto id : m_rows) {
    if (g_pattern_spec_match_string(spec.get(), m_presentations.at(id).filterKey.c_str())) {
      selected.insert(id);
    }
  }
  m_selection.selected = std::move(selected);
  notify();
  return std::nullopt;
}
std::optional<luciole::EntryId> luciole::DirectoryModel::prefixMatch(const std::string& prefix, bool cycle) const {
  if (!canSelect() || prefix.empty() || m_rows.empty()) {
    return std::nullopt;
  }
  const auto folded = foldedDisplayText(prefix);
  std::size_t start = 0;
  if (m_selection.focused) {
    const auto found = m_positions.find(*m_selection.focused);
    if (found != m_positions.end()) {
      start = (found->second + (cycle ? 1 : 0)) % m_rows.size();
    }
  }
  for (std::size_t offset = 0; offset < m_rows.size(); ++offset) {
    const auto id = m_rows[(start + offset) % m_rows.size()];
    if (m_presentations.at(id).filterKey.starts_with(folded)) {
      return id;
    }
  }
  return std::nullopt;
}

void luciole::DirectoryModel::clearSelection() {
  m_selection.selected.clear();
  m_selection.anchor.reset();
  notify();
}

bool luciole::DirectoryModel::replaceSelection(std::span<const EntryId> ids) {
  if (!canSelect() || std::ranges::any_of(ids, [this](EntryId id) { return !m_positions.contains(id); })) {
    return false;
  }
  std::unordered_set<EntryId> selected(ids.begin(), ids.end());
  if (selected == m_selection.selected) {
    return true;
  }
  m_selection.selected = std::move(selected);
  m_selection.anchor = ids.empty() ? std::nullopt : std::optional(ids.front());
  if (!ids.empty() && (!m_selection.focused || !m_selection.selected.contains(*m_selection.focused))) {
    m_selection.focused = ids.back();
  }
  notify();
  return true;
}

bool luciole::DirectoryModel::setFocus(std::optional<EntryId> id) {
  if (id && (!canSelect() || !m_positions.contains(*id))) {
    return false;
  }
  m_selection.focused = id;
  notify();
  return true;
}

bool luciole::DirectoryModel::setScrollAnchor(std::optional<EntryId> id) {
  if (id && (!canSelect() || !m_positions.contains(*id))) {
    return false;
  }
  m_scrollAnchor = id;
  notify();
  return true;
}

void luciole::DirectoryModel::notify() const {
  const auto callback = m_changed;
  if (callback) {
    callback();
  }
}

void luciole::DirectoryModel::restoreInteraction(SelectionState selection, std::optional<EntryId> scroll) {
  if (!m_selectionAvailable) {
    return;
  }
  const auto visible = [this](EntryId id) { return m_positions.contains(id); };
  std::erase_if(selection.selected, [&](EntryId id) { return !visible(id); });
  if (selection.focused && !visible(*selection.focused)) {
    selection.focused.reset();
  }
  if (selection.anchor && !visible(*selection.anchor)) {
    selection.anchor.reset();
  }
  if (scroll && !visible(*scroll)) {
    scroll.reset();
  }
  m_selection = std::move(selection);
  m_scrollAnchor = scroll;
  notify();
}
