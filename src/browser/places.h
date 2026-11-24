#pragma once

#include "config/shared_bookmarks.h"
#include "config/state_store.h"

#include <functional>
#include <memory>
#include <optional>

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
    // Whether a separator can go in front of row `before`: a gap inside the list that has none yet.
    bool canAddSeparator(std::size_t before) const;
  };

  // Whether the flat Places list shows a row; Settings can hide Network.
  bool placeShown(const Bookmark& place, bool showNetwork);

  // The gaps between the Places rows the sidebar shows, which is where separator lines go. A separator saved in
  // front of a hidden row is drawn in front of the next shown row, and a gap draws at most one line, so the menus
  // and dragging work on what is on screen. Rows here are rows of PinService::bookmarks().
  class SeparatorGaps {
  public:
    SeparatorGaps(const PinService& pins, bool showNetwork);
    // The shown row that the separator saved in front of row `before` is drawn above, if it is drawn.
    std::optional<std::size_t> gap(std::size_t before) const;
    // The separator drawn above shown row `row`, as the row it is saved in front of.
    std::optional<std::size_t> separatorAbove(std::size_t row) const;
    // Whether a separator can go above shown row `row`: not the first shown row, and no line there yet.
    bool free(std::size_t row) const;
    // The shown row after or before row `row`.
    std::optional<std::size_t> nextShown(std::size_t row) const;
    std::optional<std::size_t> previousShown(std::size_t row) const;

  private:
    std::vector<std::size_t> m_shown;
    std::vector<std::size_t> m_separators;
  };
} // namespace luciole
