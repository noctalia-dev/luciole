#include "browser/browser_controller.h"

#include "browser/directory_tree.h"
#include "browser/location_presentation.h"
#include "fs/file_query.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {
  std::uint64_t nextContext = 0;
}

struct luciole::BrowserController::ActivationState {
  CancelToken token;
  std::function<void(EntryId, const Location&, std::uint64_t, const std::expected<FileType, FileError>&)> resolved;
};

luciole::BrowserController::BrowserController(SortOptions sort, bool showHidden, bool showBackups)
    : m_activation(std::make_shared<ActivationState>()) {
  m_activation->resolved = [this](
                               EntryId id, const Location& target, std::uint64_t context,
                               const std::expected<FileType, FileError>& type
                           ) {
    const auto* entry = m_model.entry(id);
    if (context != m_contextSerial
        || !entry
        || entry->location() != target
        || !std::ranges::contains(m_model.rows(), id)) {
      return;
    }
    if (!type) {
      showMessage(type.error().message);
      return;
    }
    if (*type == FileType::Directory) {
      open(target);
      return;
    }
    if (m_fileActivated) {
      const auto callback = m_fileActivated;
      callback(target);
    } else {
      showMessage("Opening files is not available yet.");
    }
  };
  m_model.setSort(sort);
  m_model.setFilter({.showHidden = showHidden, .text = {}, .showBackups = showBackups});
  m_model.setChangedCallback([this] {
    if (!m_publishing) {
      m_restoring = false;
      m_restoredViewport.reset();
      notify();
    }
  });
  m_session.setChangedCallback([this] {
    if (!m_switchingSources && (m_query.empty() || !m_searchRecursive)) {
      publish(m_session.snapshot());
    }
  });
  m_search.setChangedCallback([this] {
    if (!m_switchingSources && m_searchActive && !m_query.empty()) {
      publish(m_search.snapshot());
    }
  });
}

luciole::BrowserController::~BrowserController() {
  cancelSearchTimeout();
  m_activation->resolved = {};
  m_activation->token.cancel();
  m_session.setChangedCallback({});
  m_search.setChangedCallback({});
  m_model.setChangedCallback({});
  m_tree.reset();
}

void luciole::BrowserController::advanceContext() {
  cancelSearchTimeout();
  m_contextSerial = ++nextContext;
  m_activation->token.cancel();
  m_activation->token = CancelToken{};
}

void luciole::BrowserController::setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }
void luciole::BrowserController::setFileActivatedCallback(std::function<void(const Location&)> callback) {
  m_fileActivated = std::move(callback);
}

void luciole::BrowserController::setCaptureViewport(std::function<BrowseViewport()> callback) {
  m_captureViewport = std::move(callback);
}
void luciole::BrowserController::setViewDefaults(ViewMode mode, int iconSize) {
  m_defaults.mode = mode;
  m_defaults.iconSize = iconSize;
}
std::optional<luciole::BrowseViewport> luciole::BrowserController::takeRestoredViewport() {
  return std::exchange(m_restoredViewport, std::nullopt);
}
std::optional<luciole::BrowserController::Reference>
luciole::BrowserController::reference(std::optional<EntryId> id) const {
  const auto* entry = id ? m_model.entry(*id) : nullptr;
  auto identity = entry ? entry->identity() : std::nullopt;
  if (!identity) {
    return std::nullopt;
  }
  return Reference{entry->location().uri(), std::move(*identity)};
}
void luciole::BrowserController::saveVisit() {
  if (m_history.empty() || m_restoring) {
    return;
  }
  auto& visit = m_history[m_historyIndex];
  visit.query = m_query;
  if (m_captureViewport) {
    visit.viewport = m_captureViewport();
  }
  visit.selected.clear();
  visit.selected.reserve(m_model.selection().selected.size());
  for (const auto id : m_model.selectedIds()) {
    if (auto item = reference(id)) {
      visit.selected.push_back(std::move(*item));
    }
  }
  visit.focus = reference(m_model.selection().focused);
  visit.anchor = reference(m_model.selection().anchor);
  visit.first = reference(visit.viewport.first);
  visit.viewport.first.reset();
}
void luciole::BrowserController::loadVisit() {
  const auto destination = m_history[m_historyIndex].location;
  const auto query = m_history[m_historyIndex].query;
  m_viewport = m_history[m_historyIndex].viewport;
  m_restoredViewport.reset();
  m_restoring = true;
  ++m_visitSerial;
  m_message.clear();
  m_query = query;
  m_searchComposing = false;
  m_searchActive = false;
  advanceContext();
  m_switchingSources = true;
  m_search.cancel();
  m_switchingSources = false;
  if (query.empty() || !m_searchRecursive) {
    m_session.open(destination);
  } else {
    // Browse metadata continues loading independently of the search projection.
    m_session.open(destination);
    m_searchActive = true;
    m_search.start(destination, query, m_model.filter().showHidden, m_model.filter().showBackups);
  }
}
void luciole::BrowserController::open(const Location& location) {
  const auto destination = location; // NOLINT(performance-unnecessary-copy-initialization)
  if (!m_history.empty() && m_history[m_historyIndex].location == destination) {
    m_restoring = false;
    m_restoredViewport.reset();
    if (!m_query.empty()) {
      m_query.clear();
      m_searchActive = false;
      m_searchComposing = false;
      advanceContext();
      m_search.cancel();
    }
    refresh();
    return;
  }
  saveVisit();
  if (!m_history.empty()) {
    m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(m_historyIndex + 1), m_history.end());
  }
  m_history.push_back({destination, {}, m_defaults, {}, {}, {}, {}});
  if (m_history.size() > 128) {
    m_history.erase(m_history.begin());
  }
  m_historyIndex = m_history.size() - 1;
  loadVisit();
}

bool luciole::BrowserController::openText(const std::string& text) {
  if (text.empty()) {
    showMessage("Enter a path or URI.");
    return false;
  }
  try {
    const auto base = m_model.snapshot().location ? *m_model.snapshot().location
                                                  : Location::fromPath(std::filesystem::current_path().native());
    open(resolveLocationText(text, base));
    return true;
  } catch (const std::exception& error) {
    showMessage(error.what());
    return false;
  }
}

void luciole::BrowserController::home() { (void)openText("~"); }

bool luciole::BrowserController::canBack() const { return !m_history.empty() && m_historyIndex > 0; }
bool luciole::BrowserController::canForward() const {
  return !m_history.empty() && m_historyIndex + 1 < m_history.size();
}

bool luciole::BrowserController::back() {
  if (!canBack()) {
    return false;
  }
  saveVisit();
  --m_historyIndex;
  loadVisit();
  return true;
}
bool luciole::BrowserController::forward() {
  if (!canForward()) {
    return false;
  }
  saveVisit();
  ++m_historyIndex;
  loadVisit();
  return true;
}

bool luciole::BrowserController::canUp() const {
  if (!m_model.snapshot().location) {
    return false;
  }
  return m_model.snapshot().location->parent().has_value();
}

bool luciole::BrowserController::up() {
  if (!m_model.snapshot().location) {
    return false;
  }
  const auto parent = m_model.snapshot().location->parent();
  if (!parent) {
    return false;
  }
  open(*parent);
  return true;
}

void luciole::BrowserController::refresh() {
  m_message.clear();
  if (m_query.empty() || !m_searchRecursive) {
    if (m_tree && m_query.empty()) {
      m_tree->refresh();
    }
    m_session.refresh();
  } else {
    startSearch();
  }
}

void luciole::BrowserController::stop() {
  if (m_tree) {
    m_tree->clear();
  }
  m_restoring = false;
  m_restoredViewport.reset();
  advanceContext();
  if (m_query.empty() || !m_searchRecursive) {
    m_session.cancel();
  } else {
    m_searchActive = false;
    m_search.cancel();
    auto snapshot = m_model.snapshot();
    snapshot.status = DirectoryStatus::Cancelled;
    publish(std::move(snapshot));
  }
}

void luciole::BrowserController::toggleHidden() {
  auto filter = m_model.filter();
  filter.showHidden = !filter.showHidden;
  m_model.setFilter(std::move(filter));
  if (!m_query.empty() && m_searchRecursive) {
    refresh();
  }
}

void luciole::BrowserController::setShowBackups(bool showBackups) {
  if (m_model.filter().showBackups == showBackups) {
    return;
  }
  auto filter = m_model.filter();
  filter.showBackups = showBackups;
  m_model.setFilter(std::move(filter));
  if (!m_query.empty() && m_searchRecursive) {
    refresh();
  }
}

void luciole::BrowserController::setSearchRecursive(bool recursive) {
  if (m_searchRecursive == recursive) {
    return;
  }
  m_searchRecursive = recursive;
  m_restoring = false;
  m_restoredViewport.reset();
  advanceContext();
  m_searchActive = false;
  m_search.cancel();
  if (recursive && !m_query.empty()) {
    startSearch();
  } else {
    publish(m_session.snapshot());
  }
}

void luciole::BrowserController::applyQueryFilter() {
  auto filter = m_model.filter();
  filter.text = m_searchRecursive ? std::string{} : m_query;
  m_model.setFilter(std::move(filter));
}

void luciole::BrowserController::cancelSearchTimeout() {
  if (auto* source = std::exchange(m_searchTimeout, nullptr)) {
    g_source_destroy(source);
    g_source_unref(source);
  }
  m_searchPending = false;
}

void luciole::BrowserController::scheduleSearch() {
  if (!m_searchPending || m_searchComposing) {
    return;
  }
  if (m_searchTimeout) {
    g_source_destroy(m_searchTimeout);
    g_source_unref(m_searchTimeout);
  }
  m_searchTimeout = g_timeout_source_new(175);
  g_source_set_callback(
      m_searchTimeout,
      [](gpointer data) -> gboolean {
        auto* owner = static_cast<BrowserController*>(data);
        auto* source = std::exchange(owner->m_searchTimeout, nullptr);
        g_source_unref(source);
        // startSearch may publish to an observer that destroys the controller.
        owner->startSearch();
        return G_SOURCE_REMOVE;
      },
      this, nullptr
  );
  g_source_attach(m_searchTimeout, g_main_context_default());
}

void luciole::BrowserController::startSearch() {
  cancelSearchTimeout();
  if (!m_searchRecursive || m_query.empty() || !m_session.snapshot().location) {
    return;
  }
  m_searchActive = true;
  m_search.start(*m_session.snapshot().location, m_query, m_model.filter().showHidden, m_model.filter().showBackups);
}

void luciole::BrowserController::publishPendingSearch() {
  publish(
      {.location = m_session.snapshot().location,
       .status = DirectoryStatus::Loading,
       .entries = {},
       .error = std::nullopt,
       .monitorError = std::nullopt,
       .incrementalResults = true}
  );
}

void luciole::BrowserController::editSearchQuery(std::string text) {
  if (m_query == text) {
    return;
  }
  m_restoring = false;
  m_restoredViewport.reset();
  advanceContext();
  m_searchActive = false;
  m_query = std::move(text);
  m_message.clear();
  m_search.cancel();
  if (!m_searchRecursive) {
    if (m_tree && (m_query.empty() || m_model.filter().text.empty())) {
      publish(m_session.snapshot());
    } else {
      applyQueryFilter();
    }
    return;
  }
  if (m_query.empty()) {
    publish(m_session.snapshot());
    return;
  }
  m_searchPending = true;
  scheduleSearch();
  publishPendingSearch();
}

void luciole::BrowserController::submitSearchQuery() {
  if (m_searchPending && !m_searchComposing) {
    startSearch();
  }
}

void luciole::BrowserController::setSearchComposing(bool composing) {
  if (m_searchComposing == composing) {
    return;
  }
  m_searchComposing = composing;
  if (!m_searchPending) {
    return;
  }
  if (composing) {
    if (auto* source = std::exchange(m_searchTimeout, nullptr)) {
      g_source_destroy(source);
      g_source_unref(source);
    }
  } else {
    scheduleSearch();
  }
}

void luciole::BrowserController::setSearchQuery(std::string text) {
  if (m_query == text) {
    submitSearchQuery();
    return;
  }
  m_restoring = false;
  m_restoredViewport.reset();
  advanceContext();
  m_searchActive = false;
  m_query = std::move(text);
  m_message.clear();
  if (m_query.empty() || !m_searchRecursive) {
    m_search.cancel();
    publish(m_session.snapshot());
  } else {
    startSearch();
  }
}

void luciole::BrowserController::publish(DirectorySnapshot snapshot) {
  snapshot.generation = m_revision = std::max(m_revision, m_model.snapshot().generation) + 1;
  m_publishing = true;
  applyQueryFilter();
  if (m_tree && m_query.empty()) {
    m_tree->setRoot(std::move(snapshot));
  } else {
    if (m_tree) {
      m_tree->clear();
    }
    m_model.applySnapshot(std::move(snapshot));
  }
  const auto status = m_model.snapshot().status;
  if (m_restoring && (status == DirectoryStatus::Ready || status == DirectoryStatus::Error)) {
    const auto& visit = m_history[m_historyIndex];
    // Only the visit's own references need resolving; a first visit has none.
    std::unordered_map<std::string_view, std::optional<EntryId>> visible;
    const auto want = [&](const Reference* reference) {
      if (reference != nullptr) {
        visible.try_emplace(reference->uri);
      }
    };
    for (const auto& item : visit.selected) {
      want(&item);
    }
    want(visit.focus ? &*visit.focus : nullptr);
    want(visit.anchor ? &*visit.anchor : nullptr);
    want(visit.first ? &*visit.first : nullptr);
    std::size_t unresolved = visible.size();
    for (const auto id : m_model.rows()) {
      if (unresolved == 0) {
        break;
      }
      const auto found = visible.find(m_model.entry(id)->location().uri());
      if (found != visible.end() && !found->second) {
        found->second = id;
        --unresolved;
      }
    }
    const auto resolve = [&](const std::optional<Reference>& reference) -> std::optional<EntryId> {
      if (!reference) {
        return std::nullopt;
      }
      const auto found = visible.find(reference->uri);
      if (found == visible.end()
          || !found->second
          || m_model.entry(*found->second)->identity() != reference->identity) {
        return std::nullopt;
      }
      return found->second;
    };
    SelectionState selection;
    selection.selected.reserve(visit.selected.size());
    for (const auto& item : visit.selected) {
      if (const auto id = resolve(item)) {
        selection.selected.insert(*id);
      }
    }
    selection.focused = resolve(visit.focus);
    selection.anchor = resolve(visit.anchor);
    auto viewport = visit.viewport;
    viewport.first = resolve(visit.first);
    m_model.restoreInteraction(std::move(selection), viewport.first);
    m_restoredViewport = viewport;
    m_restoring = false;
  }
  m_publishing = false;
  notify();
}

void luciole::BrowserController::setExpandableFolders(bool enabled) {
  if (enabled == expandableFolders()) {
    return;
  }
  m_tree.reset();
  if (enabled) {
    m_tree = std::make_unique<DirectoryTree>(m_model);
  }
  if (m_query.empty()) {
    publish(m_session.snapshot());
  }
}
bool luciole::BrowserController::toggleFolder(EntryId id) { return m_tree && m_query.empty() && m_tree->toggle(id); }
luciole::TreeRowInfo luciole::BrowserController::treeRow(EntryId id) const {
  return m_tree && m_query.empty() ? m_tree->row(id) : TreeRowInfo{};
}
bool luciole::BrowserController::treeKey(bool expand) {
  if (!m_tree || !m_query.empty() || !m_model.selection().focused) {
    return false;
  }
  const auto id = *m_model.selection().focused;
  const auto info = m_tree->row(id);
  if (expand) {
    if (info.expandable && !info.expanded) {
      (void)m_tree->expand(id);
    } else if (info.expanded) {
      const auto found = std::ranges::find_if(m_model.rows(), [&](auto row) { return m_model.treeParent(row) == id; });
      if (found != m_model.rows().end()) {
        m_model.select(*found);
      }
    }
  } else if (info.expanded) {
    (void)m_tree->collapse(id);
  } else if (const auto parent = m_model.treeParent(id)) {
    m_model.select(*parent);
  }
  return true;
}

void luciole::BrowserController::activate(EntryId id) {
  const auto* entry = m_model.entry(id);
  if (entry == nullptr || std::ranges::find(m_model.rows(), id) == m_model.rows().end()) {
    return;
  }
  const auto location = entry->location();
  if (location.isTrash()) {
    if (entry->type == FileType::Directory && !entry->symlink) {
      open(location);
    } else {
      showMessage("Restore this item before opening it.");
    }
  } else if (entry->symlink) {
    queryFileType(
        location, m_activation->token,
        [state = m_activation, id, target = location,
         context = m_contextSerial](std::expected<FileType, FileError> type) {
          const auto callback = state->resolved;
          if (callback && (type || type.error().kind != FileErrorKind::Cancelled)) {
            callback(id, target, context, type);
          }
        }
    );
  } else if (entry->type == FileType::Directory) {
    open(location);
  } else if (m_fileActivated) {
    const auto callback = m_fileActivated;
    callback(location);
  } else {
    showMessage("Opening files is not available yet.");
  }
}

void luciole::BrowserController::showMessage(std::string message) {
  m_message = std::move(message);
  notify();
}

void luciole::BrowserController::notify() const {
  const auto callback = m_changed;
  if (callback) {
    callback();
  }
}

void luciole::BrowserController::duplicateFrom(BrowserController& source) {
  if (&source == this) {
    return;
  }
  source.saveVisit();
  m_defaults = source.m_defaults;
  m_searchRecursive = source.m_searchRecursive;
  m_history = source.m_history;
  m_historyIndex = source.m_historyIndex;
  m_model.setSort(source.m_model.sort());
  m_model.setFilter({source.m_model.filter().showHidden, {}, source.m_model.filter().showBackups});
  if (!m_history.empty()) {
    loadVisit();
  }
}
