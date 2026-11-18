#include "browser/folder_views.h"

#include "fs/background_job.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

struct luciole::FolderViewService::State : std::enable_shared_from_this<State> {
  struct Result {
    std::vector<FolderViewEntry> entries;
    std::string error;
  };
  explicit State(std::filesystem::path path) : store(std::move(path)) {}
  FolderViewStore store; // Accessed only by the one running job.
  std::vector<FolderViewEntry> entries;
  // Folders edited before the first load finished; their in-memory settings win over the loaded ones.
  std::unordered_set<std::string> early;
  std::function<void()> changed;
  std::string error;
  bool enabled = false;
  bool loaded = false;
  bool active = false;
  bool dirty = false;

  void notify() const {
    if (changed) {
      const auto callback = changed;
      callback();
    }
  }

  static std::string message(const ConfigIssues& issues) {
    return issues.empty() ? std::string{} : issues.front().format();
  }

  void load() {
    active = true;
    runJob<Result>(
        [owner = shared_from_this()] {
          owner->store.load();
          return Result{.entries = owner->store.entries(), .error = message(owner->store.issues())};
        },
        [weak = weak_from_this()](JobResult<Result> result) {
          const auto owner = weak.lock();
          if (!owner) {
            return;
          }
          owner->active = false;
          owner->loaded = true;
          if (result) {
            auto merged = std::move(result->entries);
            std::erase_if(merged, [&](const auto& entry) { return owner->early.contains(entry.uri); });
            for (auto& entry : owner->entries) {
              merged.push_back(std::move(entry));
            }
            owner->entries = std::move(merged);
            owner->error = std::move(result->error);
          } else {
            owner->error = result.error().message;
          }
          owner->early.clear();
          owner->pump();
          owner->notify();
        }
    );
  }

  void pump() {
    if (active || !loaded || !dirty) {
      return;
    }
    dirty = false;
    active = true;
    runJob<std::string>(
        [owner = shared_from_this(), entries = entries] mutable {
          (void)owner->store.save(std::move(entries));
          return message(owner->store.issues());
        },
        // A strong reference, so an edit made during this save is still written after the service is gone.
        [owner = shared_from_this()](JobResult<std::string> result) {
          owner->active = false;
          owner->error = result ? std::move(*result) : result.error().message;
          owner->pump();
        }
    );
  }

  void edit(const Location& location, const std::function<void(FolderView&)>& change) {
    const auto& uri = location.uri();
    FolderView view;
    if (const auto found = std::ranges::find(entries, uri, &FolderViewEntry::uri); found != entries.end()) {
      view = found->view;
      entries.erase(found);
    }
    change(view);
    if (!view.empty()) {
      entries.push_back({.uri = uri, .view = view});
    }
    if (!loaded) {
      early.insert(uri);
    }
    dirty = true;
    pump();
    notify();
  }
};

luciole::FolderViewService::FolderViewService(std::filesystem::path path, bool enabled)
    : m_state(std::make_shared<State>(std::move(path))) {
  m_state->enabled = enabled;
  m_state->load();
}

luciole::FolderViewService::~FolderViewService() { m_state->changed = {}; }

void luciole::FolderViewService::setEnabled(bool enabled) {
  if (m_state->enabled == enabled) {
    return;
  }
  m_state->enabled = enabled;
  m_state->notify();
}

bool luciole::FolderViewService::enabled() const { return m_state->enabled; }

std::optional<luciole::FolderView> luciole::FolderViewService::lookup(const Location& location) const {
  if (!m_state->enabled) {
    return std::nullopt;
  }
  const auto& uri = location.uri();
  const auto found = std::ranges::find(m_state->entries, uri, &FolderViewEntry::uri);
  return found != m_state->entries.end() ? found->view : FolderView{};
}

void luciole::FolderViewService::setSort(const Location& location, SortOptions sort) {
  m_state->edit(location, [&](FolderView& view) { view.sort = sort; });
}

void luciole::FolderViewService::setMode(const Location& location, ViewMode mode) {
  m_state->edit(location, [&](FolderView& view) { view.mode = mode; });
}

void luciole::FolderViewService::reset(const Location& location) {
  m_state->edit(location, [](FolderView& view) { view = {}; });
}

bool luciole::FolderViewService::busy() const { return m_state->active || m_state->dirty; }

const std::string& luciole::FolderViewService::error() const { return m_state->error; }

void luciole::FolderViewService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
