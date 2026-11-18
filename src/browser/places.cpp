#include "browser/places.h"

#include "fs/background_job.h"
#include "fs/directory_watch.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <array>
#include <deque>
#include <glib.h>
#include <unordered_set>
#include <utility>

namespace {
  std::string pinLabel(const std::string& input) {
    const luciole::GString valid(g_utf8_make_valid(input.data(), static_cast<gssize>(input.size())));
    std::string label(valid.get());
    for (auto& c : label) {
      if (static_cast<unsigned char>(c) < 32 || c == 127) {
        c = ' ';
      }
    }
    if (label.size() > 256) {
      auto end = std::size_t{256};
      while ((static_cast<unsigned char>(label[end]) & 0xc0U) == 0x80U) {
        --end;
      }
      label.resize(end);
    }
    return label.find_first_not_of(' ') == std::string::npos ? "Folder" : label;
  }
} // namespace

std::vector<luciole::UserPlace> luciole::configuredUserPlaces() {
  struct Special {
    GUserDirectory directory;
    const char* label;
    const char* glyph;
  };
  constexpr std::array specials{
      Special{G_USER_DIRECTORY_DESKTOP, "Desktop", "device-desktop"},
      Special{G_USER_DIRECTORY_DOCUMENTS, "Documents", "file-text"},
      Special{G_USER_DIRECTORY_DOWNLOAD, "Downloads", "download"},
      Special{G_USER_DIRECTORY_MUSIC, "Music", "music"},
      Special{G_USER_DIRECTORY_PICTURES, "Pictures", "photo"},
      Special{G_USER_DIRECTORY_VIDEOS, "Videos", "video"},
      Special{G_USER_DIRECTORY_PUBLIC_SHARE, "Public", "users"},
      Special{G_USER_DIRECTORY_TEMPLATES, "Templates", "file-plus"},
  };
  std::vector<UserPlace> result;
  std::unordered_set<std::string> seen{Location::fromPath("/").uri()};
  if (const char* home = g_get_home_dir(); home != nullptr && g_path_is_absolute(home)) {
    auto location = Location::fromPath(home);
    seen.insert(location.uri());
    result.push_back({{.label = "Home", .location = std::move(location), .iconName = {}}, "home"});
  }
  for (const auto& special : specials) {
    const char* path = g_get_user_special_dir(special.directory);
    if (path == nullptr || !g_path_is_absolute(path)) {
      continue;
    }
    auto location = Location::fromPath(path);
    if (seen.insert(location.uri()).second) {
      result.push_back({{.label = special.label, .location = std::move(location), .iconName = {}}, special.glyph});
    }
  }
  result.push_back({{.label = "Trash", .location = Location::trash(), .iconName = {}}, "trash"});
  result.push_back({{.label = "Network", .location = Location::network(), .iconName = {}}, "network"});
  return result;
}

struct luciole::PinService::State : std::enable_shared_from_this<State> {
  struct Result {
    bool ready = false;
    std::vector<Bookmark> bookmarks;
    std::vector<SavedWindow> windows;
    std::vector<UserPlace> places;
    std::string error;
    std::vector<std::filesystem::path> watchPaths;
  };
  struct Work {
    std::optional<std::vector<Bookmark>> bookmarks;
    std::optional<std::vector<SavedWindow>> windows;
    std::vector<Bookmark> expected;
  };
  State(std::filesystem::path path, std::filesystem::path sharedPath, std::filesystem::path legacyPath)
      : store(std::move(path)), shared(sharedPath, legacyPath), sharedPath(std::move(sharedPath)),
        legacyPath(std::move(legacyPath)) {}
  StateStore store; // Accessed only by serial worker tasks.
  SharedBookmarks shared;
  std::filesystem::path sharedPath;
  std::filesystem::path legacyPath;
  std::vector<std::unique_ptr<DirectoryWatch>> watches;
  guint reloadSource = 0;
  bool reloadPending = false;
  bool active = true;
  std::vector<std::filesystem::path> resolvedPaths;
  std::vector<Bookmark> bookmarks;
  std::vector<SavedWindow> windows;
  std::vector<UserPlace> places;
  std::string error;
  std::string sessionError;
  std::deque<Work> queue;
  std::function<void()> changed;
  bool ready = false;
  bool busy = false;
  bool pinBusy = false;

  void stop() {
    active = false;
    changed = {};
    watches.clear();
    if (reloadSource != 0) {
      g_source_remove(std::exchange(reloadSource, 0));
    }
  }
  void watch() {
    if (!active) {
      return;
    }
    auto targets = resolvedPaths;
    targets.push_back(sharedPath);
    targets.push_back(legacyPath);
    std::unordered_set<std::string> paths;
    for (const auto& target : targets) {
      for (auto path = target.parent_path(); !path.empty(); path = path.parent_path()) {
        paths.insert(path.native());
        if (path == path.root_path()) {
          break;
        }
      }
    }
    std::vector<std::unique_ptr<DirectoryWatch>> next;
    for (const auto& path : paths) {
      auto monitor = DirectoryWatch::start(path, [weak = weak_from_this(), targets](const auto& changed) {
        if (auto owner = weak.lock()) {
          if (std::ranges::any_of(targets, [&](const auto& target) {
                return changed == target || target.native().starts_with(changed.native() + '/');
              })) {
            owner->scheduleReload();
          }
        }
      });
      if (monitor) {
        next.push_back(std::move(monitor));
      }
    }
    watches = std::move(next);
  }
  void scheduleReload() {
    if (!active || reloadSource != 0) {
      return;
    }
    reloadSource = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 50,
        +[](gpointer data) -> gboolean {
          if (auto owner = static_cast<std::weak_ptr<State>*>(data)->lock()) {
            owner->reloadSource = 0;
            owner->requestReload();
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        +[](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
  }
  void requestReload() {
    if (busy) {
      reloadPending = true;
    } else {
      run(std::nullopt);
    }
  }

  static bool builtin(const Location& location, const std::vector<UserPlace>& places) {
    return std::ranges::any_of(places, [&](const auto& place) { return place.bookmark.location == location; });
  }
  static std::vector<Bookmark> layout(const SessionState& state, const std::vector<UserPlace>& places) {
    auto result = state.bookmarks;
    if (!state.placesCustomized) {
      std::vector<Bookmark> initial;
      for (const auto& place : places) {
        const auto existing = std::ranges::find(result, place.bookmark.location, &Bookmark::location);
        initial.push_back(existing != result.end() ? *existing : place.bookmark);
      }
      for (const auto& bookmark : result) {
        if (!builtin(bookmark.location, places)) {
          initial.push_back(bookmark);
        }
      }
      result = std::move(initial);
    }
    if (std::ranges::none_of(result, [](const auto& pin) { return pin.location == Location::trash(); })) {
      result.push_back({"Trash", Location::trash(), {}});
    }
    if (std::ranges::none_of(result, [](const auto& pin) { return pin.location == Location::network(); })) {
      result.push_back({"Network", Location::network(), {}});
    }
    return result;
  }
  static std::vector<Bookmark> combine(
      const std::vector<Bookmark>& local, const std::vector<Bookmark>& shared, const std::vector<UserPlace>& places
  ) {
    std::vector<Bookmark> result;
    std::vector<Bookmark> custom;
    for (const auto& pin : shared) {
      if (!builtin(pin.location, places)) {
        auto& bookmark = custom.emplace_back(pin);
        if (const auto previous = std::ranges::find(local, pin.location, &Bookmark::location);
            previous != local.end()) {
          bookmark.iconName = previous->iconName;
        }
      }
    }
    auto next = custom.begin();
    for (const auto& pin : local) {
      if (builtin(pin.location, places)) {
        const auto external = std::ranges::find(shared, pin.location, &Bookmark::location);
        auto merged = external != shared.end() && !pin.location.isTrash() ? *external : pin;
        merged.iconName = pin.iconName;
        result.push_back(std::move(merged));
      } else if (next != custom.end()) {
        result.push_back(*next++);
      }
    }
    result.insert(result.end(), next, custom.end());
    for (const auto& pin : shared) {
      if (builtin(pin.location, places)
          && std::ranges::none_of(result, [&](const auto& existing) { return existing.location == pin.location; })) {
        result.push_back(pin);
      }
    }
    return result;
  }

  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void
  run(std::optional<std::vector<Bookmark>> proposed, std::optional<std::vector<SavedWindow>> proposedWindows = {}) {
    queue.push_back({std::move(proposed), std::move(proposedWindows), bookmarks});
    startNext();
  }
  void startNext() {
    if (busy || queue.empty()) {
      return;
    }
    busy = true;
    auto work = std::move(queue.front());
    queue.pop_front();
    const bool pinWrite = work.bookmarks.has_value();
    const bool sessionWrite = work.windows.has_value();
    if (pinWrite) {
      error.clear();
    }
    if (sessionWrite) {
      sessionError.clear();
    }
    runJob<Result>(
        [owner = shared_from_this(), proposed = std::move(work.bookmarks), proposedWindows = std::move(work.windows),
         expected = std::move(work.expected)] mutable {
          Result result;
          result.bookmarks = expected;
          auto& store = owner->store;
          auto& shared = owner->shared;
          try {
            result.places = configuredUserPlaces();
            if (proposedWindows) {
              store.saveWindows(*proposedWindows);
              if (!store.issues().empty()) {
                result.error = store.issues().front().format();
              }
              result.windows = store.state().windows;
              result.ready = true;
              return result;
            }
            if (!store.load()) {
              result.error = store.issues().front().format();
              return result;
            }
            result.windows = store.state().windows;
            if (!shared.load()) {
              result.error = shared.error();
              return result;
            }
            result.watchPaths = shared.watchPaths();
            auto local = layout(store.state(), result.places);
            result.bookmarks = combine(local, shared.bookmarks(), result.places);
            result.ready = true;
            if (proposed) {
              if (proposed->size() > StateStore::placesLimit) {
                result.error = "Places supports at most 268 rows; remove some bookmarks before adding more.";
                return result;
              }
              if (result.bookmarks != expected) {
                result.error = "Places changed in another application; review the updated list and retry.";
                return result;
              }
              std::vector<Bookmark> published;
              for (const auto& pin : *proposed) {
                if (!builtin(pin.location, result.places)
                    || std::ranges::any_of(shared.bookmarks(), [&](const auto& existing) {
                         return existing.location == pin.location;
                       })) {
                  published.push_back(pin);
                }
              }
              const auto written = shared.save(published);
              if (written.state == AtomicWriteState::NotCommitted) {
                result.error = written.error;
                if (shared.load()) {
                  result.bookmarks = combine(local, shared.bookmarks(), result.places);
                }
                return result;
              }
              result.error = written.error;
              const auto saved = store.saveBookmarks(*proposed);
              if (saved == AtomicWriteState::NotCommitted) {
                // The shared commit stands even if the separate local layout could not be saved.
                result.error = store.issues().front().format();
                store.load();
                result.bookmarks = combine(layout(store.state(), result.places), shared.bookmarks(), result.places);
              } else {
                result.bookmarks = std::move(*proposed);
                if (result.error.empty() && !store.issues().empty()) {
                  result.error = store.issues().front().format();
                }
              }
            }
          } catch (const std::exception& error) {
            result.error = error.what();
          }
          return result;
        },
        [owner = shared_from_this(), pinWrite, sessionWrite](JobResult<Result> loaded) {
          if (!loaded) {
            Result failed;
            failed.error = loaded.error().message;
            loaded = std::move(failed);
          }
          owner->busy = false;
          if (pinWrite) {
            owner->pinBusy = false;
          }
          owner->ready = loaded->ready;
          owner->bookmarks = std::move(loaded->bookmarks);
          owner->windows = std::move(loaded->windows);
          owner->places = std::move(loaded->places);
          if (sessionWrite) {
            owner->sessionError = std::move(loaded->error);
          } else {
            owner->error = std::move(loaded->error);
          }
          if (owner->active) {
            if (!sessionWrite && loaded->ready && owner->resolvedPaths != loaded->watchPaths) {
              owner->resolvedPaths = std::move(loaded->watchPaths);
              // Re-read after attaching new target watches to cover the read-to-watch handoff.
              owner->reloadPending = true;
            }
            owner->watch();
            if (owner->reloadPending) {
              owner->reloadPending = false;
              owner->run(std::nullopt);
            }
          }
          owner->startNext();
          owner->notify();
        }
    );
    notify();
  }
};

luciole::PinService::PinService(
    std::filesystem::path path, std::filesystem::path bookmarks, std::filesystem::path legacy
)
    : m_state(std::make_shared<State>(std::move(path), std::move(bookmarks), std::move(legacy))) {
  m_state->watch();
}
luciole::PinService::~PinService() { m_state->stop(); }
void luciole::PinService::setChangedCallback(std::function<void()> callback) { m_state->changed = std::move(callback); }
void luciole::PinService::reload() { m_state->requestReload(); }
bool luciole::PinService::editable() const { return m_state->ready && !m_state->pinBusy; }
bool luciole::PinService::busy() const { return m_state->busy; }
const std::vector<luciole::Bookmark>& luciole::PinService::bookmarks() const { return m_state->bookmarks; }
const std::vector<luciole::UserPlace>& luciole::PinService::userPlaces() const { return m_state->places; }
const std::string& luciole::PinService::error() const { return m_state->error; }
const std::string& luciole::PinService::sessionError() const { return m_state->sessionError; }
bool luciole::PinService::contains(const Location& location) const {
  return std::ranges::any_of(m_state->bookmarks, [&](const auto& pin) { return pin.location == location; });
}
bool luciole::PinService::save(std::vector<Bookmark> bookmarks) {
  if (!editable()) {
    return false;
  }
  if (bookmarks != m_state->bookmarks) {
    m_state->pinBusy = true;
    m_state->run(std::move(bookmarks));
  }
  return true;
}
bool luciole::PinService::saveWindows(std::vector<SavedWindow> windows) {
  if (!m_state->ready) {
    return false;
  }
  m_state->run(std::nullopt, std::move(windows));
  return true;
}
bool luciole::PinService::add(std::vector<Bookmark> bookmarks, std::size_t before) {
  if (!editable() || std::ranges::any_of(bookmarks, [](const auto& bookmark) {
        return bookmark.location.isTrash() && bookmark.location != Location::trash();
      })) {
    return false;
  }
  auto proposed = m_state->bookmarks;
  before = std::min(before, proposed.size());
  std::vector<Bookmark> incoming;
  for (auto& bookmark : bookmarks) {
    if (std::ranges::any_of(incoming, [&](const auto& pin) { return pin.location == bookmark.location; })) {
      continue;
    }
    const auto existing = std::ranges::find(proposed, bookmark.location, &Bookmark::location);
    if (existing != proposed.end()) {
      bookmark.label = existing->label;
      bookmark.iconName = existing->iconName;
      if (static_cast<std::size_t>(existing - proposed.begin()) < before) {
        --before;
      }
      proposed.erase(existing);
    } else {
      const auto known = std::ranges::find_if(m_state->places, [&](const auto& place) {
        return place.bookmark.location == bookmark.location;
      });
      bookmark.label = known == m_state->places.end() ? pinLabel(bookmark.label) : known->bookmark.label;
    }
    incoming.push_back(std::move(bookmark));
  }
  proposed.insert(proposed.begin() + static_cast<std::ptrdiff_t>(before), incoming.begin(), incoming.end());
  return save(std::move(proposed));
}
bool luciole::PinService::remove(const Location& location) { return removeMany({location}); }
bool luciole::PinService::removeMany(const std::vector<Location>& locations) {
  if (std::ranges::any_of(locations, [](const auto& location) {
        return location.isTrash() || location == Location::network();
      })) {
    return false;
  }
  auto proposed = m_state->bookmarks;
  std::erase_if(proposed, [&](const auto& pin) { return std::ranges::contains(locations, pin.location); });
  return save(std::move(proposed));
}
bool luciole::PinService::move(const Location& location, std::size_t position) {
  auto proposed = m_state->bookmarks;
  const auto found = std::ranges::find(proposed, location, &Bookmark::location);
  if (found == proposed.end()) {
    return false;
  }
  auto bookmark = std::move(*found);
  proposed.erase(found);
  position = std::min(position, proposed.size());
  proposed.insert(proposed.begin() + static_cast<std::ptrdiff_t>(position), std::move(bookmark));
  return save(std::move(proposed));
}
bool luciole::PinService::rename(const Bookmark& expected, std::string label) {
  if (!editable()
      || expected.location.isTrash()
      || expected.location == Location::network()
      || bookmarkLabelProblem(label)) {
    return false;
  }
  auto proposed = m_state->bookmarks;
  const auto found = std::ranges::find(proposed, expected.location, &Bookmark::location);
  if (found == proposed.end() || found->label != expected.label) {
    return false;
  }
  found->label = std::move(label);
  return save(std::move(proposed));
}
bool luciole::PinService::setIcon(const Bookmark& expected, std::string iconName) {
  if (!editable() || expected.location.isTrash() || expected.location == Location::network()) {
    return false;
  }
  auto proposed = m_state->bookmarks;
  const auto found = std::ranges::find(proposed, expected.location, &Bookmark::location);
  if (found == proposed.end() || found->label != expected.label || found->iconName != expected.iconName) {
    return false;
  }
  found->iconName = std::move(iconName);
  return save(std::move(proposed));
}
