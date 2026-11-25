#pragma once

#include <cstdint>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// One concrete icon-theme directory to search, with the metadata needed for
// size-aware selection. size == 0 means the nominal size is unknown.
struct IconSearchDir {
  std::string path;
  int size = 0;
  bool scalable = false;
  std::string theme;
  int minSize = 0;
  int maxSize = 0;
  // The directory's Context from index.theme (Places, Applications, ...);
  // empty for base and pixmap dirs, which have no index.theme entry.
  std::string context;
  // Whether the directory existed when the plan was built; a plan is rebuilt when one appears.
  bool exists = false;
};

// An icon-theme icon name with the Context of the directory it was first
// seen in (theme preference order); empty for icons found only outside any
// indexed theme directory. Used to offer per-category picker scopes.
struct ThemedIcon {
  std::string name;
  std::string context;
};

struct IconThemeWatchPaths {
  std::vector<std::string> baseDirs;
  std::vector<std::string> themeRoots;
  std::vector<std::string> iconDirs;
  std::vector<std::string> settingsFiles;
};

struct IconThemePlan;

class IconResolver {
public:
  IconResolver();
  explicit IconResolver(bool cacheMissing);

  // targetSize is the intended on-screen pixel size. The first theme in
  // inheritance order that has the icon wins. When > 0, prefer its matching size
  // range and closest nominal drawing, then SVG over PNG at the same size. Fallbacks
  // prefer gentle downscaling to enlargement. targetSize == 0 keeps the legacy
  // "prefer scalable, then largest" behavior for callers that have no size.
  const std::string& resolve(const std::string& iconName, int targetSize = 0);

  // Caller owns the settings object; nullptr when the desktop schema is absent.
  static GSettings* createThemeSettings();
  static bool checkThemeChanged();
  static bool checkThemePlanChanged();
  static void invalidateThemeContent();
  static std::shared_ptr<const IconThemeWatchPaths> themeWatchPaths();
  static std::uint64_t themeGeneration();
  static std::string activeThemeName();
  // Every icon name the active theme chain (plus hicolor/Adwaita fallbacks)
  // can resolve: stems of the .svg/.png files in the plan's search, base and
  // pixmap dirs. Sorted by name, unique. Blocking (walks the theme dirs once per theme generation, then copies the
  // cached list): worker only.
  static std::vector<ThemedIcon> listThemeIcons();
  // The active theme chain's roots when its index.theme sets FollowsColorScheme (KDE), else empty.
  static std::vector<std::string> colorSchemeThemeRoots();
  // Icon lookups performed so far in this process.
  static std::uint64_t lookupCount();

private:
  void rebuild();
  void ensureFresh();
  std::string findIcon(const std::string& name, int targetSize) const;

  std::unordered_map<std::string, std::string> m_cache;
  std::unordered_set<std::string> m_missingCache;
  std::shared_ptr<const IconThemePlan> m_plan;
  std::string m_empty;
  std::uint64_t m_generation = 0;
  bool m_cacheMissing = false;
};
