#pragma once

#include "config/shared_bookmarks.h"
#include "config/state_store.h"

#include <functional>
#include <memory>

namespace luciole {
  struct UserPlace {
    Bookmark bookmark;
    std::string glyphName;
  };

  // Read platform user-directory configuration once at startup, outside Render.
  std::vector<UserPlace> configuredUserPlaces();

  class PinService {
  public:
    explicit PinService(
        std::filesystem::path path, std::filesystem::path bookmarks = SharedBookmarks::pathFromEnvironment(),
        std::filesystem::path legacy = SharedBookmarks::legacyFromEnvironment()
    );
    ~PinService();
    PinService(const PinService&) = delete;
    PinService& operator=(const PinService&) = delete;
    void reload();
    bool add(std::vector<Bookmark> bookmarks, std::size_t before);
    bool remove(const Location& location);
    bool removeMany(const std::vector<Location>& locations);
    bool move(const Location& location, std::size_t position);
    bool rename(const Bookmark& expected, std::string label);
    bool setIcon(const Bookmark& expected, std::string iconName);
    // Separator lines between Places rows, each as the row it sits before. Moving a pin keeps them where they are,
    // so the pin crosses them; adding or removing pins keeps each in front of the same row.
    const std::vector<std::size_t>& separators() const;
    bool addSeparator(std::size_t before);
    bool removeSeparator(std::size_t before);
    bool moveSeparator(std::size_t before, bool up);
    // Moves the separator in front of row `before` to sit in front of row `target` instead.
    bool moveSeparatorTo(std::size_t before, std::size_t target);
    bool saveWindows(std::vector<SavedWindow> windows);
    bool contains(const Location& location) const;
    bool editable() const;
    bool busy() const;
    const std::vector<Bookmark>& bookmarks() const;
    const std::vector<UserPlace>& userPlaces() const;
    const std::string& error() const;
    const std::string& sessionError() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
    bool save(std::vector<Bookmark> bookmarks);
    bool save(std::vector<Bookmark> bookmarks, std::vector<std::size_t> separators);
  };
} // namespace luciole
