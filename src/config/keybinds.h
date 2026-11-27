#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace luciole {
  // XKB keysyms with the same modifier bits as KeyMod.
  struct KeybindChord {
    std::uint32_t sym = 0;
    std::uint32_t modifiers = 0;

    bool operator==(const KeybindChord&) const = default;
  };

  namespace KeybindMod {
    inline constexpr std::uint32_t Shift = 1U << 0;
    inline constexpr std::uint32_t Ctrl = 1U << 1;
    inline constexpr std::uint32_t Alt = 1U << 2;
    inline constexpr std::uint32_t Super = 1U << 3;
  } // namespace KeybindMod

  // Conflicts are checked within a scope only.
  enum class KeybindScope : std::uint8_t { Controls, FileBrowser, TabsAndWindows };

  // Commands in settings order.
  enum class KeybindCommand : std::uint8_t {
    Validate = 0,
    Cancel,
    FocusNext,
    FocusPrevious,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveFirst,
    MoveLast,
    MovePageUp,
    MovePageDown,

    Back,
    Forward,
    Up,
    HomeFolder,
    Refresh,
    EditLocation,
    Search,
    ViewGrid,
    ViewList,
    ViewMiller,
    ToggleHidden,
    ToggleSidebar,
    TogglePreview,
    QuickPreview,
    Open,
    OpenInNewTab,
    OpenInNewTabBackground,
    OpenInOtherPane,
    OpenInNewWindow,
    AdministratorAccess,
    ToggleSelection,
    Rename,
    CreateFolder,
    CreateFile,
    Cut,
    Copy,
    CopyTargetPath,
    CopyTargetUri,
    Paste,
    Trash,
    DeletePermanently,
    Properties,
    Terminal,
    ParentFolder,
    ContextMenu,
    SelectAll,
    InvertSelection,
    SelectPattern,
    ClearSelection,
    Undo,
    Redo,
    Settings,
    KeyboardShortcuts,
    ZoomIn,
    ZoomOut,
    ZoomReset,

    NewTab,
    CloseTab,
    DuplicateTab,
    ReopenTab,
    NextTab,
    PreviousTab,
    SplitView,
    SwitchArea,
    ClosePane,
    NewWindow,
    CloseWindow,
    Fullscreen,
    Quit,

    SelectTab1,
    SelectTab2,
    SelectTab3,
    SelectTab4,
    SelectTab5,
    SelectTab6,
    SelectTab7,
    SelectTab8,
    SelectTab9,
  };
  inline constexpr std::size_t kKeybindCommandCount = 80;
  inline constexpr std::size_t kMaxKeybindChords = 4;
  inline constexpr std::array<KeybindScope, 3> kKeybindScopes{
      KeybindScope::Controls, KeybindScope::FileBrowser, KeybindScope::TabsAndWindows
  };

  // A command's chords, packed from index 0 with no gaps. An empty slot ends the list.
  using KeybindChords = std::array<std::optional<KeybindChord>, kMaxKeybindChords>;

  class KeybindConfig {
  public:
    KeybindConfig();
    const KeybindChords& chords(KeybindCommand command) const;
    // Stored chords retain defaults that conflict with a configured command.
    const KeybindChords& storedChords(KeybindCommand command) const;
    bool hasExplicitFallback(KeybindCommand command) const;
    void setChords(KeybindCommand command, KeybindChords chords);
    void resolveFallbackDefaults(const KeybindConfig* inherited = nullptr);
    bool operator==(const KeybindConfig&) const = default;

  private:
    std::array<KeybindChords, kKeybindCommandCount> m_chords{};
    std::bitset<kKeybindCommandCount> m_explicitFallbacks;
    std::bitset<kKeybindCommandCount> m_suppressedDefaults;
  };

  std::span<const KeybindCommand> keybindCommands(KeybindScope scope);
  std::string_view keybindCommandKey(KeybindCommand command);
  std::string_view keybindCommandLabel(KeybindCommand command);
  KeybindScope keybindCommandScope(KeybindCommand command);
  std::optional<KeybindCommand> keybindCommandFromKey(std::string_view key);

  // Parses a display/TOML chord; rejects malformed chords and Super.
  std::optional<KeybindChord> parseKeybindChord(std::string_view spec);
  // Lower-cases printable keysyms, folds ISO_Left_Tab into Shift+Tab and keypad Enter into Return.
  KeybindChord normalizeKeybindChord(KeybindChord chord);
  std::string keybindChordSpec(const KeybindChord& chord);  // Compact storage spelling
  std::string keybindChordLabel(const KeybindChord& chord); // Compact display spelling

  // Friendlier display name for an XKB key; empty when the XKB name already reads well.
  inline std::string_view keybindKeyNameLabel(std::string_view name) {
    if (name == "Return") {
      return "Enter";
    }
    if (name == "KP_Enter") {
      return "Numpad Enter";
    }
    if (name == "space") {
      return "Space";
    }
    if (name == "Prior") {
      return "Page Up";
    }
    if (name == "Next") {
      return "Page Down";
    }
    if (name == "Tab" || name == "ISO_Left_Tab") {
      return "Tab";
    }
    if (name == "BackSpace") {
      return "Backspace";
    }
    return {};
  }
  bool keybindChordMatches(const KeybindChord& chord, std::uint32_t sym, std::uint32_t modifiers) noexcept;

  // The configuration the application currently uses. Installed at startup and whenever settings change.
  const KeybindConfig& activeKeybinds();
  void setActiveKeybinds(const KeybindConfig& config);
  bool keybindMatches(const KeybindConfig& config, KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers);
  bool keybindMatches(KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers);

  struct KeybindMatch {
    KeybindCommand command;
    KeybindChord chord;
    std::uint32_t extraModifiers;
  };
  inline constexpr std::size_t kMovementCommandCount = 8;
  std::span<const KeybindCommand> keybindMovementCommands();
  // Chooses the most specific matching movement chord, so a configured Shift+Down is not shadowed by Down + Shift.
  std::optional<KeybindMatch>
  keybindBestMatch(std::span<const KeybindCommand> commands, std::uint32_t sym, std::uint32_t modifiers);

  std::string keybindShortcutLabel(const KeybindChords& chords);
  std::string keybindShortcutLabel(const KeybindConfig& config, KeybindCommand command);
  std::string keybindShortcutLabel(KeybindCommand command);
  inline std::optional<std::string> keybindShortcutOptionalLabel(KeybindCommand command) {
    std::string label = keybindShortcutLabel(command);
    return label.empty() ? std::nullopt : std::optional(std::move(label));
  }

  // The command in the same scope that already uses `chord`, ignoring `command`'s own `slot`.
  std::optional<KeybindCommand>
  keybindConflict(const KeybindConfig& config, KeybindCommand command, std::size_t slot, const KeybindChord& chord);
  bool keybindConfigValid(const KeybindConfig& config);

  // Writes one slot and keeps the chord list packed. A nullopt or zero-sym chord removes the slot.
  void
  setKeybindChord(KeybindConfig& config, KeybindCommand command, std::size_t slot, std::optional<KeybindChord> chord);
} // namespace luciole
