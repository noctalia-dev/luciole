#include "browser/directory_model.h"

#include "browser/entry_details.h"
#include "browser/location_presentation.h"
#include "browser/selection_tools.h"
#include "fs/file_visibility.h"
#include "fs/name_matching.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
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
  if (snapshot.location == m_snapshot.location
      && snapshot.entries.sameDirectoryLinks(m_snapshot.entries)
      && parents == m_treeParents
      && preserveRootOrder == m_preserveRootOrder
      && (m_sort.field == SortField::Name || m_sort.field == SortField::Location)) {
    m_snapshot = std::move(snapshot);
    reconcileSelection();
    notify();
    return true;
  }
  const auto focused = m_selection.focused;
  const auto focusedRow = focused ? rowOf(*focused) : std::nullopt;
  const auto focusedParent = focused ? treeParent(*focused) : std::nullopt;
  const bool focusedSelected = focused && m_selection.selected.contains(*focused);
  m_preserveRootOrder = preserveRootOrder;
  m_treeParents = std::move(parents);
  m_alternateSortKeys.clear();
  m_alternateKeys.clear();
  m_preparedSortField.reset();
  const bool sameLocation = snapshot.location == m_snapshot.location;
  const auto count = snapshot.entries.size();
  std::vector<Presentation> presentations(count);
  std::string keyText;
  keyText.reserve(sameLocation ? m_keyText.size() : 0);
  const auto appendKey = [&keyText](std::string_view text) {
    const auto offset = static_cast<std::uint32_t>(keyText.size());
    keyText.append(text);
    keyText.push_back('\0');
    return offset;
  };
  std::vector<std::uint32_t> slots(count == 0 ? 0 : std::bit_ceil(count * 2), kNoRow);
  const bool reuse = sameLocation && !m_sourceSlots.empty();
  struct TypeLabel {
    const std::string* label = nullptr;
    bool known = false;
  };
  std::unordered_map<std::string_view, TypeLabel> typeLabels;
  // Every presentation is rebuilt below, so the previous pool can be replaced afterwards.
  std::unordered_set<std::string> labels;
  const auto pooled = [&labels](std::string label) { return &*labels.insert(std::move(label)).first; };
  for (std::size_t i = 0; i < count; ++i) {
    const auto brief = snapshot.entries.brief(i);
    const auto* known = snapshot.entries.known(i);
    auto& presentation = presentations[i];
    // An ID keeps its keys while its display name is unchanged.
    const auto previous = reuse ? m_sourceSlots[sourceSlot(m_sourceSlots, m_snapshot.entries, brief.id)] : kNoRow;
    if (previous != kNoRow && m_snapshot.entries.brief(previous).displayName == brief.displayName) {
      const auto& kept = m_presentations[previous];
      presentation.sortKey = appendKey(this->keyText(kept.sortKey));
      presentation.filterKey = kept.filterKey == kNoKey ? kNoKey : appendKey(this->keyText(kept.filterKey));
    } else {
      const auto folded = foldedDisplayText(std::string(brief.displayName));
      const GString key(g_utf8_collate_key_for_filename(folded.c_str(), -1));
      presentation.sortKey = appendKey(key.get());
      if (folded != brief.displayName) {
        presentation.filterKey = appendKey(folded);
      }
    }
    // A repeated ID resolves to its last entry.
    slots[sourceSlot(slots, snapshot.entries, brief.id)] = static_cast<std::uint32_t>(i);
    if (!known) {
      continue;
    }
    const auto& entry = *known;
    const bool regular = entry.type == FileType::Regular && !entry.symlink;
    if (regular && entry.contentType) {
      auto [label, inserted] = typeLabels.try_emplace(*entry.contentType);
      if (inserted) {
        label->second = {
            .label = pooled(fileTypeDescription(entry.type, entry.symlink, entry.contentType)),
            .known = !g_content_type_is_unknown(entry.contentType->c_str()),
        };
      }
      presentation.typeLabel = label->second.label;
      presentation.knownType = label->second.known;
    } else {
      presentation.typeLabel = pooled(fileTypeDescription(entry.type, entry.symlink, entry.contentType));
      presentation.knownType =
          entry.symlink || entry.type == FileType::Symlink || (!regular && entry.type != FileType::Unknown);
    }
  }
  std::vector<EntryId> removalOrder;
  if (sameLocation
      && snapshot.status == DirectoryStatus::Ready
      && focusedRow
      && (slots.empty() || slots[sourceSlot(slots, snapshot.entries, *focused)] == kNoRow)) {
    // Keep the previous visible order only for an actual removal, never a filter or an incomplete scan.
    removalOrder.reserve(m_rows.size() - 1);
    removalOrder.insert(
        removalOrder.end(), m_rows.begin() + static_cast<std::ptrdiff_t>(*focusedRow + 1), m_rows.end()
    );
    for (auto index = *focusedRow; index > 0; --index) {
      removalOrder.push_back(m_rows[index - 1]);
    }
  }
  m_presentations = std::move(presentations);
  keyText.shrink_to_fit();
  m_keyText = std::move(keyText);
  m_sourceSlots = std::move(slots);
  m_typeLabels = std::move(labels);
  m_snapshot = std::move(snapshot);
  rebuildProjection();
  if (!removalOrder.empty()) {
    const auto next = std::ranges::find_if(removalOrder, [&](EntryId id) {
      // Retain failed/pending selected items; a collapsed tree must not select outside its old branch.
      return visible(id)
          && (m_selection.selected.empty() ? treeParent(id) == focusedParent : m_selection.selected.contains(id));
    });
    if (next != removalOrder.end()) {
      m_selection.focused = *next;
      if (focusedSelected && m_selection.selected.empty()) {
        m_selection.selected.insert(*next);
        m_selection.anchor = *next;
      }
    }
  }
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
  rebuildProjection(false);
  notify();
}

std::size_t luciole::DirectoryModel::sourceSlot(
    const std::vector<std::uint32_t>& slots, const DirectoryEntries& entries, EntryId id
) {
  const auto mask = slots.size() - 1;
  // Fibonacci hashing spreads the session's sequential IDs over the table.
  for (auto slot = static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> 32) & mask;; slot = (slot + 1) & mask) {
    if (slots[slot] == kNoRow || entries.brief(slots[slot]).id == id) {
      return slot;
    }
  }
}

std::optional<std::size_t> luciole::DirectoryModel::sourceOf(EntryId id) const {
  if (m_sourceSlots.empty()) {
    return std::nullopt;
  }
  const auto source = m_sourceSlots[sourceSlot(m_sourceSlots, m_snapshot.entries, id)];
  return source == kNoRow ? std::nullopt : std::optional<std::size_t>(source);
}

std::optional<std::size_t> luciole::DirectoryModel::rowOf(EntryId id) const {
  const auto source = sourceOf(id);
  if (!source || *source >= m_rowBySource.size() || m_rowBySource[*source] == kNoRow) {
    return std::nullopt;
  }
  return m_rowBySource[*source];
}

std::string_view luciole::DirectoryModel::filterText(std::size_t source) const {
  const auto key = m_presentations[source].filterKey;
  return key == kNoKey ? std::string_view(m_snapshot.entries.brief(source).displayName)
                       : std::string_view(keyText(key));
}

const luciole::DirectoryEntry* luciole::DirectoryModel::entry(EntryId id) const {
  const auto source = sourceOf(id);
  if (!source) {
    return nullptr;
  }
  if (!m_snapshot.entries.known(*source) && m_requestMetadata) {
    m_requestMetadata(*source, id, 0);
  }
  return &m_snapshot.entries[*source];
}

void luciole::DirectoryModel::setMetadataRequestedCallback(
    std::function<void(std::size_t, EntryId, std::uint8_t)> callback
) {
  m_requestMetadata = std::move(callback);
}
std::optional<luciole::Location> luciole::DirectoryModel::location(EntryId id) const {
  const auto source = sourceOf(id);
  return source ? std::optional(m_snapshot.entries.location(*source)) : std::nullopt;
}
bool luciole::DirectoryModel::metadataPending(std::span<const EntryId> ids, std::uint8_t details) const {
  bool pending = false;
  for (const auto id : ids) {
    const auto source = sourceOf(id);
    const auto* item = source ? m_snapshot.entries.known(*source) : nullptr;
    if (source
        && (!item || item->metadataPending || (!item->metadataFailed && (item->queriedDetails & details) != details))) {
      pending = true;
      if (m_requestMetadata) {
        m_requestMetadata(*source, id, details);
      }
    }
  }
  return pending;
}
void luciole::DirectoryModel::requestDetails(EntryId id, std::uint8_t details) const {
  const std::array ids{id};
  (void)metadataPending(ids, details);
}
std::string_view luciole::DirectoryModel::typeLabel(EntryId id) const {
  const auto source = sourceOf(id);
  if (!source) {
    return {};
  }
  if (m_snapshot.entries.isCatalogue() || m_presentations[*source].typeLabel == nullptr) {
    const auto* item = m_snapshot.entries.known(*source);
    if (!item) {
      return "Reading metadata…";
    }
    return *m_typeLabels.insert(fileTypeDescription(item->type, item->symlink, item->contentType)).first;
  }
  return *m_presentations[*source].typeLabel;
}

bool luciole::DirectoryModel::less(const SortItem& leftItem, const SortItem& rightItem) const {
  const auto left = leftItem.id;
  const auto right = rightItem.id;
  const auto& leftPresentation = *leftItem.presentation;
  const auto& rightPresentation = *rightItem.presentation;
  if (m_sort.hiddenLast && leftItem.hidden != rightItem.hidden) {
    return !leftItem.hidden;
  }
  if (m_sort.directoriesFirst && leftItem.directory != rightItem.directory) {
    return leftItem.directory;
  }
  if (needsTextSortKey(m_sort.field)) {
    const auto* first = m_alternateKeys[leftItem.source];
    const auto* second = m_alternateKeys[rightItem.source];
    if ((first != nullptr) != (second != nullptr)) {
      return first != nullptr;
    }
    if (first && second && first != second && *first != *second) {
      return m_sort.descending ? *first > *second : *first < *second;
    }
  } else if (m_sort.field != SortField::Name) {
    const auto first = leftItem.entry ? entrySortNumber(*leftItem.entry, m_sort.field) : std::nullopt;
    const auto second = rightItem.entry ? entrySortNumber(*rightItem.entry, m_sort.field) : std::nullopt;
    // Unknown metadata stays last in both directions; zero is a real value.
    if (first.has_value() != second.has_value()) {
      return first.has_value();
    }
    if (first && first != second) {
      return m_sort.descending ? first > second : first < second;
    }
  }
  int comparison = std::strcmp(keyText(leftPresentation.sortKey), keyText(rightPresentation.sortKey));
  if (comparison == 0) {
    comparison =
        m_snapshot.entries.location(leftItem.source).uri().compare(m_snapshot.entries.location(rightItem.source).uri());
  }
  if (comparison == 0) {
    return left < right;
  }
  return m_sort.descending ? comparison > 0 : comparison < 0;
}

void luciole::DirectoryModel::prepareSortKeys() {
  if (!needsTextSortKey(m_sort.field)) {
    m_alternateKeys = {};
    m_alternateSortKeys.clear();
    m_preparedSortField.reset();
    return;
  }
  if (m_preparedSortField == m_sort.field) {
    return;
  }
  m_alternateSortKeys.clear();
  m_alternateKeys.assign(m_presentations.size(), nullptr);
  for (std::size_t source = 0; source < m_presentations.size(); ++source) {
    const auto& presentation = m_presentations[source];
    auto& alternate = m_alternateKeys[source];
    const auto* entry = m_snapshot.entries.known(source);
    if (m_sort.field == SortField::Location) {
      if (const auto parent = m_snapshot.entries.location(source).parent()) {
        auto [key, inserted] = m_alternateSortKeys.try_emplace(parent->uri());
        if (inserted) {
          key->second = textSortKey(locationEditText(*parent));
        }
        alternate = &key->second;
      }
      continue;
    }
    if (!entry) {
      continue;
    }
    const std::string* value = nullptr;
    if (m_sort.field == SortField::Type && presentation.knownType) {
      value = presentation.typeLabel;
    } else if (m_sort.field == SortField::Owner && entry->details.owner) {
      value = &*entry->details.owner;
    } else if (m_sort.field == SortField::Group && entry->details.group) {
      value = &*entry->details.group;
    }
    if (value) {
      auto [key, inserted] = m_alternateSortKeys.try_emplace(*value);
      if (inserted) {
        key->second = textSortKey(*value);
      }
      alternate = &key->second;
    }
  }
  m_preparedSortField = m_sort.field;
}

void luciole::DirectoryModel::setEntryFilter(EntryFilter filter) {
  m_entryFilter = std::move(filter);
  rebuildProjection();
  notify();
}

void luciole::DirectoryModel::setSingleSelection(bool single) {
  m_singleSelection = single;
  if (single && m_selection.selected.size() > 1) {
    const auto ids = selectedIds();
    m_selection.selected.clear();
    if (!ids.empty()) {
      m_selection.selected.insert(ids.front());
      m_selection.anchor = ids.front();
    }
  }
  notify();
}

void luciole::DirectoryModel::rebuildProjection(bool resort) {
  // A wider filter can admit entries absent from the cached order.
  resort = resort
      || m_entryFilter
      || (m_filter.showHidden && !m_sortedFilter.showHidden)
      || (m_filter.showBackups && !m_sortedFilter.showBackups)
      || !m_filterKey.contains(foldedDisplayText(m_sortedFilter.text));
  const auto matches = [this](std::size_t source, const EntryBrief& entry) {
    return (!entry.hidden || m_filter.showHidden)
        && (m_filter.showBackups || !isEditorBackup(entry.name, entry.type))
        && (m_filterKey.empty() || filterText(source).contains(m_filterKey))
        && (!m_entryFilter || m_entryFilter(entry, m_snapshot.entries.known(source)));
  };
  m_rows.clear();
  m_rowBySource.assign(m_snapshot.entries.size(), kNoRow);
  if (resort) {
    prepareSortKeys();
    // Sorting reads presentation and entry through direct pointers rather than hashing IDs per comparison.
    std::vector<SortItem> items;
    items.reserve(m_snapshot.entries.size());
    for (std::size_t source = 0; source < m_snapshot.entries.size(); ++source) {
      const auto entry = m_snapshot.entries.brief(source);
      if (!matches(source, entry)) {
        continue;
      }
      items.push_back(
          {entry.id, static_cast<std::uint32_t>(source), entry.type == FileType::Directory || entry.linksToDirectory,
           entry.hidden, &m_presentations[source], m_snapshot.entries.known(source)}
      );
    }
    std::ranges::sort(items, [this](const SortItem& left, const SortItem& right) { return less(left, right); });
    m_sortedSources.clear();
    m_sortedSources.reserve(items.size());
    m_rows.reserve(items.size());
    for (const auto& item : items) {
      m_sortedSources.push_back(item.source);
      m_rowBySource[item.source] = static_cast<std::uint32_t>(m_rows.size());
      m_rows.push_back(item.id);
    }
    m_sortedFilter = m_filter;
  } else {
    m_rows.reserve(m_sortedSources.size());
    for (const auto source : m_sortedSources) {
      const auto entry = m_snapshot.entries.brief(source);
      if (matches(source, entry)) {
        m_rowBySource[source] = static_cast<std::uint32_t>(m_rows.size());
        m_rows.push_back(entry.id);
      }
    }
  }
  if (!m_treeParents.empty() || m_preserveRootOrder) {
    // A match inside a filtered-out folder keeps its ancestors so the tree can still reach it.
    if (!m_treeParents.empty()) {
      std::unordered_set<EntryId> present(m_rows.begin(), m_rows.end());
      std::vector<EntryId> rows;
      rows.reserve(m_rows.size());
      for (const auto id : m_rows) {
        std::vector<EntryId> missing;
        for (auto parent = treeParent(id); parent && missing.size() < 32 && present.insert(*parent).second;
             parent = treeParent(*parent)) {
          missing.push_back(*parent);
        }
        rows.insert(rows.end(), missing.rbegin(), missing.rend());
        rows.push_back(id);
      }
      m_rows = std::move(rows);
    }
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
      std::ranges::sort(roots, {}, [&](EntryId id) { return *sourceOf(id); });
    }
    m_rows.clear();
    std::unordered_set<EntryId> visited;
    const auto append = [&](auto&& self, EntryId id, std::size_t depth) -> void {
      if (depth > 32 || !visited.insert(id).second) {
        return;
      }
      m_rows.push_back(id);
      if (const auto found = children.find(id); found != children.end()) {
        for (const auto child : found->second) {
          self(self, child, depth + 1);
        }
      }
    };
    for (const auto id : roots) {
      append(append, id, 0);
    }
    m_rowBySource.assign(m_snapshot.entries.size(), kNoRow);
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
      m_rowBySource[*sourceOf(m_rows[row])] = static_cast<std::uint32_t>(row);
    }
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
  std::erase_if(m_selection.selected, [this](EntryId id) { return !rowOf(id); });
  for (auto* id : {&m_selection.focused, &m_selection.anchor, &m_scrollAnchor}) {
    if (*id && !rowOf(**id)) {
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
    const auto source = sourceOf(id);
    if (!source) {
      continue;
    }
    const auto brief = m_snapshot.entries.brief(*source);
    const auto* item = m_snapshot.entries.known(*source);
    if (!item) {
      if (brief.type == FileType::Directory && !brief.symlink) {
        ++summary.folders;
      } else if (brief.type == FileType::Regular && !brief.symlink) {
        ++summary.files;
        ++summary.pendingSizes;
      } else {
        ++summary.others;
      }
      if (m_requestMetadata) {
        m_requestMetadata(*source, id, 0);
      }
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
  if (!canSelect() || !rowOf(id)) {
    return false;
  }
  if (m_singleSelection) {
    if (mode == SelectionMode::Toggle && m_selection.selected.contains(id)) {
      m_selection.selected.clear();
      m_selection.focused = id;
      notify();
      return true;
    }
    mode = SelectionMode::Replace;
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
    // std::minmax returns references; keep the rows alive in named values.
    const auto anchorRow = *rowOf(anchor);
    const auto idRow = *rowOf(id);
    const auto [first, last] = std::minmax(anchorRow, idRow);
    for (std::size_t i = first; i <= last; ++i) {
      m_selection.selected.insert(m_rows[i]);
    }
  }
  m_selection.focused = id;
  notify();
  return true;
}

void luciole::DirectoryModel::selectAll() {
  if (!canSelect() || m_singleSelection) {
    return;
  }
  m_selection.selected.clear();
  m_selection.selected.insert(m_rows.begin(), m_rows.end());
  notify();
}

bool luciole::DirectoryModel::invertSelection() {
  if (!canSelect() || m_singleSelection) {
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
  if (m_singleSelection) {
    return "Only one item can be chosen.";
  }
  const auto folded = foldedDisplayText(pattern);
  const std::unique_ptr<GPatternSpec, decltype(&g_pattern_spec_free)> spec(
      g_pattern_spec_new(folded.c_str()), g_pattern_spec_free
  );
  std::unordered_set<EntryId> selected;
  for (const auto id : m_rows) {
    // filterText views a whole std::string, so its data is NUL-terminated.
    if (g_pattern_spec_match_string(spec.get(), filterText(*sourceOf(id)).data())) {
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
    if (const auto row = rowOf(*m_selection.focused)) {
      start = (*row + (cycle ? 1 : 0)) % m_rows.size();
    }
  }
  for (std::size_t offset = 0; offset < m_rows.size(); ++offset) {
    const auto id = m_rows[(start + offset) % m_rows.size()];
    if (filterText(*sourceOf(id)).starts_with(folded)) {
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
  if (!canSelect() || std::ranges::any_of(ids, [this](EntryId id) { return !rowOf(id); })) {
    return false;
  }
  if (m_singleSelection && ids.size() > 1) {
    ids = ids.first(1);
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
  if (id && (!canSelect() || !rowOf(*id))) {
    return false;
  }
  m_selection.focused = id;
  notify();
  return true;
}

bool luciole::DirectoryModel::setScrollAnchor(std::optional<EntryId> id) {
  if (id && (!canSelect() || !rowOf(*id))) {
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
  const auto visible = [this](EntryId id) { return rowOf(id).has_value(); };
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
