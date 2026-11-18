#pragma once

#include "config/diagnostic.h"
#include "fs/atomic_file.h"
#include "fs/location.h"

#include <string_view>
#include <vector>

namespace luciole {
  std::optional<std::string> bookmarkLabelProblem(std::string_view label);
  std::optional<std::string> storedUriProblem(const std::string& uri);

  struct Bookmark {
    std::string label;
    Location location;
    // Private Luciole override; the GTK bookmark format has no icon field.
    std::string iconName;
    bool operator==(const Bookmark&) const = default;
  };

  enum class ActivePane : std::uint8_t { Primary, Secondary };
  struct SavedTab {
    Location primary;
    std::optional<Location> secondary;
    ActivePane activePane = ActivePane::Primary;
    bool operator==(const SavedTab&) const = default;
  };

  struct SavedWindow {
    std::vector<SavedTab> tabs;
    std::size_t activeTab = 0;
    bool operator==(const SavedWindow&) const = default;
  };

  struct SessionState {
    std::vector<Bookmark> bookmarks;
    std::vector<SavedWindow> windows;
    bool placesCustomized = false;
    bool operator==(const SessionState&) const = default;
  };

  // Private Places layout and window state; shared bookmarks own user pin contents.
  // Single owning I/O thread; load before saving and marshal snapshots to UI.
  class StateStore {
  public:
    static constexpr std::size_t bookmarkLimit = 256;
    static constexpr std::size_t placesLimit =
        bookmarkLimit + 12; // Home, eight XDG directories, Trash, Network and root.
    explicit StateStore(std::filesystem::path path);
    static std::filesystem::path pathFromEnvironment();
    bool load(); // Failure preserves the last valid snapshot and the file.
    AtomicWriteState saveBookmarks(std::vector<Bookmark> bookmarks);
    AtomicWriteState saveWindows(std::vector<SavedWindow> windows);
    const SessionState& state() const { return m_state; }
    const ConfigIssues& issues() const { return m_issues; }

  private:
    AtomicWriteState save(SessionState proposed, bool bookmarks);
    bool validPath() const;

    std::filesystem::path m_path;
    SessionState m_state;
    ConfigIssues m_issues;
    bool m_loaded = false;
  };
} // namespace luciole
