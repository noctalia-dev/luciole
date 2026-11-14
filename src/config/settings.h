#pragma once

#include "config/diagnostic.h"
#include "fs/atomic_file.h"
#include "model/directory_options.h"
#include "model/thumbnail_options.h"

#include <concepts>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace luciole {
  struct BrowserSettings {
    SortOptions sort;
    ActivationMode activationMode = ActivationMode::DoubleClick;
    TypingMode typingMode = TypingMode::Search;
    bool switchToNewTab = true;
    bool restoreLastWindow = false;
    ViewMode viewMode = ViewMode::List;
    bool showHidden = false;
    bool showBackups = true;
    bool alwaysShowTabBar = true;
    BrowsePresentation presentation;
    int gridIconSize = kDefaultGridIconSize;
    int sidebarWidth = kDefaultSidebarWidth;
    bool showSidebar = true;
    bool sidebarTree = false;
    bool showDrives = true;
    bool expandableFolders = false;
    bool showPreview = true;
    int previewWidth = 320;
    int millerColumnWidth = 260;
    bool millerShowParents = true;
    DetailsColumnVisibility columns{};
    DetailsColumnOrder columnOrder = kDefaultDetailsColumnOrder;
    DetailsColumnWidths columnWidths = kDefaultDetailsColumnWidths;
    ThumbnailOptions thumbnails;
    std::string terminalCommand;
    bool operator==(const BrowserSettings&) const = default;
  };

  struct Settings {
    BrowserSettings browser;
    bool operator==(const Settings&) const = default;
  };

  struct ConfigPaths {
    std::filesystem::path base;
    std::filesystem::path overrides;
    static ConfigPaths fromEnvironment();
  };

  using SettingsResult = std::expected<Settings, ConfigIssues>;

  enum class BrowserPreferenceId : std::uint8_t {
    ActivationMode,
    TypingMode,
    SwitchToNewTab,
    RestoreLastWindow,
    ShowHidden,
    ShowBackups,
    AlwaysShowTabBar,
    SortBy,
    SortDescending,
    DirectoriesFirst,
    ViewMode,
    GridIconSize,
    Density,
    ShowSizeCaption,
    ShowTypeCaption,
    ShowModifiedCaption,
    ShowEmblems,
    SidebarWidth,
    ShowSidebar,
    SidebarTree,
    ShowDrives,
    ExpandableFolders,
    ShowPreview,
    PreviewWidth,
    MillerColumnWidth,
    MillerShowParents,
    ShowTypeColumn,
    ShowSizeColumn,
    ShowModifiedColumn,
    ShowAccessedColumn,
    ShowCreatedColumn,
    ShowOwnerColumn,
    ShowGroupColumn,
    ShowPermissionsColumn,
    ShowLocationColumn,
    ColumnOrder,
    NameColumnWidth,
    TypeColumnWidth,
    SizeColumnWidth,
    ModifiedColumnWidth,
    AccessedColumnWidth,
    CreatedColumnWidth,
    OwnerColumnWidth,
    GroupColumnWidth,
    PermissionsColumnWidth,
    LocationColumnWidth,
    ThumbnailPolicy,
    ThumbnailMaxFileMib,
    ThumbnailMaxMegapixels,
    ThumbnailCacheMib,
    TerminalCommand,
    Count
  };

  // Names one overridable browser setting whose value has type T. Only
  // BrowserPreference creates keys, so a key's type always matches its setting.
  template <class T> class PreferenceKey {
  public:
    using Value = T;
    constexpr BrowserPreferenceId id() const { return m_id; }
    bool operator==(const PreferenceKey&) const = default;

  private:
    friend struct BrowserPreference;
    constexpr explicit PreferenceKey(BrowserPreferenceId id) : m_id(id) {}
    BrowserPreferenceId m_id;
  };

  // The browser.* settings that settings.toml can override. settings.cpp maps
  // each key to its TOML name, codec, bounds, and BrowserSettings field.
  struct BrowserPreference {
    using Id = BrowserPreferenceId;
    static constexpr PreferenceKey<std::string> TerminalCommand{Id::TerminalCommand};
    static constexpr PreferenceKey<luciole::ActivationMode> ActivationMode{Id::ActivationMode};
    static constexpr PreferenceKey<luciole::TypingMode> TypingMode{Id::TypingMode};
    static constexpr PreferenceKey<bool> SwitchToNewTab{Id::SwitchToNewTab};
    static constexpr PreferenceKey<bool> RestoreLastWindow{Id::RestoreLastWindow};
    static constexpr PreferenceKey<bool> ShowHidden{Id::ShowHidden};
    static constexpr PreferenceKey<bool> ShowBackups{Id::ShowBackups};
    static constexpr PreferenceKey<bool> AlwaysShowTabBar{Id::AlwaysShowTabBar};
    static constexpr PreferenceKey<luciole::SortField> SortBy{Id::SortBy};
    static constexpr PreferenceKey<bool> SortDescending{Id::SortDescending};
    static constexpr PreferenceKey<bool> DirectoriesFirst{Id::DirectoriesFirst};
    static constexpr PreferenceKey<luciole::ViewMode> ViewMode{Id::ViewMode};
    static constexpr PreferenceKey<int> GridIconSize{Id::GridIconSize};
    static constexpr PreferenceKey<luciole::ViewDensity> Density{Id::Density};
    static constexpr PreferenceKey<bool> ShowSizeCaption{Id::ShowSizeCaption};
    static constexpr PreferenceKey<bool> ShowTypeCaption{Id::ShowTypeCaption};
    static constexpr PreferenceKey<bool> ShowModifiedCaption{Id::ShowModifiedCaption};
    static constexpr PreferenceKey<bool> ShowEmblems{Id::ShowEmblems};
    static constexpr PreferenceKey<int> SidebarWidth{Id::SidebarWidth};
    static constexpr PreferenceKey<bool> ShowSidebar{Id::ShowSidebar};
    static constexpr PreferenceKey<bool> SidebarTree{Id::SidebarTree};
    static constexpr PreferenceKey<bool> ShowDrives{Id::ShowDrives};
    static constexpr PreferenceKey<bool> ExpandableFolders{Id::ExpandableFolders};
    static constexpr PreferenceKey<bool> ShowPreview{Id::ShowPreview};
    static constexpr PreferenceKey<int> PreviewWidth{Id::PreviewWidth};
    static constexpr PreferenceKey<int> MillerColumnWidth{Id::MillerColumnWidth};
    static constexpr PreferenceKey<bool> MillerShowParents{Id::MillerShowParents};
    static constexpr PreferenceKey<bool> ShowTypeColumn{Id::ShowTypeColumn};
    static constexpr PreferenceKey<bool> ShowSizeColumn{Id::ShowSizeColumn};
    static constexpr PreferenceKey<bool> ShowModifiedColumn{Id::ShowModifiedColumn};
    static constexpr PreferenceKey<bool> ShowAccessedColumn{Id::ShowAccessedColumn};
    static constexpr PreferenceKey<bool> ShowCreatedColumn{Id::ShowCreatedColumn};
    static constexpr PreferenceKey<bool> ShowOwnerColumn{Id::ShowOwnerColumn};
    static constexpr PreferenceKey<bool> ShowGroupColumn{Id::ShowGroupColumn};
    static constexpr PreferenceKey<bool> ShowPermissionsColumn{Id::ShowPermissionsColumn};
    static constexpr PreferenceKey<bool> ShowLocationColumn{Id::ShowLocationColumn};
    static constexpr PreferenceKey<luciole::DetailsColumnOrder> ColumnOrder{Id::ColumnOrder};
    static constexpr PreferenceKey<int> NameColumnWidth{Id::NameColumnWidth};
    static constexpr PreferenceKey<int> TypeColumnWidth{Id::TypeColumnWidth};
    static constexpr PreferenceKey<int> SizeColumnWidth{Id::SizeColumnWidth};
    static constexpr PreferenceKey<int> ModifiedColumnWidth{Id::ModifiedColumnWidth};
    static constexpr PreferenceKey<int> AccessedColumnWidth{Id::AccessedColumnWidth};
    static constexpr PreferenceKey<int> CreatedColumnWidth{Id::CreatedColumnWidth};
    static constexpr PreferenceKey<int> OwnerColumnWidth{Id::OwnerColumnWidth};
    static constexpr PreferenceKey<int> GroupColumnWidth{Id::GroupColumnWidth};
    static constexpr PreferenceKey<int> PermissionsColumnWidth{Id::PermissionsColumnWidth};
    static constexpr PreferenceKey<int> LocationColumnWidth{Id::LocationColumnWidth};
    static constexpr PreferenceKey<luciole::ThumbnailPolicy> ThumbnailPolicy{Id::ThumbnailPolicy};
    static constexpr PreferenceKey<int> ThumbnailMaxFileMib{Id::ThumbnailMaxFileMib};
    static constexpr PreferenceKey<int> ThumbnailMaxMegapixels{Id::ThumbnailMaxMegapixels};
    static constexpr PreferenceKey<int> ThumbnailCacheMib{Id::ThumbnailCacheMib};
  };

  // The BrowserSettings field a preference reads and writes.
  template <class T> T& preferenceValue(BrowserSettings& settings, PreferenceKey<T> key);
  template <class T> const T& preferenceValue(const BrowserSettings& settings, PreferenceKey<T> key);

  // One settings.toml change. std::nullopt removes the override so the base
  // file or the default applies again.
  class OverrideEdit {
  public:
    template <class T, std::same_as<T> V>
    OverrideEdit(PreferenceKey<T> key, V value) : m_preference(key.id()), m_value(std::in_place_type<T>, value) {}
    template <class T> OverrideEdit(PreferenceKey<T> key, std::nullopt_t) : m_preference(key.id()) {}

    BrowserPreferenceId preference() const { return m_preference; }
    // The new value, or nullopt when the edit restores inheritance or names another preference.
    template <class T> std::optional<T> value(PreferenceKey<T> key) const {
      const auto* value = key.id() == m_preference ? std::get_if<T>(&m_value) : nullptr;
      return value != nullptr ? std::optional<T>(*value) : std::nullopt;
    }

  private:
    BrowserPreferenceId m_preference;
    std::variant<
        std::monostate, bool, int, SortField, ViewMode, ViewDensity, DetailsColumnOrder, ActivationMode, TypingMode,
        ThumbnailPolicy, std::string>
        m_value;
  };

  // Edits that remove every override.
  std::vector<OverrideEdit> inheritAllPreferences();

  // Bounded, read-only local file loading. Call outside Render and traversal jobs.
  // Validation reads exactly one required file, without adjacent override files.
  ConfigIssues validateConfigFile(const std::filesystem::path& file);
  // Runtime loads warn about unknown keys and retain recognized settings; known-value errors still fail.
  SettingsResult loadSettings(const ConfigPaths& paths);
  SettingsResult loadInheritedSettings(const ConfigPaths& paths);

  class ConfigService {
  public:
    explicit ConfigService(ConfigPaths paths);
    bool reload(); // Explicit reload; failure retains the last valid settings.
    // Single-owner service: perform saves on its I/O thread and marshal results
    // to UI owners. Never call concurrently on the same service or during Render.
    AtomicWriteState saveOverrides(std::span<const OverrideEdit> edits);
    const Settings& settings() const { return m_settings; }
    const ConfigIssues& issues() const { return m_issues; }

  private:
    ConfigPaths m_paths;
    Settings m_settings;
    ConfigIssues m_issues;
  };
} // namespace luciole
