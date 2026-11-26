#pragma once

#include "config/diagnostic.h"
#include "config/keybinds.h"
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
    DropAction dropAction = DropAction::Menu;
    bool singleClickLocationEdit = false;
    bool confirmPermanentDeletion = true;
    bool showPermanentDeletion = false;
    bool switchToNewTab = true;
    bool restoreLastWindow = false;
    bool singleWindow = false;
    ViewMode viewMode = ViewMode::List;
    bool rememberFolderViews = false; // Sort and view mode changes apply to the current folder only.
    bool showHidden = false;
    bool showBackups = true;
    bool alwaysShowTabBar = true;
    bool alwaysShowStatusBar = false;
    bool compactMenus = true;
    bool contextMenuIcons = true;
    bool showCloseButtons = true;
    bool reduceAnimations = false;
    bool useSSD = true;
    bool showFolderInTitle = true; // Browser windows title themselves after the shown folder.
    ColorScheme colorScheme = ColorScheme::Auto;
    double backgroundOpacity = 1.0; // 0 (clear) to 1 (opaque) for the window background.
    double cornerRadiusScale = 1.0; // 0 (square) to 2; 1 is the default rounding.
    BrowsePresentation presentation;
    int gridIconSize = kDefaultGridIconSize;
    int listIconSize = 0;   // 0 keeps kDefaultListIconSize.
    int millerIconSize = 0; // 0 keeps the density's Columns icon size.
    int sidebarWidth = kDefaultSidebarWidth;
    bool showSidebar = true;
    bool sidebarTree = false;
    bool showDrives = true;
    bool showNetwork = true;
    bool expandableFolders = false;
    bool showPreview = true;
    // File picker windows keep their own preview visibility and layout.
    bool chooserShowPreview = false;
    ChooserStartFolder chooserStartFolder = ChooserStartFolder::Home;
    SortOptions chooserSort;
    ViewMode chooserViewMode = ViewMode::List;
    bool chooserShowHidden = false;
    int chooserGridIconSize = kDefaultGridIconSize;
    int chooserListIconSize = 0;
    int previewWidth = 320;
    int millerColumnWidth = 260;
    bool millerShowParents = true;
    DetailsColumnVisibility columns{};
    DetailsColumnOrder columnOrder = kDefaultDetailsColumnOrder;
    DetailsColumnWidths columnWidths = kDefaultDetailsColumnWidths;
    ThumbnailOptions thumbnails;
    std::string terminalCommand;
    KeybindConfig keybinds;
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
    DropAction,
    SingleClickLocationEdit,
    ConfirmPermanentDeletion,
    ShowPermanentDeletion,
    SwitchToNewTab,
    RestoreLastWindow,
    SingleWindow,
    ShowHidden,
    ShowBackups,
    AlwaysShowTabBar,
    AlwaysShowStatusBar,
    CompactMenus,
    ContextMenuIcons,
    ShowCloseButtons,
    ReduceAnimations,
    UseSSD,
    ShowFolderInTitle,
    ColorScheme,
    BackgroundOpacity,
    CornerRadiusScale,
    SortBy,
    SortDescending,
    DirectoriesFirst,
    HiddenLast,
    ViewMode,
    RememberFolderViews,
    GridIconSize,
    ListIconSize,
    MillerIconSize,
    Density,
    FilenameTruncation,
    ShowSizeCaption,
    ShowTypeCaption,
    ShowModifiedCaption,
    ShowEmblems,
    SidebarWidth,
    ShowSidebar,
    SidebarTree,
    ShowDrives,
    ShowNetwork,
    ExpandableFolders,
    ShowPreview,
    ChooserShowPreview,
    ChooserStartFolder,
    ChooserSortBy,
    ChooserSortDescending,
    ChooserDirectoriesFirst,
    ChooserHiddenLast,
    ChooserViewMode,
    ChooserShowHidden,
    ChooserGridIconSize,
    ChooserListIconSize,
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
    ThumbnailCacheMib,
    TerminalCommand,
    Keybinds,
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
    static constexpr PreferenceKey<luciole::DropAction> DropAction{Id::DropAction};
    static constexpr PreferenceKey<bool> SingleClickLocationEdit{Id::SingleClickLocationEdit};
    static constexpr PreferenceKey<bool> ConfirmPermanentDeletion{Id::ConfirmPermanentDeletion};
    static constexpr PreferenceKey<bool> ShowPermanentDeletion{Id::ShowPermanentDeletion};
    static constexpr PreferenceKey<bool> SwitchToNewTab{Id::SwitchToNewTab};
    static constexpr PreferenceKey<bool> RestoreLastWindow{Id::RestoreLastWindow};
    static constexpr PreferenceKey<bool> SingleWindow{Id::SingleWindow};
    static constexpr PreferenceKey<bool> ShowHidden{Id::ShowHidden};
    static constexpr PreferenceKey<bool> ShowBackups{Id::ShowBackups};
    static constexpr PreferenceKey<bool> AlwaysShowTabBar{Id::AlwaysShowTabBar};
    static constexpr PreferenceKey<bool> AlwaysShowStatusBar{Id::AlwaysShowStatusBar};
    static constexpr PreferenceKey<bool> CompactMenus{Id::CompactMenus};
    static constexpr PreferenceKey<bool> ContextMenuIcons{Id::ContextMenuIcons};
    static constexpr PreferenceKey<bool> ShowCloseButtons{Id::ShowCloseButtons};
    static constexpr PreferenceKey<bool> ReduceAnimations{Id::ReduceAnimations};
    static constexpr PreferenceKey<bool> UseSSD{Id::UseSSD};
    static constexpr PreferenceKey<bool> ShowFolderInTitle{Id::ShowFolderInTitle};
    static constexpr PreferenceKey<luciole::ColorScheme> ColorScheme{Id::ColorScheme};
    static constexpr PreferenceKey<double> BackgroundOpacity{Id::BackgroundOpacity};
    static constexpr PreferenceKey<double> CornerRadiusScale{Id::CornerRadiusScale};
    static constexpr PreferenceKey<luciole::SortField> SortBy{Id::SortBy};
    static constexpr PreferenceKey<bool> SortDescending{Id::SortDescending};
    static constexpr PreferenceKey<bool> DirectoriesFirst{Id::DirectoriesFirst};
    static constexpr PreferenceKey<bool> HiddenLast{Id::HiddenLast};
    static constexpr PreferenceKey<luciole::ViewMode> ViewMode{Id::ViewMode};
    static constexpr PreferenceKey<bool> RememberFolderViews{Id::RememberFolderViews};
    static constexpr PreferenceKey<int> GridIconSize{Id::GridIconSize};
    static constexpr PreferenceKey<int> ListIconSize{Id::ListIconSize};
    static constexpr PreferenceKey<int> MillerIconSize{Id::MillerIconSize};
    static constexpr PreferenceKey<luciole::ViewDensity> Density{Id::Density};
    static constexpr PreferenceKey<luciole::FilenameTruncation> FilenameTruncation{Id::FilenameTruncation};
    static constexpr PreferenceKey<bool> ShowSizeCaption{Id::ShowSizeCaption};
    static constexpr PreferenceKey<bool> ShowTypeCaption{Id::ShowTypeCaption};
    static constexpr PreferenceKey<bool> ShowModifiedCaption{Id::ShowModifiedCaption};
    static constexpr PreferenceKey<bool> ShowEmblems{Id::ShowEmblems};
    static constexpr PreferenceKey<int> SidebarWidth{Id::SidebarWidth};
    static constexpr PreferenceKey<bool> ShowSidebar{Id::ShowSidebar};
    static constexpr PreferenceKey<bool> SidebarTree{Id::SidebarTree};
    static constexpr PreferenceKey<bool> ShowDrives{Id::ShowDrives};
    static constexpr PreferenceKey<bool> ShowNetwork{Id::ShowNetwork};
    static constexpr PreferenceKey<bool> ExpandableFolders{Id::ExpandableFolders};
    static constexpr PreferenceKey<bool> ShowPreview{Id::ShowPreview};
    static constexpr PreferenceKey<bool> ChooserShowPreview{Id::ChooserShowPreview};
    static constexpr PreferenceKey<luciole::ChooserStartFolder> ChooserStartFolder{Id::ChooserStartFolder};
    static constexpr PreferenceKey<luciole::SortField> ChooserSortBy{Id::ChooserSortBy};
    static constexpr PreferenceKey<bool> ChooserSortDescending{Id::ChooserSortDescending};
    static constexpr PreferenceKey<bool> ChooserDirectoriesFirst{Id::ChooserDirectoriesFirst};
    static constexpr PreferenceKey<bool> ChooserHiddenLast{Id::ChooserHiddenLast};
    static constexpr PreferenceKey<luciole::ViewMode> ChooserViewMode{Id::ChooserViewMode};
    static constexpr PreferenceKey<bool> ChooserShowHidden{Id::ChooserShowHidden};
    static constexpr PreferenceKey<int> ChooserGridIconSize{Id::ChooserGridIconSize};
    static constexpr PreferenceKey<int> ChooserListIconSize{Id::ChooserListIconSize};
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
    static constexpr PreferenceKey<int> ThumbnailCacheMib{Id::ThumbnailCacheMib};
    static constexpr PreferenceKey<luciole::KeybindConfig> Keybinds{Id::Keybinds};
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
        std::monostate, bool, int, double, SortField, ViewMode, ViewDensity, FilenameTruncation, DetailsColumnOrder,
        ActivationMode, TypingMode, DropAction, ThumbnailPolicy, ChooserStartFolder, ColorScheme, std::string,
        KeybindConfig>
        m_value;
  };

  // The TOML spelling of sort fields and view modes, shared by settings.toml and the folder view state.
  std::string_view sortFieldName(SortField field);
  std::optional<SortField> sortFieldFromName(std::string_view name);
  std::string_view viewModeName(ViewMode mode);
  std::optional<ViewMode> viewModeFromName(std::string_view name);

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
