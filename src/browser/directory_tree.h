#pragma once

#include "browser/directory_model.h"

#include <map>

namespace luciole {
  struct TreeRowInfo {
    std::size_t depth = 0;
    bool enabled = false, expandable = false, expanded = false, loading = false;
    std::string status;
  };
  // Shared lazy hierarchy for the details view and sidebar. All entry IDs remain model action targets.
  class DirectoryTree {
  public:
    static constexpr std::size_t maxBranches = 16, maxDepth = 16, maxChildren = 2048, maxLoading = 4;
    explicit DirectoryTree(DirectoryModel& model, bool foldersOnly = false);
    ~DirectoryTree();
    void setRoot(DirectorySnapshot snapshot);
    void clear();
    // Collapse branches on a drive before explicit removal, including branches below a different root.
    void releaseLocation(const Location& root);
    bool toggle(EntryId id);
    bool expand(EntryId id);
    bool collapse(EntryId id);
    void refresh();
    TreeRowInfo row(EntryId id) const;
    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }
    std::size_t branches() const { return m_nodes.size(); }
    std::size_t loading() const;

  private:
    struct Node;
    void publish();
    void pump();
    bool cycle(const DirectoryEntry& entry) const;
    DirectoryModel& m_model;
    bool m_foldersOnly = false, m_pumping = false;
    DirectorySnapshot m_root;
    std::map<EntryId, std::unique_ptr<Node>> m_nodes;
    std::shared_ptr<EntryId> m_nextId = std::make_shared<EntryId>(EntryId{1} << 62);
    std::optional<std::pair<EntryId, std::string>> m_rejected;
    std::function<void()> m_changed;
  };
} // namespace luciole
