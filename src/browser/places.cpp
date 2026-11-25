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
    std::vector<std::size_t> separators;
    std::vector<SavedWindow> windows;
    std::vector<UserPlace> places;
    std::string error;
    std::vector<std::filesystem::path> watchPaths;
  };
  struct Work {
    std::optional<std::vector<Bookmark>> bookmarks;
    std::vector<std::size_t> separators;
    std::optional<std::vector<SavedWindow>> windows;
    std::vector<Bookmark> expected;
    // The separators this instance had when the write was queued; another writer may have changed them since.
    std::vector<std::size_t> expectedSeparators;
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
  std::vector<std::size_t> separators;
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
  // Separators are rows of the list they were saved with. When the list has changed since, each follows the row it
  // was in front of, or the first one after it still listed; a separator with no row left below it, or that would
  // sit above the first row or share a gap, is dropped. This covers Luciole's own pin changes and bookmarks changed
  // in another application, which leave the saved rows behind.
  static std::vector<std::size_t> follow(
      const std::vector<std::size_t>& rows, const std::vector<Bookmark>& saved, const std::vector<Bookmark>& current
  ) {
    std::vector<std::size_t> result;
    for (const auto row : rows) {
      for (std::size_t old = row; old < saved.size(); ++old) {
        const auto found = std::ranges::find(current, saved[old].location, &Bookmark::location);
        if (found != current.end()) {
          const auto index = static_cast<std::size_t>(found - current.begin());
          if (index > 0 && (result.empty() || result.back() < index)) {
            result.push_back(index);
          }
          break;
        }
      }
    }
    return result;
  }
  // Separators that sit inside the list: past its last row they have nothing below them.
  static std::vector<std::size_t> within(std::vector<std::size_t> rows, std::size_t count) {
    std::erase_if(rows, [count](std::size_t row) { return row == 0 || row >= count; });
    return rows;
  }
  void
  run(std::optional<std::vector<Bookmark>> proposed, std::optional<std::vector<SavedWindow>> proposedWindows = {},
      std::optional<std::vector<std::size_t>> proposedSeparators = {}) {
    queue.push_back(
        {.bookmarks = std::move(proposed),
         .separators = proposedSeparators ? std::move(*proposedSeparators) : separators,
         .windows = std::move(proposedWindows),
         .expected = bookmarks,
         .expectedSeparators = separators}
    );
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
         expected = std::move(work.expected), separators = std::move(work.separators),
         expectedSeparators = std::move(work.expectedSeparators)] mutable {
          Result result;
          result.bookmarks = expected;
          result.separators = expectedSeparators;
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
            result.separators = follow(store.state().placeSeparators, local, result.bookmarks);
            result.ready = true;
            if (proposed) {
              if (proposed->size() > StateStore::placesLimit) {
                result.error = "Places supports at most 268 rows; remove some bookmarks before adding more.";
                return result;
              }
              // Separators are only in state.toml, which isn't watched, so another instance's change to them is
              // found here rather than by a reload.
              if (result.bookmarks != expected || result.separators != expectedSeparators) {
                result.error = "Places changed in another application; review the updated list and retry.";
                return result;
              }
              if (*proposed == result.bookmarks && separators == result.separators) {
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
              const auto kept = within(separators, proposed->size());
              const auto saved = store.saveBookmarks(*proposed, kept);
              if (saved == AtomicWriteState::NotCommitted) {
                // The shared commit stands even if the separate local layout could not be saved.
                result.error = store.issues().front().format();
                store.load();
                const auto stored = layout(store.state(), result.places);
                result.bookmarks = combine(stored, shared.bookmarks(), result.places);
                result.separators = follow(store.state().placeSeparators, stored, result.bookmarks);
              } else {
                result.bookmarks = std::move(*proposed);
                result.separators = kept;
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
          if (!sessionWrite) {
            owner->separators = std::move(loaded->separators);
          }
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
  // Each separator stays in front of the same row: the first row at or after it that is still listed.
  auto separators = State::follow(m_state->separators, m_state->bookmarks, bookmarks);
  return save(std::move(bookmarks), std::move(separators));
}
bool luciole::PinService::save(std::vector<Bookmark> bookmarks, std::vector<std::size_t> separators) {
  if (!editable()) {
    return false;
  }
  if (bookmarks != m_state->bookmarks || separators != m_state->separators) {
    // A change to the separators alone writes only state.toml, so nothing but the end of the write would redraw
    // Places: show it now, in the frame that closes the menu or ends the drag. The write's result replaces it, so a
    // conflict or a failed write still shows what is on disk. Not when pins change too, since the rows would then
    // refer to the new list while Places still shows the old one.
    const bool separatorsOnly = bookmarks == m_state->bookmarks;
    auto shown = separatorsOnly ? std::optional(separators) : std::nullopt;
    m_state->pinBusy = true;
    m_state->run(std::move(bookmarks), std::nullopt, std::move(separators));
    if (shown) {
      m_state->separators = std::move(*shown);
      m_state->notify();
    }
  }
  return true;
}
const std::vector<std::size_t>& luciole::PinService::separators() const { return m_state->separators; }
bool luciole::PinService::canAddSeparator(std::size_t before) const {
  return before > 0 && before < m_state->bookmarks.size() && !std::ranges::contains(m_state->separators, before);
}
bool luciole::PinService::addSeparator(std::size_t before) {
  if (!canAddSeparator(before)) {
    return false;
  }
  auto separators = m_state->separators;
  separators.insert(std::ranges::upper_bound(separators, before), before);
  return save(m_state->bookmarks, std::move(separators));
}
bool luciole::PinService::removeSeparator(std::size_t before) {
  auto separators = m_state->separators;
  if (std::erase(separators, before) == 0) {
    return false;
  }
  return save(m_state->bookmarks, std::move(separators));
}
bool luciole::PinService::moveSeparatorTo(std::size_t before, std::size_t target) {
  auto separators = m_state->separators;
  const auto found = std::ranges::find(separators, before);
  if (found == separators.end() || !canAddSeparator(target)) {
    return false;
  }
  *found = target;
  std::ranges::sort(separators);
  return save(m_state->bookmarks, std::move(separators));
}
bool luciole::PinService::saveWindows(std::vector<SavedWindow> windows) {
  if (!m_state->ready) {
    return false;
  }
  m_state->run(std::nullopt, std::move(windows));
  return true;
}
bool luciole::PinService::add(std::vector<Bookmark> bookmarks, std::size_t before, bool belowSeparator) {
  if (!editable() || std::ranges::any_of(bookmarks, [](const auto& bookmark) {
        return bookmark.location.isTrash() && bookmark.location != Location::trash();
      })) {
    return false;
  }
  const auto& current = m_state->bookmarks;
  before = std::min(before, current.size());
  std::vector<bool> moved(current.size());
  std::vector<Bookmark> incoming;
  for (auto& bookmark : bookmarks) {
    if (std::ranges::any_of(incoming, [&](const auto& pin) { return pin.location == bookmark.location; })) {
      continue;
    }
    const auto existing = std::ranges::find(current, bookmark.location, &Bookmark::location);
    if (existing != current.end()) {
      bookmark.label = existing->label;
      bookmark.iconName = existing->iconName;
      moved[static_cast<std::size_t>(existing - current.begin())] = true;
    } else {
      const auto known = std::ranges::find_if(m_state->places, [&](const auto& place) {
        return place.bookmark.location == bookmark.location;
      });
      bookmark.label = known == m_state->places.end() ? pinLabel(bookmark.label) : known->bookmark.label;
    }
    incoming.push_back(std::move(bookmark));
  }
  // The rows that stay, then the new or moved pins inserted among them, at the stayed rows above `before`.
  std::vector<Bookmark> proposed;
  std::vector<std::size_t> stayed(current.size() + 1); // How many rows above each row stay.
  for (std::size_t i = 0; i < current.size(); ++i) {
    stayed[i + 1] = stayed[i] + (moved[i] ? 0 : 1);
    if (!moved[i]) {
      proposed.push_back(current[i]);
    }
  }
  const auto at = stayed[before];
  proposed.insert(proposed.begin() + static_cast<std::ptrdiff_t>(at), incoming.begin(), incoming.end());
  // Each separator stays in front of the same staying row. One that ends up at the drop goes below the new pins, or
  // above them for a drop below its line; one left with nothing between it and the one before it is dropped.
  std::vector<std::size_t> separators;
  for (const auto row : m_state->separators) {
    auto gap = stayed[std::min(row, current.size())];
    if (gap > at || (gap == at && !belowSeparator)) {
      gap += incoming.size();
    }
    if (gap > 0 && gap < proposed.size() && (separators.empty() || separators.back() < gap)) {
      separators.push_back(gap);
    }
  }
  return save(std::move(proposed), std::move(separators));
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
  // Separators keep their rows, so a moved pin crosses them.
  return save(std::move(proposed), m_state->separators);
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
bool luciole::placeShown(const Bookmark& place, bool showNetwork) {
  return showNetwork || place.location != Location::network();
}
luciole::SeparatorGaps::SeparatorGaps(const PinService& pins, bool showNetwork) : m_separators(pins.separators()) {
  const auto& rows = pins.bookmarks();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (placeShown(rows[i], showNetwork)) {
      m_shown.push_back(i);
    }
  }
}
std::optional<std::size_t> luciole::SeparatorGaps::gap(std::size_t before) const {
  // Nothing is above the first shown row to separate it from.
  const auto row = std::ranges::lower_bound(m_shown, before);
  if (row == m_shown.end() || row == m_shown.begin()) {
    return std::nullopt;
  }
  return *row;
}
std::optional<std::size_t> luciole::SeparatorGaps::separatorAbove(std::size_t row) const {
  const auto found = std::ranges::find_if(m_separators, [&](std::size_t before) { return gap(before) == row; });
  return found == m_separators.end() ? std::nullopt : std::optional(*found);
}
bool luciole::SeparatorGaps::free(std::size_t row) const { return gap(row) == row && !separatorAbove(row); }
std::optional<std::size_t> luciole::SeparatorGaps::nextShown(std::size_t row) const {
  const auto next = std::ranges::upper_bound(m_shown, row);
  return next == m_shown.end() ? std::nullopt : std::optional(*next);
}
std::optional<std::size_t> luciole::SeparatorGaps::previousShown(std::size_t row) const {
  const auto next = std::ranges::lower_bound(m_shown, row);
  return next == m_shown.begin() ? std::nullopt : std::optional(*std::prev(next));
}
