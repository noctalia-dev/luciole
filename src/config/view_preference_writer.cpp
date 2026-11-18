#include "config/view_preference_writer.h"

#include "fs/background_job.h"
#include "fs/directory_watch.h"

#include <algorithm>
#include <glib.h>
#include <utility>

namespace {
  std::string issueMessage(const luciole::ConfigIssue& issue) {
    return issue.file.filename().string() + ": " + (issue.key.empty() ? "" : issue.key + ": ") + issue.message;
  }
  using WatchTarget = std::pair<std::filesystem::path, std::filesystem::path>;
  std::vector<WatchTarget> watchTargets(const luciole::ConfigPaths& paths) {
    std::vector<WatchTarget> targets;
    const auto add = [&](const std::filesystem::path& path) {
      auto directory = path.parent_path();
      std::error_code error;
      while (!directory.empty() && !std::filesystem::is_directory(directory, error)) {
        const auto parent = directory.parent_path();
        if (parent == directory) {
          return;
        }
        directory = parent;
      }
      if (directory.empty()) {
        return;
      }
      const auto relative = path.lexically_relative(directory);
      if (relative.empty()) {
        return;
      }
      const WatchTarget target{directory, directory / *relative.begin()};
      if (!std::ranges::contains(targets, target)) {
        targets.push_back(target);
      }
    };
    for (const auto& path : {paths.base, paths.overrides}) {
      add(path);
      // Observe directory/symlink replacement as well as edits to the two files.
      for (auto parent = path.parent_path(); parent != parent.root_path() && !parent.empty();
           parent = parent.parent_path()) {
        add(parent);
      }
      std::error_code error;
      const auto resolved = std::filesystem::canonical(path, error);
      if (!error && resolved != path) {
        add(resolved);
      }
    }
    return targets;
  }
} // namespace

struct luciole::ViewPreferenceWriter::State : std::enable_shared_from_this<State> {
  struct Result {
    std::string error;
    std::optional<Settings> settings;
    std::optional<Settings> inherited;
    std::vector<WatchTarget> targets;
  };
  ConfigPaths paths;
  std::vector<OverrideEdit> pending;
  std::function<void(const std::string&)> changed;
  std::optional<Settings> settings;
  std::optional<Settings> inherited;
  std::string pendingError;
  bool active = false;
  bool reloadPending = false;
  bool watching = false;
  std::vector<WatchTarget> targets;
  std::vector<std::unique_ptr<DirectoryWatch>> monitors;
  GSource* debounce = nullptr;

  void stopWatching() {
    watching = false;
    if (auto* source = std::exchange(debounce, nullptr)) {
      g_source_destroy(source);
      g_source_unref(source);
    }
    monitors.clear();
    targets.clear();
  }

  void scheduleReload() {
    if (debounce) {
      g_source_destroy(debounce);
      g_source_unref(debounce);
    }
    debounce = g_timeout_source_new(100);
    g_source_set_callback(
        debounce,
        [](gpointer data) -> gboolean {
          auto* owner = static_cast<State*>(data);
          g_source_unref(std::exchange(owner->debounce, nullptr));
          owner->reloadPending = true;
          owner->pump();
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(debounce, g_main_context_default());
  }

  void watch(std::vector<WatchTarget> next) {
    if (!watching || targets == next) {
      return;
    }
    stopWatching();
    watching = true;
    targets = std::move(next);
    std::vector<std::filesystem::path> directories;
    for (const auto& [directory, target] : targets) {
      if (std::ranges::contains(directories, directory)) {
        continue;
      }
      directories.push_back(directory);
      auto monitor = DirectoryWatch::start(directory, [this](const std::filesystem::path& path) {
        if (std::ranges::any_of(targets, [&](const auto& entry) { return entry.second == path; })) {
          scheduleReload();
        }
      });
      if (!monitor) {
        continue;
      }
      monitors.push_back(std::move(monitor));
    }
    // Close the read/install gap, including directories created while the first load ran.
    reloadPending = true;
  }

  void pump() {
    if (active || (pending.empty() && !reloadPending)) {
      return;
    }
    auto edits = std::exchange(pending, {});
    reloadPending = false;
    active = true;
    runJob<Result>(
        [owner = shared_from_this(), edits = std::move(edits)] {
          Result outcome;
          ConfigService service(owner->paths);
          if (!edits.empty()) {
            const auto result = service.saveOverrides(edits);
            if (result == AtomicWriteState::NotCommitted || !service.issues().empty()) {
              outcome.error =
                  service.issues().empty() ? "Cannot save settings." : issueMessage(service.issues().front());
            }
          }
          const auto loaded = loadSettings(owner->paths);
          const auto inherited = loadInheritedSettings(owner->paths);
          if (loaded && inherited) {
            outcome.settings = *loaded;
            outcome.inherited = *inherited;
          } else if (outcome.error.empty()) {
            outcome.error = loaded ? issueMessage(inherited.error().front()) : issueMessage(loaded.error().front());
          }
          outcome.targets = watchTargets(owner->paths);
          return outcome;
        },
        [owner = shared_from_this()](JobResult<Result> result) {
          if (!result) {
            Result failed;
            failed.error = result.error().message;
            result = std::move(failed);
          }
          auto& outcome = *result;
          owner->active = false;
          owner->watch(outcome.targets);
          if (!outcome.error.empty()) {
            owner->pendingError = outcome.error;
          }
          if (!owner->pending.empty() || owner->reloadPending) {
            owner->pump();
            return;
          }
          if (outcome.settings) {
            owner->settings = outcome.settings;
          }
          if (outcome.inherited) {
            owner->inherited = outcome.inherited;
          }
          const auto changed = owner->changed;
          const auto error = std::exchange(owner->pendingError, {});
          if (changed) {
            changed(error);
          }
        }
    );
  }

  void queue(const std::vector<OverrideEdit>& edits) {
    for (const auto& edit : edits) {
      std::erase_if(pending, [&](const auto& previous) { return previous.preference() == edit.preference(); });
      pending.push_back(edit);
    }
    pump();
  }
};

luciole::ViewPreferenceWriter::ViewPreferenceWriter(ConfigPaths paths) : m_state(std::make_shared<State>()) {
  m_state->paths = std::move(paths);
}
luciole::ViewPreferenceWriter::~ViewPreferenceWriter() {
  m_state->changed = {};
  m_state->stopWatching();
}
void luciole::ViewPreferenceWriter::save(ViewMode mode) { m_state->queue({{BrowserPreference::ViewMode, mode}}); }
void luciole::ViewPreferenceWriter::saveGridIconSize(int size) {
  m_state->queue({{BrowserPreference::GridIconSize, size}});
}
void luciole::ViewPreferenceWriter::saveListIconSize(int size) {
  m_state->queue({{BrowserPreference::ListIconSize, size}});
}
void luciole::ViewPreferenceWriter::saveSidebarWidth(int width) {
  m_state->queue({{BrowserPreference::SidebarWidth, width}});
}
void luciole::ViewPreferenceWriter::saveShowSidebar(bool visible) {
  m_state->queue({{BrowserPreference::ShowSidebar, visible}});
}
void luciole::ViewPreferenceWriter::saveInteraction(ActivationMode activation, TypingMode typing) {
  m_state->queue({{BrowserPreference::ActivationMode, activation}, {BrowserPreference::TypingMode, typing}});
}
void luciole::ViewPreferenceWriter::saveShowHidden(bool visible) {
  m_state->queue({{BrowserPreference::ShowHidden, visible}});
}
void luciole::ViewPreferenceWriter::saveSort(SortOptions options) {
  m_state->queue({
      {BrowserPreference::SortBy, options.field},
      {BrowserPreference::SortDescending, options.descending},
      {BrowserPreference::DirectoriesFirst, options.directoriesFirst},
      {BrowserPreference::HiddenLast, options.hiddenLast},
  });
}
bool luciole::ViewPreferenceWriter::busy() const {
  return m_state->active || !m_state->pending.empty() || m_state->reloadPending;
}
void luciole::ViewPreferenceWriter::saveColumnOrder(DetailsColumnOrder order) {
  m_state->queue({{BrowserPreference::ColumnOrder, order}});
}

void luciole::ViewPreferenceWriter::saveColumnWidths(DetailsColumnWidths widths) {
  m_state->queue(
      {{BrowserPreference::NameColumnWidth, widths[0]},
       {BrowserPreference::TypeColumnWidth, widths[1]},
       {BrowserPreference::SizeColumnWidth, widths[2]},
       {BrowserPreference::ModifiedColumnWidth, widths[3]},
       {BrowserPreference::AccessedColumnWidth, widths[4]},
       {BrowserPreference::CreatedColumnWidth, widths[5]},
       {BrowserPreference::OwnerColumnWidth, widths[6]},
       {BrowserPreference::GroupColumnWidth, widths[7]},
       {BrowserPreference::PermissionsColumnWidth, widths[8]},
       {BrowserPreference::LocationColumnWidth, widths[9]}}
  );
}
void luciole::ViewPreferenceWriter::saveDetailsColumns(DetailsColumnVisibility visibility) {
  m_state->queue({
      {BrowserPreference::ShowTypeColumn, visibility.type},
      {BrowserPreference::ShowSizeColumn, visibility.size},
      {BrowserPreference::ShowModifiedColumn, visibility.modified},
      {BrowserPreference::ShowAccessedColumn, visibility.accessed},
      {BrowserPreference::ShowCreatedColumn, visibility.created},
      {BrowserPreference::ShowOwnerColumn, visibility.owner},
      {BrowserPreference::ShowGroupColumn, visibility.group},
      {BrowserPreference::ShowPermissionsColumn, visibility.permissions},
      {BrowserPreference::ShowLocationColumn, visibility.location},

  });
}
void luciole::ViewPreferenceWriter::setChangedCallback(std::function<void(const std::string&)> callback) {
  m_state->changed = std::move(callback);
}

void luciole::ViewPreferenceWriter::submit(const std::vector<OverrideEdit>& edits) { m_state->queue(edits); }
void luciole::ViewPreferenceWriter::resetAll() { submit(inheritAllPreferences()); }
void luciole::ViewPreferenceWriter::reload() {
  m_state->reloadPending = true;
  m_state->pump();
}
void luciole::ViewPreferenceWriter::startWatching() {
  m_state->watching = true;
  reload();
}
const std::optional<luciole::Settings>& luciole::ViewPreferenceWriter::settings() const { return m_state->settings; }
const std::optional<luciole::Settings>& luciole::ViewPreferenceWriter::inheritedSettings() const {
  return m_state->inherited;
}
