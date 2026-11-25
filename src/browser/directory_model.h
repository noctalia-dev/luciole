#pragma once

#include "fs/directory_session.h"
#include "model/directory_options.h"

#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace luciole {
  using EntryId = std::uint64_t;

  enum class SelectionMode : std::uint8_t { Replace, Toggle, Range, AddRange };
  struct SelectionState {
    std::unordered_set<EntryId> selected;
    std::optional<EntryId> focused;
    std::optional<EntryId> anchor;
  };

  struct SelectionSummary {
    std::size_t files = 0;
    std::size_t folders = 0;
    std::size_t others = 0;
    std::uint64_t bytes = 0;
    std::size_t unknownSizes = 0;
    std::size_t pendingSizes = 0;
    bool overflow = false;
    bool operator==(const SelectionSummary&) const = default;
  };

  // One model per DirectorySession, updated on the main thread by its observer.
  // The model owns its snapshot; borrowed rows/entries expire on mutation.
  class DirectoryModel {
  public:
    // `known` is null while the entry's detailed facts are pending.
    using EntryFilter = std::function<bool(const EntryBrief&, const DirectoryEntry* known)>;

    DirectoryModel() = default;
    DirectoryModel(const DirectoryModel&) = delete;
    DirectoryModel& operator=(const DirectoryModel&) = delete;
    DirectoryModel(DirectoryModel&&) = delete;
    DirectoryModel& operator=(DirectoryModel&&) = delete;

    // Returns false for an obsolete generation. Completed same-folder removals continue the cursor through
    // surviving visible IDs. Observers must not throw.
    bool applySnapshot(
        DirectorySnapshot snapshot, std::unordered_map<EntryId, EntryId> parents = {}, bool preserveRootOrder = false
    );
    std::optional<EntryId> treeParent(EntryId id) const;
    std::size_t treeDepth(EntryId id) const;
    void setChangedCallback(std::function<void()> callback);
    void setMetadataRequestedCallback(std::function<void(std::size_t, EntryId, std::uint8_t)> callback);
    bool metadataPending(std::span<const EntryId> ids, std::uint8_t details = 0) const;
    void requestDetails(EntryId id, std::uint8_t details) const;
    void setSort(SortOptions options);
    void setFilter(FilterOptions options);
    void setEntryFilter(EntryFilter filter);
    // At most one selected entry; bulk selection is refused.
    void setSingleSelection(bool single);
    bool singleSelection() const { return m_singleSelection; }

    const DirectorySnapshot& snapshot() const { return m_snapshot; }
    const std::vector<EntryId>& rows() const { return m_rows; }
    const DirectoryEntry* entry(EntryId id) const;
    std::optional<Location> location(EntryId id) const;
    bool visible(EntryId id) const { return rowOf(id).has_value(); }
    std::string_view typeLabel(EntryId id) const;
    const SortOptions& sort() const { return m_sort; }
    const FilterOptions& filter() const { return m_filter; }
    const SelectionState& selection() const { return m_selection; }
    std::optional<EntryId> scrollAnchor() const { return m_scrollAnchor; }
    std::vector<EntryId> selectedIds() const; // Visible order, for action dispatch.
    SelectionSummary selectionSummary() const;

    // Reject invisible or unknown IDs without changing focus or selection.
    bool select(EntryId id, SelectionMode mode = SelectionMode::Replace);
    bool selectionAvailable() const { return m_selectionAvailable; }
    void selectAll();
    bool invertSelection();
    // Case-folded, normalized visible-name glob. Bulk selection keeps focus and range anchor.
    std::optional<std::string> selectPattern(const std::string& pattern);
    std::optional<EntryId> prefixMatch(const std::string& prefix, bool cycle = false) const;
    void clearSelection();
    bool replaceSelection(std::span<const EntryId> ids);
    void restoreInteraction(SelectionState selection, std::optional<EntryId> scroll);
    bool setFocus(std::optional<EntryId> id);
    bool setScrollAnchor(std::optional<EntryId> id);

  private:
    // Per-entry keys live NUL-terminated in m_keyText; presentations hold offsets.
    struct Presentation {
      std::uint32_t sortKey = 0;
      // kNoKey when the folded display name equals the display name; see filterText().
      std::uint32_t filterKey = kNoKey;
      // Points into m_typeLabels; equal labels share one string.
      const std::string* typeLabel = nullptr;
      bool knownType = false;
    };
    static constexpr std::uint32_t kNoRow = UINT32_MAX;
    static constexpr std::uint32_t kNoKey = UINT32_MAX;

    const char* keyText(std::uint32_t offset) const { return m_keyText.data() + offset; }
    // Open-addressing slot of `id` in `slots` over `entries`, or the empty slot where it would go.
    static std::size_t sourceSlot(const std::vector<std::uint32_t>& slots, const DirectoryEntries& entries, EntryId id);

    void prepareSortKeys();
    void rebuildProjection(bool resort = true);
    void reconcileSelection();
    void notify() const;
    bool canSelect() const;
    std::optional<std::size_t> sourceOf(EntryId id) const;
    // The visible row of an ID, or nullopt when it is filtered out or absent.
    std::optional<std::size_t> rowOf(EntryId id) const;
    std::string_view filterText(std::size_t source) const;
    struct SortItem {
      EntryId id;
      std::uint32_t source;
      bool directory; // A folder or a link to one, for Folders first.
      bool hidden;
      const Presentation* presentation;
      const DirectoryEntry* entry;
    };
    bool less(const SortItem& left, const SortItem& right) const;

    DirectorySnapshot m_snapshot;
    SortOptions m_sort;
    FilterOptions m_filter;
    std::string m_filterKey;
    mutable std::unordered_set<std::string> m_typeLabels;
    // Indexed like m_snapshot.entries.
    std::vector<Presentation> m_presentations;
    std::string m_keyText;
    // Source indexes by entry ID: open addressing over m_snapshot.entries, kNoRow marks an empty slot.
    std::vector<std::uint32_t> m_sourceSlots;
    // One selected text-sort field, shared by repeated values/containing folders.
    std::unordered_map<std::string, std::string> m_alternateSortKeys;
    // Indexed like m_snapshot.entries while a text sort field is prepared; points into m_alternateSortKeys.
    std::vector<const std::string*> m_alternateKeys;
    std::optional<SortField> m_preparedSortField;
    std::vector<std::uint32_t> m_sortedSources;
    FilterOptions m_sortedFilter;
    std::vector<EntryId> m_rows;
    std::unordered_map<EntryId, EntryId> m_treeParents;
    // Visible row of each source entry, kNoRow when filtered out.
    std::vector<std::uint32_t> m_rowBySource;
    SelectionState m_selection;
    std::optional<EntryId> m_scrollAnchor;
    bool m_selectionAvailable = false;
    bool m_preserveRootOrder = false;
    bool m_singleSelection = false;
    EntryFilter m_entryFilter;
    std::function<void()> m_changed;
    std::function<void(std::size_t, EntryId, std::uint8_t)> m_requestMetadata;
  };
} // namespace luciole
