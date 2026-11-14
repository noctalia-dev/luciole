#include "browser/places.h"

#include "fs/background_job.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <array>
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
    result.push_back({{.label = "Home", .location = std::move(location)}, "home"});
  }
  for (const auto& special : specials) {
    const char* path = g_get_user_special_dir(special.directory);
    if (path == nullptr || !g_path_is_absolute(path)) {
      continue;
    }
    auto location = Location::fromPath(path);
    if (seen.insert(location.uri()).second) {
      result.push_back({{.label = special.label, .location = std::move(location)}, special.glyph});
    }
  }
  result.push_back({{.label = "Trash", .location = Location::trash()}, "trash"});
  result.push_back({{.label = "File System", .location = Location::fromPath("/")}, "database"});
  return result;
}

struct luciole::PinService::State : std::enable_shared_from_this<State> {
  struct Result {
    bool ready = false;
    std::vector<Bookmark> bookmarks;
    std::vector<SavedWindow> windows;
    std::vector<UserPlace> places;
    std::string error;
  };
  explicit State(std::filesystem::path path) : store(std::move(path)) {}
  StateStore store; // Accessed only by serial worker tasks.
  std::vector<Bookmark> bookmarks;
  std::vector<SavedWindow> windows;
  std::vector<UserPlace> places;
  std::string error;
  std::function<void()> changed;
  bool ready = false;
  bool busy = false;

  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void
  run(std::optional<std::vector<Bookmark>> proposed, std::optional<std::vector<SavedWindow>> proposedWindows = {}) {
    busy = true;
    error.clear();
    runJob<Result>(
        [owner = shared_from_this(), proposed = std::move(proposed), proposedWindows = std::move(proposedWindows)] {
          Result result;
          auto& store = owner->store;
          try {
            result.places = configuredUserPlaces();
            if (proposed) {
              const auto outcome = store.saveBookmarks(*proposed);
              if (!store.issues().empty()) {
                result.error = store.issues().front().format();
              }
              result.ready = outcome != AtomicWriteState::NotCommitted;
              if (!result.ready) {
                result.ready = store.load();
              }
            } else if (proposedWindows) {
              const auto outcome = store.saveWindows(*proposedWindows);
              if (!store.issues().empty()) {
                result.error = store.issues().front().format();
              }
              result.ready = outcome != AtomicWriteState::NotCommitted;
              if (!result.ready) {
                result.ready = store.load();
              }
            } else {
              result.ready = store.load();
              if (!result.ready && !store.issues().empty()) {
                result.error = store.issues().front().format();
              }
            }
            result.bookmarks = store.state().bookmarks;
            result.windows = store.state().windows;
            if (!store.state().placesCustomized) {
              auto available = StateStore::bookmarkLimit - result.bookmarks.size();
              std::vector<Bookmark> initial;
              for (const auto& place : result.places) {
                const auto existing = std::ranges::find(result.bookmarks, place.bookmark.location, &Bookmark::location);
                if (existing != result.bookmarks.end()) {
                  initial.push_back(*existing);
                } else if (available != 0) {
                  initial.push_back(place.bookmark);
                  --available;
                }
              }
              for (const auto& bookmark : result.bookmarks) {
                if (std::ranges::none_of(initial, [&](const auto& pin) { return pin.location == bookmark.location; })) {
                  initial.push_back(bookmark);
                }
              }
              result.bookmarks = std::move(initial);
            }
            if (std::ranges::none_of(result.bookmarks, [](const auto& pin) {
                  return pin.location == Location::trash();
                })) {
              result.bookmarks.push_back({"Trash", Location::trash()});
            }
          } catch (const std::exception& error) {
            result.error = error.what();
          }
          return result;
        },
        [owner = shared_from_this()](JobResult<Result> loaded) {
          if (!loaded) {
            Result failed;
            failed.error = loaded.error().message;
            loaded = std::move(failed);
          }
          owner->busy = false;
          owner->ready = loaded->ready;
          owner->bookmarks = std::move(loaded->bookmarks);
          owner->windows = std::move(loaded->windows);
          owner->places = std::move(loaded->places);
          owner->error = std::move(loaded->error);
          owner->notify();
        }
    );
    notify();
  }
};

luciole::PinService::PinService(std::filesystem::path path) : m_state(std::make_shared<State>(std::move(path))) {}
luciole::PinService::~PinService() { m_state->changed = {}; }
void luciole::PinService::setChangedCallback(std::function<void()> callback) { m_state->changed = std::move(callback); }
void luciole::PinService::reload() {
  if (!m_state->busy) {
    m_state->run(std::nullopt);
  }
}
bool luciole::PinService::editable() const { return m_state->ready && !m_state->busy; }
bool luciole::PinService::busy() const { return m_state->busy; }
const std::vector<luciole::Bookmark>& luciole::PinService::bookmarks() const { return m_state->bookmarks; }
const std::vector<luciole::UserPlace>& luciole::PinService::userPlaces() const { return m_state->places; }
const std::string& luciole::PinService::error() const { return m_state->error; }
bool luciole::PinService::contains(const Location& location) const {
  return std::ranges::any_of(m_state->bookmarks, [&](const auto& pin) { return pin.location == location; });
}
bool luciole::PinService::save(std::vector<Bookmark> bookmarks) {
  if (!editable()) {
    return false;
  }
  if (bookmarks != m_state->bookmarks) {
    m_state->run(std::move(bookmarks));
  }
  return true;
}
bool luciole::PinService::saveWindows(std::vector<SavedWindow> windows) {
  if (!editable()) {
    return false;
  }
  if (windows != m_state->windows) {
    m_state->run(std::nullopt, std::move(windows));
  }
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
  if (std::ranges::any_of(locations, [](const auto& location) { return location.isTrash(); })) {
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
  if (!editable() || expected.location.isTrash() || bookmarkLabelProblem(label)) {
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
