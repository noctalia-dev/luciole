#include "browser/directory_tree.h"

#include <algorithm>
#include <set>

struct luciole::DirectoryTree::Node {
  struct Identity {
    EntryId id;
    std::optional<FileIdentity> identity;
  };
  // A node's session always lists the same folder, so a child's name stands for its location.
  struct Ids {
    std::map<std::string, Identity> previous;
    std::shared_ptr<EntryId> next;
    EntryId resolve(const DirectoryEntry& entry) const {
      const auto prior = previous.find(entry.name);
      return prior != previous.end() && prior->second.identity == entry.identity() ? prior->second.id : (*next)++;
    }
  };
  DirectoryEntry entry;
  std::shared_ptr<Ids> ids;
  DirectorySession session;
  bool started = false;
  Node(DirectoryEntry value, std::shared_ptr<EntryId> next)
      : entry(std::move(value)), ids(std::make_shared<Ids>(Ids{{}, std::move(next)})),
        session(maxChildren, [ids = ids](const DirectoryEntry& child) { return ids->resolve(child); }) {}
};

luciole::DirectoryTree::DirectoryTree(DirectoryModel& model, bool foldersOnly)
    : m_model(model), m_foldersOnly(foldersOnly) {}
luciole::DirectoryTree::~DirectoryTree() { clear(); }
void luciole::DirectoryTree::clear() {
  for (auto& [uri, node] : m_nodes) {
    node->session.setChangedCallback({});
  }
  m_nodes.clear();
  m_rejected.reset();
}
void luciole::DirectoryTree::releaseLocation(const Location& root) {
  const auto removed = std::erase_if(m_nodes, [&](const auto& item) {
    const auto& location = item.second->entry.navigationLocation();
    if (location == root || location.uri().starts_with(root.uri() + (root.uri().ends_with('/') ? "" : "/"))) {
      item.second->session.setChangedCallback({});
      return true;
    }
    return false;
  });
  if (removed) {
    publish();
  }
}

void luciole::DirectoryTree::setRoot(DirectorySnapshot snapshot) {
  if (m_root.location != snapshot.location) {
    clear();
  }
  m_root = std::move(snapshot);
  publish();
}
std::size_t luciole::DirectoryTree::loading() const {
  return std::ranges::count_if(m_nodes, [](const auto& pair) {
    return pair.second->started && pair.second->session.snapshot().status == DirectoryStatus::Loading;
  });
}
luciole::TreeRowInfo luciole::DirectoryTree::row(EntryId id) const {
  TreeRowInfo result{.depth = m_model.treeDepth(id), .enabled = true, .status = {}};
  const auto* entry = m_model.entry(id);
  result.expandable = entry && entry->isFolderLink() && !entry->symlink && !entry->location().isTrash();
  if (!entry) {
    return result;
  }
  if (const auto found = m_nodes.find(id); found != m_nodes.end()) {
    result.expanded = true;
    const auto& node = *found->second;
    const auto& snapshot = node.session.snapshot();
    result.loading = !node.started || snapshot.status == DirectoryStatus::Loading;
    if (result.loading) {
      result.status = "Loading…";
    } else if (snapshot.error) {
      result.status = snapshot.error->message;
    } else if (snapshot.status == DirectoryStatus::Ready && snapshot.entries.empty()) {
      result.status = "Empty";
    }
  }
  if (m_rejected && m_rejected->first == id) {
    result.status = m_rejected->second;
  }
  return result;
}
bool luciole::DirectoryTree::cycle(const DirectoryEntry& entry) const {
  auto parent = m_model.treeParent(entry.id);
  std::set<EntryId> visited;
  const auto location = entry.navigationLocation();
  const auto identity = entry.identity();
  while (parent) {
    if (!visited.insert(*parent).second) {
      return true;
    }
    const auto* ancestor = m_model.entry(*parent);
    if (!ancestor) {
      break;
    }
    if (ancestor->navigationLocation() == location || (identity && ancestor->identity() == identity)) {
      return true;
    }
    parent = m_model.treeParent(*parent);
  }
  return false;
}
bool luciole::DirectoryTree::expand(EntryId id) {
  const auto info = row(id);
  if (!info.expandable || info.expanded) {
    return false;
  }
  const auto entry = *m_model.entry(id);
  if (info.depth >= maxDepth || m_nodes.size() >= maxBranches || cycle(entry)) {
    m_rejected = {id, cycle(entry) ? "Folder cycle blocked" : "Tree limit reached. Open this folder to continue."};
    if (m_changed) {
      m_changed();
    }
    return false;
  }
  m_rejected.reset();
  auto node = std::make_unique<Node>(entry, m_nextId);
  node->session.setChangedCallback([this] {
    publish();
    pump();
  });
  m_nodes.emplace(id, std::move(node));
  publish();
  pump();
  return true;
}
bool luciole::DirectoryTree::collapse(EntryId id) {
  const auto* entry = m_model.entry(id);
  if (!entry || !m_nodes.contains(id)) {
    return false;
  }
  auto focused = m_model.selection().focused;
  while (focused && *focused != id) {
    focused = m_model.treeParent(*focused);
  }
  for (auto it = m_nodes.begin(); it != m_nodes.end();) {
    auto parent = std::optional(it->first);
    while (parent && *parent != id) {
      parent = m_model.treeParent(*parent);
    }
    if (parent) {
      it->second->session.setChangedCallback({});
      it = m_nodes.erase(it);
    } else {
      ++it;
    }
  }
  m_rejected.reset();
  publish();
  if (focused) {
    m_model.setFocus(id);
  }
  pump();
  return true;
}
bool luciole::DirectoryTree::toggle(EntryId id) { return row(id).expanded ? collapse(id) : expand(id); }
void luciole::DirectoryTree::pump() {
  if (m_pumping) {
    return;
  }
  m_pumping = true;
  while (loading() < maxLoading) {
    const auto found = std::ranges::find_if(m_nodes, [](const auto& pair) { return !pair.second->started; });
    if (found == m_nodes.end()) {
      break;
    }
    auto& node = *found->second;
    node.started = true;
    if (node.session.snapshot().location) {
      node.session.refresh();
    } else {
      node.session.open(node.entry.navigationLocation());
    }
  }
  m_pumping = false;
}
void luciole::DirectoryTree::refresh() {
  for (auto& [id, node] : m_nodes) {
    node->session.setChangedCallback({});
    node->session.cancel();
    node->started = false;
    node->session.setChangedCallback([this] {
      publish();
      pump();
    });
  }
  publish();
  pump();
}
void luciole::DirectoryTree::publish() {
  auto snapshot = m_root;
  std::vector<DirectoryEntries> sources{m_root.entries};
  std::vector<DirectoryEntries::Index> rows;
  std::unordered_map<EntryId, EntryId> parents;
  std::set<EntryId> retained;
  const auto append = [&](auto&& self, const DirectoryEntry& entry, std::size_t depth) -> void {
    if (depth >= maxDepth + 1) {
      return;
    }
    const auto found = m_nodes.find(entry.id);
    if (found == m_nodes.end()) {
      return;
    }
    auto& node = *found->second;
    if (entry.id != node.entry.id
        || entry.identity() != node.entry.identity()
        || entry.symlink
        || !entry.isFolderLink()
        || entry.navigationLocation() != node.entry.navigationLocation()) {
      return;
    }
    retained.insert(found->first);
    std::map<std::string, Node::Identity> currentIds;
    const auto source = sources.size();
    const auto& children = node.session.snapshot().entries;
    sources.push_back(children);
    for (std::size_t i = 0; i < children.size(); ++i) {
      const auto& child = children[i];
      currentIds.emplace(child.name, Node::Identity{child.id, child.identity()});
      if (m_foldersOnly && !child.isFolderLink()) {
        continue;
      }
      if (parents.empty()) {
        rows.reserve(m_root.entries.size() + children.size());
        for (std::size_t root = 0; root < m_root.entries.size(); ++root) {
          rows.push_back({0, root});
        }
      }
      parents.emplace(child.id, entry.id);
      rows.push_back({source, i});
      self(self, child, depth + 1);
    }
    node.ids->previous = std::move(currentIds);
  };
  if (!m_nodes.empty()) {
    for (const auto& entry : m_root.entries) {
      append(append, entry, 0);
    }
  }
  for (auto it = m_nodes.begin(); it != m_nodes.end();) {
    if (!retained.contains(it->first)) {
      it->second->session.setChangedCallback({});
      it = m_nodes.erase(it);
    } else {
      ++it;
    }
  }
  if (!parents.empty()) {
    snapshot.entries = DirectoryEntries::project(std::move(sources), rows);
  }
  snapshot.generation = m_model.snapshot().generation + 1;
  m_model.applySnapshot(std::move(snapshot), std::move(parents), m_foldersOnly);
  if (m_changed) {
    m_changed();
  }
}
