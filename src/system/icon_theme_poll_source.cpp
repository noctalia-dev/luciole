#include "system/icon_theme_poll_source.h"

#include "core/inotify/inotify.h"
#include "core/log.h"
#include "fs/gio_ptr.h"
#include "system/icon_resolver.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace {
  using Clock = std::chrono::steady_clock;
  constexpr Logger kLog("icon-theme");
  // Fallback check while the watch set is missing or incomplete; a complete set reports every relevant change.
  constexpr std::chrono::seconds kCheckInterval{60};
  constexpr std::chrono::milliseconds kDebounceInterval{100};
  constexpr std::uint32_t kEntryChange = IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
  constexpr std::uint32_t kWatchMask = kEntryChange | IN_DELETE_SELF | IN_MOVE_SELF;

  enum WatchRole : std::uint8_t {
    WatchBaseDir = 1U << 0U,
    WatchThemeRoot = 1U << 1U,
    WatchIconDir = 1U << 2U,
    WatchSettingsDir = 1U << 3U,
  };

  struct WatchDetails {
    std::uint8_t roles = 0;
    std::unordered_set<std::string> settingsFiles;
    std::unordered_set<std::string> children;
  };

  bool hasRole(std::uint8_t roles, WatchRole role) { return (roles & static_cast<std::uint8_t>(role)) != 0; }
  bool isIconFile(std::string_view name) { return name.ends_with(".svg") || name.ends_with(".png"); }

  fs::path directoryPath(const std::string& value) {
    auto path = fs::path(value);
    if (path != path.root_path() && path.filename().empty()) {
      path = path.parent_path();
    }
    return path;
  }

  struct WatchSet {
    Inotify inotify;
    std::unordered_map<int, WatchDetails> watches;
    bool complete = true;

    WatchSet(const IconThemeWatchPaths& paths, GCancellable* cancelled) {
      std::unordered_map<std::string, WatchDetails> desired;
      for (const auto& path : paths.baseDirs) {
        desired[directoryPath(path).string()].roles |= WatchBaseDir | WatchIconDir;
      }
      for (const auto& path : paths.themeRoots) {
        desired[directoryPath(path).string()].roles |= WatchThemeRoot;
      }
      for (const auto& path : paths.iconDirs) {
        desired[directoryPath(path).string()].roles |= WatchIconDir;
      }
      for (const auto& path : paths.settingsFiles) {
        const fs::path file(path);
        auto& watch = desired[file.parent_path().string()];
        watch.roles |= WatchSettingsDir;
        watch.settingsFiles.emplace(file.filename().string());
      }

      std::vector<std::string> order;
      order.reserve(desired.size());
      for (const auto& [path, details] : desired) {
        (void)details;
        order.push_back(path);
      }
      std::unordered_set<std::string> expanded;
      for (const auto& value : order) {
        auto path = fs::path(value);
        while (path != path.root_path() && expanded.emplace(path.string()).second) {
          const auto parent = path.parent_path();
          if (parent.empty()) {
            break;
          }
          desired[parent.string()].children.emplace(path.filename().string());
          path = parent;
        }
      }
      order.clear();
      order.reserve(desired.size());
      for (const auto& [path, details] : desired) {
        (void)details;
        order.push_back(path);
      }
      // Observe structural edits before registering descendants, including linked or missing directories.
      std::ranges::sort(order, [](const auto& left, const auto& right) {
        return left.size() != right.size() ? left.size() < right.size() : left < right;
      });
      for (const auto& path : order) {
        if (g_cancellable_is_cancelled(cancelled)) {
          return;
        }
        std::error_code error;
        if (!fs::is_directory(path, error)) {
          continue;
        }
        const auto wd = inotify.watch(path, kWatchMask);
        if (!wd) {
          complete = false;
          continue;
        }
        auto& active = watches[*wd];
        const auto& watch = desired.at(path);
        active.roles |= watch.roles;
        active.settingsFiles.insert(watch.settingsFiles.begin(), watch.settingsFiles.end());
        active.children.insert(watch.children.begin(), watch.children.end());
      }
    }

    void drain(const std::function<void(bool, bool)>& queue) {
      inotify.drain([&](const inotify_event* event) {
        if ((event->mask & IN_Q_OVERFLOW) != 0) {
          queue(true, true);
          return;
        }
        const auto it = watches.find(event->wd);
        if (it == watches.end()) {
          return;
        }
        const auto& watch = it->second;
        const std::string name = event->len > 0 ? event->name : "";
        const bool entry = (event->mask & kEntryChange) != 0;
        const bool structure = entry && watch.children.contains(name);
        bool content = structure || (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0;
        bool metadata = content;
        if (entry && hasRole(watch.roles, WatchSettingsDir) && watch.settingsFiles.contains(name)) {
          metadata = true;
        }
        if (entry && hasRole(watch.roles, WatchBaseDir)) {
          metadata = true;
        }
        if (entry && hasRole(watch.roles, WatchThemeRoot) && name == "index.theme") {
          content = metadata = true;
        }
        if (entry && hasRole(watch.roles, WatchIconDir) && isIconFile(name)) {
          content = true;
        }
        if (content || metadata) {
          queue(content, metadata);
        }
      });
    }
  };

  void retireWatches(WatchSet* watches) {
    // Removing thousands of kernel watches must also stay off the UI thread.
    auto* task = g_task_new(nullptr, nullptr, nullptr, nullptr);
    g_task_set_task_data(task, watches, nullptr);
    g_task_run_in_thread(
        task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
          delete static_cast<WatchSet*>(data);
          g_task_return_boolean(task, true);
        }
    );
    g_object_unref(task);
  }

  bool samePaths(const IconThemeWatchPaths& left, const IconThemeWatchPaths& right) {
    return left.baseDirs == right.baseDirs
        && left.themeRoots == right.themeRoots
        && left.iconDirs == right.iconDirs
        && left.settingsFiles == right.settingsFiles;
  }

  struct RefreshResult {
    bool changed = false;
    std::shared_ptr<WatchSet> watches;
  };

  void refresh(GTask* task, GCancellable* cancelled, bool initial, bool content) {
    try {
      if (g_task_return_error_if_cancelled(task)) {
        return;
      }
      bool changed = initial || content;
      if (content) {
        IconResolver::invalidateThemeContent();
      } else if (!initial) {
        changed = IconResolver::checkThemeChanged();
      }
      auto result = std::make_unique<RefreshResult>();
      result->changed = changed;
      if (changed) {
        // Before any lookup no icon bytes exist that predate the watches, so comparing signatures is enough.
        const bool unloaded = initial && !content;
        result->changed = !unloaded;
        while (!g_cancellable_is_cancelled(cancelled)) {
          const auto paths = IconResolver::themeWatchPaths();
          auto watches = std::make_unique<WatchSet>(*paths, cancelled);
          if (g_task_return_error_if_cancelled(task)) {
            return;
          }
          if (unloaded && IconResolver::lookupCount() == 0) {
            result->changed = IconResolver::checkThemeChanged() || result->changed;
          } else {
            // Refresh bytes loaded before their watches existed. Ancestors cover edits during this validation.
            IconResolver::invalidateThemeContent();
            result->changed = true;
          }
          if (samePaths(*paths, *IconResolver::themeWatchPaths())) {
            result->watches = std::shared_ptr<WatchSet>(watches.release(), retireWatches);
            break;
          }
        }
      }
      if (!g_task_return_error_if_cancelled(task)) {
        g_task_return_pointer(task, result.release(), +[](gpointer data) { delete static_cast<RefreshResult*>(data); });
      }
    } catch (const std::exception& error) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.what());
    }
  }

  int millisecondsUntil(Clock::time_point deadline, Clock::time_point now) {
    return now >= deadline ? 0 : static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
  }
} // namespace

struct IconThemePollSource::Impl : std::enable_shared_from_this<Impl> {
  std::function<void()> changeCallback;
  std::shared_ptr<WatchSet> active;
  luciole::GioPtr<GCancellable> cancel;
  Clock::time_point nextCheck = Clock::now() + kCheckInterval;
  Clock::time_point debounceUntil;
  bool contentPending = false;
  bool metadataPending = false;
  bool busy = false;
  bool alive = true;
  GSettings* settings = IconResolver::createThemeSettings();

  void start() {
    if (settings != nullptr) {
      g_signal_connect(
          settings, "changed::icon-theme",
          G_CALLBACK(+[](GSettings*, gchar*, gpointer data) { static_cast<Impl*>(data)->queueChange(false, true); }),
          this
      );
      g_free(g_settings_get_string(settings, "icon-theme"));
    }
    launch();
  }

  void close() {
    alive = false;
    changeCallback = {};
    if (settings != nullptr) {
      g_signal_handlers_disconnect_by_data(settings, this);
      g_object_unref(std::exchange(settings, nullptr));
    }
    if (cancel) {
      g_cancellable_cancel(cancel.get());
    }
    active.reset();
  }

  bool periodic() const { return !active || !active->complete; }

  void queueChange(bool content, bool metadata) {
    contentPending = contentPending || content;
    metadataPending = metadataPending || metadata;
    debounceUntil = Clock::now() + kDebounceInterval;
  }

  void drain() {
    if (active) {
      active->drain([this](bool content, bool metadata) { queueChange(content, metadata); });
    }
  }

  void launch() {
    struct Request {
      std::weak_ptr<Impl> owner;
      bool initial;
      bool content;
    };
    const bool initial = !active || !active->complete;
    const bool content = std::exchange(contentPending, false);
    metadataPending = false;
    busy = true;
    cancel.reset(g_cancellable_new());
    auto* task = g_task_new(
        nullptr, cancel.get(),
        +[](GObject*, GAsyncResult* response, gpointer) {
          auto* task = G_TASK(response);
          const auto* request = static_cast<Request*>(g_task_get_task_data(task));
          GError* error = nullptr;
          const std::unique_ptr<RefreshResult> result(
              static_cast<RefreshResult*>(g_task_propagate_pointer(task, &error))
          );
          const auto owner = request->owner.lock();
          if (!owner || !owner->alive) {
            g_clear_error(&error);
            return;
          }
          owner->busy = false;
          owner->cancel.reset();
          if (error != nullptr) {
            kLog.warn("could not refresh icon watches: {}", error->message);
            g_clear_error(&error);
            return;
          }
          if (result->watches) {
            owner->drain();
            owner->active = result->watches;
            owner->drain();
          }
          if (result->changed && owner->changeCallback) {
            const auto callback = owner->changeCallback;
            callback();
          }
        },
        nullptr
    );
    g_task_set_task_data(
        task, new Request{weak_from_this(), initial, content},
        +[](gpointer data) { delete static_cast<Request*>(data); }
    );
    g_task_run_in_thread(
        task, +[](GTask* task, gpointer, gpointer data, GCancellable* cancelled) {
          const auto* request = static_cast<Request*>(data);
          refresh(task, cancelled, request->initial, request->content);
        }
    );
    g_object_unref(task);
  }
};

IconThemePollSource::IconThemePollSource() : m_impl(std::make_shared<Impl>()) { m_impl->start(); }
IconThemePollSource::~IconThemePollSource() { m_impl->close(); }
void IconThemePollSource::setChangeCallback(std::function<void()> callback) {
  m_impl->changeCallback = std::move(callback);
}
bool IconThemePollSource::refreshing() const {
  return m_impl->busy || m_impl->contentPending || m_impl->metadataPending;
}

int IconThemePollSource::pollTimeoutMs() const {
  const auto now = Clock::now();
  int timeout = m_impl->periodic() ? millisecondsUntil(m_impl->nextCheck, now) : -1;
  if (!m_impl->busy && (m_impl->contentPending || m_impl->metadataPending)) {
    const int debounce = millisecondsUntil(m_impl->debounceUntil, now);
    timeout = timeout < 0 ? debounce : std::min(timeout, debounce);
  }
  return timeout;
}

void IconThemePollSource::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_impl->active
      && startIdx < fds.size()
      && fds[startIdx].fd == m_impl->active->inotify.fd()
      && (fds[startIdx].revents & POLLIN) != 0) {
    m_impl->drain();
  }
  const auto now = Clock::now();
  if (m_impl->periodic() && now >= m_impl->nextCheck) {
    m_impl->nextCheck = now + kCheckInterval;
    m_impl->metadataPending = true;
    m_impl->debounceUntil = now;
  }
  if (!m_impl->busy && (m_impl->contentPending || m_impl->metadataPending) && now >= m_impl->debounceUntil) {
    m_impl->launch();
  }
}

void IconThemePollSource::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_impl->active && m_impl->active->inotify.fd() >= 0) {
    fds.push_back({.fd = m_impl->active->inotify.fd(), .events = POLLIN, .revents = 0});
  }
}
