#include "system/icon_resolver.h"

#include "core/log.h"
#include "system/freedesktop_key_file.h"
#include "util/string_utils.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <gio/gio.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

struct IconThemePlan {
  std::vector<std::string> baseDirs;
  std::vector<std::string> themeRoots;
  std::vector<IconSearchDir> searchDirs;
  std::vector<std::string> pixmapDirs;
  std::string activeTheme;
  // The active theme's index.theme sets FollowsColorScheme.
  bool followsColorScheme = false;
  std::uint64_t planSignature = 0;
  std::uint64_t contentSignature = 0;
  std::shared_ptr<const IconThemeWatchPaths> watchPaths;
};

namespace {

  constexpr Logger kLog("icon-resolver");

  // findIcon calls in this process; lets the watch setup skip a refresh when nothing was loaded yet.
  std::atomic<std::uint64_t> g_lookups{0};

  // FNV-1a over the signature text; equal inputs give equal values, any byte change a different one.
  struct Fnv64 {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    void add(std::string_view text) {
      for (const unsigned char byte : text) {
        value ^= byte;
        value *= 0x100000001b3ULL;
      }
    }
  };

  void pushUniqueDir(std::vector<IconSearchDir>& dirs, std::unordered_set<std::string>& seen, IconSearchDir dir) {
    if (dir.path.empty()) {
      return;
    }
    if (seen.emplace(dir.path).second) {
      dirs.push_back(std::move(dir));
    }
  }

  // Nominal size encoded in a well-known subdir name like "48x48" or "scalable".
  int sizeFromDirName(std::string_view dirName) {
    if (dirName.contains("scalable")) {
      return 0;
    }
    int size = 0;
    for (char c : dirName) {
      if (c >= '0' && c <= '9') {
        size = size * 10 + (c - '0');
      } else if (size > 0) {
        break;
      }
    }
    return size;
  }

  // Shared across all IconResolver instances, including thread_local ones on
  // script worker threads; every access goes through `mutex`.
  struct IconThemeState {
    std::mutex mutex;
    bool initialized = false;
    std::uint64_t generation = 1;
    std::shared_ptr<const IconThemePlan> plan;
  };

  IconThemeState& iconThemeState() {
    static IconThemeState state;
    return state;
  }

  std::string trimAndUnquote(std::string_view value) { return StringUtils::unquote(StringUtils::trim(value)); }

  void pushUnique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
      return;
    }
    if (!std::ranges::contains(values, value)) {
      values.push_back(std::move(value));
    }
  }

  bool pathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
  }

  bool pathIsDirectory(const fs::path& path) {
    std::error_code ec;
    const bool isDirectory = fs::is_directory(path, ec);
    if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
      static std::mutex logMutex;
      static std::unordered_set<std::string> loggedPaths;
      std::scoped_lock lock(logMutex);
      const std::string pathString = path.string();
      if (loggedPaths.emplace(pathString).second) {
        kLog.warn("cannot access icon directory '{}': {}", pathString, ec.message());
      }
    }
    return isDirectory;
  }

  std::vector<std::string> splitList(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
      const auto next = value.find(separator, start);
      const auto part = next == std::string_view::npos ? value.substr(start) : value.substr(start, next - start);
      const std::string trimmed = trimAndUnquote(part);
      if (!trimmed.empty()) {
        parts.push_back(trimmed);
      }
      if (next == std::string_view::npos) {
        break;
      }
      start = next + 1;
    }
    return parts;
  }

  std::vector<std::string> xdgDataDirs() {
    std::vector<std::string> dirs;

    const char* dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && dataHome[0] != '\0') {
      pushUnique(dirs, dataHome);
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      pushUnique(dirs, std::string(home) + "/.local/share");
    }

    const char* dataDirs = std::getenv("XDG_DATA_DIRS");
    if (dataDirs != nullptr && dataDirs[0] != '\0') {
      for (auto dir : splitList(dataDirs, ':')) {
        pushUnique(dirs, std::move(dir));
      }
    } else {
      pushUnique(dirs, "/usr/local/share");
      pushUnique(dirs, "/usr/share");
    }

    return dirs;
  }

  std::vector<std::string> iconBaseDirs(const std::vector<std::string>& dataDirs) {
    std::vector<std::string> roots;
    if (!dataDirs.empty()) {
      pushUnique(roots, dataDirs.front() + "/icons");
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      pushUnique(roots, std::string(home) + "/.icons");
    }
    for (std::size_t i = dataDirs.empty() ? 0 : 1; i < dataDirs.size(); ++i) {
      pushUnique(roots, dataDirs[i] + "/icons");
    }
    return roots;
  }

  std::vector<std::string> pixmapDirs(const std::vector<std::string>& dataDirs) {
    std::vector<std::string> roots;
    for (const auto& dataDir : dataDirs) {
      pushUnique(roots, dataDir + "/pixmaps");
    }
    return roots;
  }

  void appendPathIdentity(Fnv64& signature, std::string_view kind, const std::string& path) {
    signature.add(kind);
    signature.add(path);
    signature.add(":");
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    signature.add(ec ? "missing" : canonical.native());
    signature.add("\n");
  }

  std::uint64_t planSignatureFor(const IconThemePlan& plan) {
    Fnv64 signature;
    signature.add(plan.activeTheme);
    signature.add(plan.followsColorScheme ? "\nfollows-color-scheme\n" : "\n");
    for (const auto& root : plan.baseDirs) {
      appendPathIdentity(signature, "base:", root);
    }
    for (const auto& root : plan.themeRoots) {
      appendPathIdentity(signature, "theme:", root);
    }
    for (const auto& dir : plan.searchDirs) {
      signature.add("search:");
      signature.add(dir.path);
      signature.add(
          std::format(
              ":{}{}{}:{}:{}\n", dir.size, dir.scalable ? ":scalable:" : ":fixed:", dir.theme, dir.minSize, dir.maxSize
          )
      );
      signature.add("context:");
      signature.add(dir.context);
      signature.add("\n");
    }
    for (const auto& dir : plan.pixmapDirs) {
      appendPathIdentity(signature, "pixmap:", dir);
    }
    return signature.value;
  }

  // Stats every directory once: records which search directories exist, so lookups skip the missing ones,
  // and folds their modification times into the content signature.
  void scanDirectories(IconThemePlan& plan) {
    Fnv64 signature;
    const auto appendDirectory = [&](const std::string& path) {
      signature.add(path);
      signature.add(":");
      std::error_code ec;
      const auto modified = fs::last_write_time(path, ec);
      signature.add(ec ? "missing" : std::to_string(static_cast<std::int64_t>(modified.time_since_epoch().count())));
      signature.add("\n");
      return !ec;
    };
    for (auto& dir : plan.searchDirs) {
      dir.exists = appendDirectory(dir.path);
    }
    for (const auto& dir : plan.baseDirs) {
      appendDirectory(dir);
    }
    for (const auto& dir : plan.pixmapDirs) {
      appendDirectory(dir);
    }
    plan.contentSignature = signature.value;
  }

  struct GSettingsDeleter {
    void operator()(GSettings* settings) const {
      if (settings != nullptr) {
        g_object_unref(settings);
      }
    }
  };

  GSettings* iconSettings() {
    static std::unique_ptr<GSettings, GSettingsDeleter> settings{IconResolver::createThemeSettings()};
    return settings.get();
  }

  std::optional<std::string> readGSettingsIconTheme() {
    GSettings* settings = iconSettings();
    if (settings == nullptr) {
      return std::nullopt;
    }

    gchar* raw = g_settings_get_string(settings, "icon-theme");
    if (raw == nullptr) {
      return std::nullopt;
    }

    std::string value = trimAndUnquote(raw);
    g_free(raw);
    if (value.empty()) {
      return std::nullopt;
    }
    return value;
  }

  fs::path gtkConfigRoot() {
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && *config != '\0') {
      return fs::path(config);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr) {
      return fs::path(home) / ".config";
    }
    return {};
  }

  // Returns theme name candidates in priority order:
  //   1. GSettings (the canonical source used by compositors and other launchers)
  //   2. GTK3 ini file (reliable fallback)
  //   3. GTK4 ini file (may be stale)
  std::vector<std::string> readGtkThemeCandidates() {
    std::vector<std::string> candidates;

    if (auto value = readGSettingsIconTheme(); value.has_value()) {
      candidates.emplace_back(std::move(*value));
    }

    // 2. GTK ini files
    const auto configRoot = gtkConfigRoot();
    if (!configRoot.empty()) {
      for (const char* cfg : {"gtk-3.0/settings.ini", "gtk-4.0/settings.ini"}) {
        const fs::path path = configRoot / cfg;
        const auto parsed = freedesktop::parseKeyFile(path);
        if (!parsed) {
          if (parsed.error().reason != freedesktop::ParseFailure::NotFound) {
            kLog.warn("failed to parse GTK settings '{}': {}", path.string(), parsed.error().message);
          }
          continue;
        }
        for (const auto& diagnostic : parsed->diagnostics) {
          kLog.debug(
              "ignoring malformed GTK settings entry '{}:{}: {}'", path.string(), diagnostic.line, diagnostic.message
          );
        }
        if (const auto value = parsed->file.value("Settings", "gtk-icon-theme-name"); value.has_value()) {
          const std::string theme = trimAndUnquote(*value);
          if (!theme.empty()) {
            candidates.emplace_back(theme);
          }
        }
      }
    }

    return candidates;
  }

  // KDE's FollowsColorScheme key in the first index.theme of `themeName`: its SVGs take the desktop color scheme.
  bool themeFollowsColorScheme(const std::vector<std::string>& baseDirs, const std::string& themeName) {
    for (const auto& base : baseDirs) {
      if (const auto parsed = freedesktop::parseKeyFile(fs::path(base) / themeName / "index.theme")) {
        return parsed->file.value("Icon Theme", "FollowsColorScheme") == "true";
      }
    }
    return false;
  }

  // Parse index.theme and return (subdir paths sorted by preference, parent theme names).
  // Preference: scalable/large dirs first so we get crisp icons at any size.
  std::pair<std::vector<IconSearchDir>, std::vector<std::string>> parseIndexTheme(const std::string& themeRoot) {
    const fs::path indexPath = fs::path(themeRoot) / "index.theme";
    const auto parsed = freedesktop::parseKeyFile(indexPath);
    if (!parsed) {
      if (parsed.error().reason != freedesktop::ParseFailure::NotFound) {
        kLog.warn("failed to parse icon theme metadata '{}': {}", indexPath.string(), parsed.error().message);
      }
      return {};
    }
    for (const auto& diagnostic : parsed->diagnostics) {
      kLog.debug(
          "ignoring malformed index.theme entry '{}:{}: {}'", indexPath.string(), diagnostic.line, diagnostic.message
      );
    }

    struct DirEntry {
      std::string path;
      int size = 0;
      int maxSize = 0;
      int minSize = 0;
      int threshold = 2;
      std::string type = "Threshold";
      std::string context;
      bool scalable = false;
    };

    std::vector<std::string> dirNames;
    std::vector<std::string> inherits;
    std::unordered_map<std::string, DirEntry> dirMap;

    if (const auto directories = parsed->file.value("Icon Theme", "Directories"); directories.has_value()) {
      for (auto name : splitList(*directories, ',')) {
        dirNames.emplace_back(name);
        dirMap[std::move(name)].path = dirNames.back();
      }
    }
    if (const auto inherited = parsed->file.value("Icon Theme", "Inherits"); inherited.has_value()) {
      for (auto name : splitList(*inherited, ',')) {
        inherits.emplace_back(std::move(name));
      }
    }

    for (const auto& name : dirNames) {
      const auto* section = parsed->file.section(name);
      if (section == nullptr) {
        continue;
      }
      auto& entry = dirMap[name];
      if (const auto it = section->find("Size"); it != section->end()) {
        try {
          entry.size = std::stoi(it->second);
        } catch (...) {
        }
      }
      if (const auto it = section->find("Type"); it != section->end()) {
        entry.type = it->second;
        entry.scalable = (entry.type == "Scalable" || entry.type == "Threshold");
      }
      if (const auto it = section->find("Context"); it != section->end()) {
        entry.context = it->second;
      }
      if (const auto it = section->find("MaxSize"); it != section->end()) {
        try {
          entry.maxSize = std::stoi(it->second);
        } catch (...) {
        }
      }
      entry.minSize = entry.size;
      if (const auto it = section->find("MinSize"); it != section->end()) {
        try {
          entry.minSize = std::stoi(it->second);
        } catch (...) {
        }
      }
      if (const auto it = section->find("Threshold"); it != section->end()) {
        try {
          entry.threshold = std::max(0, std::stoi(it->second));
        } catch (...) {
        }
      }
    }

    // Sort dirs: scalable first, then by size descending (MaxSize first, then use Size as a tiebreaker)
    std::ranges::stable_sort(dirNames, [&](const std::string& a, const std::string& b) {
      const auto& da = dirMap[a];
      const auto& db = dirMap[b];
      if (da.scalable != db.scalable) {
        return da.scalable > db.scalable;
      }
      if (da.maxSize != db.maxSize) {
        return da.maxSize > db.maxSize;
      }
      return da.size > db.size;
    });

    std::vector<IconSearchDir> sortedPaths;
    sortedPaths.reserve(dirNames.size());
    for (const auto& name : dirNames) {
      const auto& entry = dirMap[name];
      int minSize = entry.size;
      int maxSize = entry.size;
      if (entry.type == "Threshold") {
        minSize = std::max(1, entry.size - entry.threshold);
        maxSize = entry.size + entry.threshold;
      } else if (entry.type == "Scalable") {
        minSize = entry.minSize;
        maxSize = entry.maxSize > 0 ? entry.maxSize : entry.size;
      }
      sortedPaths.push_back(
          IconSearchDir{
              .path = name,
              .size = entry.size,
              .scalable = entry.scalable,
              .theme = {},
              .minSize = minSize,
              .maxSize = std::max(minSize, maxSize),
              .context = entry.context
          }
      );
    }

    return {sortedPaths, inherits};
  }

  void buildThemeSearchPaths(
      const std::string& themeName, const std::vector<std::string>& baseDirs, std::set<std::string>& visited,
      std::vector<std::string>& themeRoots, std::vector<IconSearchDir>& searchDirs,
      std::unordered_set<std::string>& seenDirs
  ) {
    if (visited.contains(themeName)) {
      return;
    }
    visited.insert(themeName);

    std::vector<std::string> roots;
    std::vector<IconSearchDir> dirs;
    std::vector<std::string> parents;
    for (const auto& base : baseDirs) {
      const std::string themeRoot = std::format("{}/{}", base, themeName);
      if (!pathIsDirectory(themeRoot)) {
        continue;
      }
      pushUnique(themeRoots, themeRoot);
      roots.push_back(themeRoot);
      if (dirs.empty()) {
        auto [parsedDirs, inherits] = parseIndexTheme(themeRoot);
        if (!parsedDirs.empty()) {
          dirs = std::move(parsedDirs);
          parents = std::move(inherits);
        } else {
          for (const auto& parent : inherits) {
            pushUnique(parents, parent);
          }
        }
      }
    }

    // The first index.theme describes every root of this logical theme.
    for (const auto& themeRoot : roots) {
      if (dirs.empty()) {
        // No index.theme , fall back to common paths so the theme isn't silently skipped
        for (const char* path :
             {"/scalable/apps/", "/512x512/apps/", "/256x256/apps/", "/128x128/apps/", "/64x64/apps/", "/48x48/apps/",
              "/32x32/apps/", "/"}) {
          const std::string_view name(path);
          pushUniqueDir(
              searchDirs, seenDirs,
              IconSearchDir{
                  .path = themeRoot + path,
                  .size = sizeFromDirName(name),
                  .scalable = name.contains("scalable"),
                  .theme = themeName,
                  .minSize = sizeFromDirName(name),
                  .maxSize = sizeFromDirName(name),
                  .context = {}
              }
          );
        }
      } else {
        for (const auto& dir : dirs) {
          pushUniqueDir(
              searchDirs, seenDirs,
              IconSearchDir{
                  .path = themeRoot + "/" + dir.path + "/",
                  .size = dir.size,
                  .scalable = dir.scalable,
                  .theme = themeName,
                  .minSize = dir.minSize,
                  .maxSize = dir.maxSize,
                  .context = dir.context
              }
          );
        }
      }
    }
    for (const auto& parent : parents) {
      buildThemeSearchPaths(parent, baseDirs, visited, themeRoots, searchDirs, seenDirs);
    }
  }

  IconThemePlan buildThemePlan() {
    IconThemePlan plan;
    const auto dataDirs = xdgDataDirs();
    plan.baseDirs = iconBaseDirs(dataDirs);
    plan.pixmapDirs = pixmapDirs(dataDirs);

    std::set<std::string> visited;
    std::unordered_set<std::string> seenDirs;

    // Use the first candidate theme that actually exists on disk
    for (const auto& candidate : readGtkThemeCandidates()) {
      bool exists = false;
      for (const auto& base : plan.baseDirs) {
        if (pathIsDirectory(std::format("{}/{}", base, candidate))) {
          exists = true;
          break;
        }
      }
      if (exists) {
        plan.activeTheme = candidate;
        plan.followsColorScheme = themeFollowsColorScheme(plan.baseDirs, candidate);
        buildThemeSearchPaths(candidate, plan.baseDirs, visited, plan.themeRoots, plan.searchDirs, seenDirs);
        break;
      }
    }

    // hicolor is the mandatory base theme , always include it last
    buildThemeSearchPaths("hicolor", plan.baseDirs, visited, plan.themeRoots, plan.searchDirs, seenDirs);
    buildThemeSearchPaths("Adwaita", plan.baseDirs, visited, plan.themeRoots, plan.searchDirs, seenDirs);
    plan.planSignature = planSignatureFor(plan);
    scanDirectories(plan);
    auto paths = std::make_shared<IconThemeWatchPaths>(IconThemeWatchPaths{
        .baseDirs = plan.baseDirs,
        .themeRoots = plan.themeRoots,
        .iconDirs = plan.pixmapDirs,
        .settingsFiles = {},
    });
    paths->iconDirs.reserve(paths->iconDirs.size() + plan.searchDirs.size());
    std::unordered_set<std::string> seenPaths(paths->iconDirs.begin(), paths->iconDirs.end());
    seenPaths.reserve(paths->iconDirs.capacity());
    for (const auto& dir : plan.searchDirs) {
      if (seenPaths.emplace(dir.path).second) {
        paths->iconDirs.push_back(dir.path);
      }
    }
    const auto configRoot = gtkConfigRoot();
    if (!configRoot.empty()) {
      paths->settingsFiles.push_back((configRoot / "gtk-3.0/settings.ini").string());
      paths->settingsFiles.push_back((configRoot / "gtk-4.0/settings.ini").string());
    }
    plan.watchPaths = std::move(paths);
    return plan;
  }

  // Requires state.mutex to be held.
  void ensureThemeStateLocked(IconThemeState& state) {
    if (!state.initialized) {
      state.plan = std::make_shared<const IconThemePlan>(buildThemePlan());
      state.initialized = true;
      kLog.debug("active icon theme: {}", state.plan->activeTheme);
    }
  }

  bool updateThemeStateLocked(IconThemeState& state, bool includeContentChanges) {
    IconThemePlan next = buildThemePlan();
    if (!state.initialized) {
      state.plan = std::make_shared<const IconThemePlan>(std::move(next));
      state.initialized = true;
      return false;
    }
    const bool changed = next.planSignature != state.plan->planSignature
        || (includeContentChanges && next.contentSignature != state.plan->contentSignature);
    state.plan = std::make_shared<const IconThemePlan>(std::move(next));
    if (changed) {
      kLog.debug("icon theme refreshed: {}", state.plan->activeTheme);
      ++state.generation;
    }
    return changed;
  }

} // namespace

IconResolver::IconResolver(bool cacheMissing) : m_cacheMissing(cacheMissing) { rebuild(); }

IconResolver::IconResolver() : IconResolver(false) {}

GSettings* IconResolver::createThemeSettings() {
  GSettingsSchemaSource* source = g_settings_schema_source_get_default();
  GSettingsSchema* schema =
      source != nullptr ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
  if (schema == nullptr) {
    kLog.debug("desktop settings schema unavailable; using GTK icon-theme fallback");
    return nullptr;
  }
  GSettings* settings = g_settings_schema_has_key(schema, "icon-theme") != FALSE
      ? g_settings_new_full(schema, nullptr, nullptr)
      : nullptr;
  g_settings_schema_unref(schema);
  return settings;
}

bool IconResolver::checkThemeChanged() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  return updateThemeStateLocked(state, true);
}

bool IconResolver::checkThemePlanChanged() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  return updateThemeStateLocked(state, false);
}

void IconResolver::invalidateThemeContent() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  IconThemePlan next = buildThemePlan();
  state.plan = std::make_shared<const IconThemePlan>(std::move(next));
  state.initialized = true;
  ++state.generation;
}

std::shared_ptr<const IconThemeWatchPaths> IconResolver::themeWatchPaths() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  return state.plan->watchPaths;
}

std::uint64_t IconResolver::themeGeneration() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  return state.generation;
}

std::string IconResolver::activeThemeName() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  return state.plan->activeTheme;
}

std::vector<std::string> IconResolver::colorSchemeThemeRoots() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  return state.plan->followsColorScheme ? state.plan->themeRoots : std::vector<std::string>{};
}

std::uint64_t IconResolver::lookupCount() { return g_lookups.load(std::memory_order_relaxed); }

void IconResolver::rebuild() {
  auto& state = iconThemeState();
  std::scoped_lock lock(state.mutex);
  ensureThemeStateLocked(state);
  m_plan = state.plan;
  m_cache.clear();
  m_missingCache.clear();
  m_generation = state.generation;
}

void IconResolver::ensureFresh() {
  if (m_generation != themeGeneration()) {
    rebuild();
  }
}

const std::string& IconResolver::resolve(const std::string& iconName, int targetSize) {
  if (iconName.empty()) {
    return m_empty;
  }
  ensureFresh();
  const std::string key = iconName + '\x1F' + std::to_string(std::max(0, targetSize));
  auto it = m_cache.find(key);
  if (it != m_cache.end()) {
    if (iconName.front() != '/' || pathExists(it->second)) {
      return it->second;
    }
    m_cache.erase(it);
  }
  const bool canCacheMissing = m_cacheMissing && iconName.front() != '/';
  if (canCacheMissing && m_missingCache.contains(key)) {
    return m_empty;
  }
  auto icon = findIcon(iconName, targetSize);
  if (icon.empty()) {
    if (canCacheMissing) {
      m_missingCache.emplace(key);
    }
    return m_empty;
  }
  auto [ins, _] = m_cache.emplace(key, icon);
  return ins->second;
}

std::string IconResolver::findIcon(const std::string& name, int targetSize) const {
  g_lookups.fetch_add(1, std::memory_order_relaxed);
  // Absolute path , use directly
  if (!name.empty() && name[0] == '/') {
    return pathExists(name) ? name : std::string{};
  }

  if (targetSize <= 0) {
    // Legacy behavior: search dirs are pre-sorted scalable-first then largest;
    // return the first match. Callers with no size budget keep this exactly.
    for (const auto& dir : m_plan->searchDirs) {
      if (!dir.exists) {
        continue;
      }
      for (const char* ext : {".svg", ".png"}) {
        std::string path = dir.path + name + ext;
        if (pathExists(path)) {
          return path;
        }
      }
    }
  } else {
    // Choose the size-specific drawing before its format. SVG variants can have different
    // detail and pixel alignment, so the large scalable drawing is not always the sharpest.
    std::string best;
    std::string_view theme;
    using Score = std::tuple<bool, bool, std::int64_t, std::int64_t, bool>;
    std::optional<Score> bestScore;
    for (const auto& dir : m_plan->searchDirs) {
      if (dir.theme != theme) {
        if (!best.empty()) {
          return best;
        }
        theme = dir.theme;
      }
      if (!dir.exists) {
        continue;
      }
      const std::int64_t minimum = dir.minSize > 0 ? dir.minSize : dir.size;
      const std::int64_t maximum = dir.maxSize > 0 ? dir.maxSize : dir.size;
      const bool matches = minimum > 0 && targetSize >= minimum && targetSize <= maximum;
      const auto distance = dir.size <= 0 ? std::numeric_limits<std::int64_t>::max()
          : targetSize < minimum          ? minimum - targetSize
          : targetSize > maximum          ? targetSize - maximum
                                          : 0;
      const auto nominalDistance = std::abs(static_cast<std::int64_t>(dir.size) - targetSize);
      for (const auto* extension : {".svg", ".png"}) {
        const Score score{
            !matches, maximum < targetSize, distance, nominalDistance, extension == std::string_view(".png")
        };
        if (bestScore && score >= *bestScore) {
          continue;
        }
        std::string path = dir.path + name + extension;
        if (pathExists(path)) {
          best = std::move(path);
          bestScore = score;
        }
      }
    }
    if (!best.empty()) {
      return best;
    }
  }

  // Fallback: unthemed icons at the top of each base dir, then pixmaps (XDG
  // LookupFallbackIcon order).
  for (const auto& dir : m_plan->baseDirs) {
    for (const char* ext : {".svg", ".png"}) {
      std::string path = std::format("{}/{}{}", dir, name, ext);
      if (pathExists(path)) {
        return path;
      }
    }
  }
  for (const auto& dir : m_plan->pixmapDirs) {
    for (const char* ext : {".svg", ".png"}) {
      std::string path = std::format("{}/{}{}", dir, name, ext);
      if (pathExists(path)) {
        return path;
      }
    }
  }

  return {};
}

std::vector<ThemedIcon> IconResolver::listThemeIcons() {
  // A large theme takes a few hundred ms to walk; reopening the picker reuses the list until the theme changes.
  static std::mutex mutex;
  static std::optional<std::pair<std::uint64_t, std::vector<ThemedIcon>>> cached;
  const std::scoped_lock lock(mutex);
  IconResolver resolver;
  resolver.ensureFresh();
  if (cached && cached->first == resolver.m_generation) {
    return cached->second;
  }
  const auto plan = resolver.m_plan;
  if (!plan) {
    return {};
  }
  struct SearchPath {
    std::string path;
    std::string context;
  };
  std::vector<SearchPath> dirs;
  dirs.reserve(plan->searchDirs.size() + plan->baseDirs.size() + plan->pixmapDirs.size());
  for (const auto& dir : plan->searchDirs) {
    if (dir.exists) {
      dirs.push_back({dir.path, dir.context});
    }
  }
  // Base and pixmap dirs carry no trailing slash in the plan; findIcon formats them with one.
  // They have no index.theme context: icons found only there list as unthemed.
  for (const auto& dir : plan->baseDirs) {
    dirs.push_back({dir + '/', {}});
  }
  for (const auto& dir : plan->pixmapDirs) {
    dirs.push_back({dir + '/', {}});
  }
  std::unordered_set<std::string> seen;
  std::vector<ThemedIcon> names;
  std::error_code ec;
  for (const auto& dir : dirs) {
    fs::directory_iterator items(dir.path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    for (const auto& item : items) {
      auto stem = item.path().filename().string();
      if (!stem.ends_with(".svg") && !stem.ends_with(".png")) {
        continue;
      }
      stem.resize(stem.size() - 4);
      // Dedupe before the type check: it stats every symlink, and symlinked themes repeat each name per size.
      if (stem.empty() || seen.contains(stem)) {
        continue;
      }
      if (!item.is_regular_file(ec)) {
        ec.clear();
        continue;
      }
      seen.insert(stem);
      names.push_back({std::move(stem), dir.context});
    }
  }
  std::ranges::sort(names, {}, &ThemedIcon::name);
  cached.emplace(resolver.m_generation, names);
  return names;
}
