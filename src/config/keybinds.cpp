#include "config/keybinds.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace {
  using luciole::KeybindChord;
  using luciole::KeybindCommand;
  using luciole::KeybindScope;
  namespace KeybindMod = luciole::KeybindMod;

  std::string asciiLower(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
      if (character >= 'A' && character <= 'Z') {
        character = static_cast<char>(character - 'A' + 'a');
      }
    }
    return result;
  }

  std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
      value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
      value.remove_suffix(1);
    }
    return value;
  }

  std::string keysymName(std::uint32_t sym) {
    if (sym == 0) {
      return {};
    }
    std::array<char, 64> buffer{};
    const int length = xkb_keysym_get_name(static_cast<xkb_keysym_t>(sym), buffer.data(), buffer.size());
    if (length <= 0) {
      return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
  }

  bool printableConfigKey(std::uint32_t sym) {
    const auto keysym = static_cast<xkb_keysym_t>(sym);
    if ((keysym >= XKB_KEY_KP_Space && keysym <= XKB_KEY_KP_Delete)
        || (keysym >= XKB_KEY_KP_0 && keysym <= XKB_KEY_KP_Equal)) {
      return false;
    }
    const auto codepoint = xkb_keysym_to_utf32(keysym);
    return codepoint >= 0x20U && codepoint != 0x7FU;
  }

  // Raw chord parser; compact shorthands are handled by parseKeybindChord.
  std::optional<KeybindChord> parseChordSpec(std::string_view rawSpec) {
    const std::string spec(trim(rawSpec));
    if (spec.empty()) {
      return std::nullopt;
    }
    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (start <= spec.size()) {
      const std::size_t plus = spec.find('+', start);
      const std::size_t length = plus == std::string::npos ? spec.size() - start : plus - start;
      const std::string token(trim(std::string_view(spec).substr(start, length)));
      if (token.empty()) {
        return std::nullopt;
      }
      tokens.push_back(token);
      if (plus == std::string::npos) {
        break;
      }
      start = plus + 1;
    }

    std::uint32_t modifiers = 0;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
      const std::string mod = asciiLower(tokens[index]);
      if (mod == "ctrl") {
        modifiers |= KeybindMod::Ctrl;
      } else if (mod == "shift") {
        modifiers |= KeybindMod::Shift;
      } else if (mod == "alt") {
        modifiers |= KeybindMod::Alt;
      } else {
        // Super belongs to the compositor; anything else is not a modifier.
        return std::nullopt;
      }
    }

    const xkb_keysym_t sym = xkb_keysym_from_name(tokens.back().c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
      return std::nullopt;
    }
    return KeybindChord{static_cast<std::uint32_t>(sym), modifiers};
  }

  std::string chordLabel(const KeybindChord& chord) {
    if (chord.sym == 0) {
      return {};
    }
    std::string keyName;
    const std::uint32_t codepoint = xkb_keysym_to_utf32(static_cast<xkb_keysym_t>(chord.sym));
    if (codepoint > 0x20 && codepoint != 0x7F) {
      std::array<char, 8> utf8{};
      const int length = xkb_keysym_to_utf8(static_cast<xkb_keysym_t>(chord.sym), utf8.data(), utf8.size());
      if (length > 1) {
        keyName.assign(utf8.data(), static_cast<std::size_t>(length - 1));
        if (keyName.size() == 1 && keyName[0] >= 'a' && keyName[0] <= 'z') {
          keyName[0] = static_cast<char>(keyName[0] - 'a' + 'A');
        }
      }
    }
    if (keyName.empty()) {
      keyName = keysymName(chord.sym);
    }
    if (const std::string_view label = luciole::keybindKeyNameLabel(keyName); !label.empty()) {
      keyName = label;
    }
    if (keyName.empty()) {
      return {};
    }
    std::string out;
    const auto append = [&](std::string_view part) {
      if (!out.empty()) {
        out += '+';
      }
      out += part;
    };
    if ((chord.modifiers & KeybindMod::Ctrl) != 0) {
      append("Ctrl");
    }
    if ((chord.modifiers & KeybindMod::Alt) != 0) {
      append("Alt");
    }
    if ((chord.modifiers & KeybindMod::Shift) != 0) {
      append("Shift");
    }
    append(keyName);
    return out;
  }

  struct KeybindSpec {
    KeybindCommand command;
    KeybindScope scope;
    std::string_view key;
    std::string_view label;
    std::array<std::string_view, luciole::kMaxKeybindChords> defaults;
    // Unless configured explicitly, the default stays unassigned while another command in its scope uses the chord.
    bool fallback = false;
  };

  // Keybind commands in enum order; defaults use display spelling.
  constexpr auto kKeybindSpecs = std::to_array<KeybindSpec>({
      {KeybindCommand::Validate, KeybindScope::Controls, "validate", "Validate", {"Return", "space"}},
      {KeybindCommand::Cancel, KeybindScope::Controls, "cancel", "Cancel", {"Escape"}},
      {KeybindCommand::FocusNext, KeybindScope::Controls, "focus_next", "Focus Next", {"Tab"}},
      {KeybindCommand::FocusPrevious, KeybindScope::Controls, "focus_previous", "Focus Previous", {"Shift+Tab"}},
      {KeybindCommand::MoveLeft, KeybindScope::Controls, "move_left", "Move Left", {"Left"}},
      {KeybindCommand::MoveRight, KeybindScope::Controls, "move_right", "Move Right", {"Right"}},
      {KeybindCommand::MoveUp, KeybindScope::Controls, "move_up", "Move Up", {"Up"}},
      {KeybindCommand::MoveDown, KeybindScope::Controls, "move_down", "Move Down", {"Down"}},
      {KeybindCommand::MoveFirst, KeybindScope::Controls, "move_first", "Move to First", {"Home"}},
      {KeybindCommand::MoveLast, KeybindScope::Controls, "move_last", "Move to Last", {"End"}},
      {KeybindCommand::MovePageUp, KeybindScope::Controls, "move_page_up", "Move Up One Page", {"Page_Up"}},
      {KeybindCommand::MovePageDown, KeybindScope::Controls, "move_page_down", "Move Down One Page", {"Page_Down"}},

      {KeybindCommand::Back, KeybindScope::FileBrowser, "back", "Back", {"Alt+Left"}},
      {KeybindCommand::Forward, KeybindScope::FileBrowser, "forward", "Forward", {"Alt+Right"}},
      {KeybindCommand::Up, KeybindScope::FileBrowser, "up", "Up", {"Alt+Up"}},
      {KeybindCommand::HomeFolder, KeybindScope::FileBrowser, "home_folder", "Home Folder", {"Alt+Home"}},
      {KeybindCommand::Refresh, KeybindScope::FileBrowser, "refresh", "Refresh", {"F5"}},
      {KeybindCommand::EditLocation, KeybindScope::FileBrowser, "edit_location", "Edit Location", {"Ctrl+L"}},
      {KeybindCommand::Search, KeybindScope::FileBrowser, "search", "Search", {"Ctrl+F", "Ctrl+K"}},
      {KeybindCommand::ViewGrid, KeybindScope::FileBrowser, "view_grid", "Grid View", {"Ctrl+1"}},
      {KeybindCommand::ViewList, KeybindScope::FileBrowser, "view_list", "List View", {"Ctrl+2"}},
      {KeybindCommand::ViewMiller, KeybindScope::FileBrowser, "view_miller", "Column View", {"Ctrl+3"}},
      {KeybindCommand::ToggleHidden, KeybindScope::FileBrowser, "toggle_hidden", "Show Hidden", {"Ctrl+H"}},
      {KeybindCommand::ToggleSidebar, KeybindScope::FileBrowser, "toggle_sidebar", "Toggle Sidebar", {"F9"}},
      {KeybindCommand::TogglePreview, KeybindScope::FileBrowser, "toggle_preview", "Toggle Preview", {"F8"}},
      {KeybindCommand::QuickPreview, KeybindScope::FileBrowser, "quick_preview", "Quick Preview", {"space"}},
      {KeybindCommand::Open, KeybindScope::FileBrowser, "open", "Open", {"Return"}},
      {KeybindCommand::OpenInNewTab, KeybindScope::FileBrowser, "open_in_new_tab", "Open in New Tab", {"Ctrl+Return"}},
      {KeybindCommand::OpenInNewTabBackground,
       KeybindScope::FileBrowser,
       "open_in_new_tab_background",
       "Open in New Tab in Background",
       {"Ctrl+Shift+Return"}},
      {KeybindCommand::OpenInOtherPane,
       KeybindScope::FileBrowser,
       "open_in_other_pane",
       "Open in Other Pane",
       {"Ctrl+Alt+Return"},
       true},
      {KeybindCommand::OpenInNewWindow,
       KeybindScope::FileBrowser,
       "open_in_new_window",
       "Open in New Window",
       {"Shift+Return"},
       true},
      {KeybindCommand::AdministratorAccess,
       KeybindScope::FileBrowser,
       "administrator_access",
       "Administrator Access",
       {"Alt+Shift+Return"},
       true},
      {KeybindCommand::ToggleSelection,
       KeybindScope::FileBrowser,
       "toggle_selection",
       "Toggle Selection",
       {"Ctrl+space"}},
      {KeybindCommand::Rename, KeybindScope::FileBrowser, "rename", "Rename", {"F2"}},
      {KeybindCommand::CreateFolder, KeybindScope::FileBrowser, "create_folder", "Create Folder", {"Ctrl+Shift+N"}},
      {KeybindCommand::CreateFile, KeybindScope::FileBrowser, "create_file", "Create File", {"Ctrl+Alt+N"}},
      {KeybindCommand::Cut, KeybindScope::FileBrowser, "cut", "Cut", {"Ctrl+X"}},
      {KeybindCommand::Copy, KeybindScope::FileBrowser, "copy", "Copy", {"Ctrl+C"}},
      {KeybindCommand::CopyTargetPath,
       KeybindScope::FileBrowser,
       "copy_target_path",
       "Copy Path",
       {"Ctrl+Alt+C"},
       true},
      {KeybindCommand::CopyTargetUri, KeybindScope::FileBrowser, "copy_target_uri", "Copy URI", {"Ctrl+Alt+U"}, true},
      {KeybindCommand::Paste, KeybindScope::FileBrowser, "paste", "Paste", {"Ctrl+V"}},
      {KeybindCommand::Trash, KeybindScope::FileBrowser, "trash", "Move to Trash", {"Delete"}},
      {KeybindCommand::DeletePermanently,
       KeybindScope::FileBrowser,
       "delete_permanently",
       "Delete Permanently",
       {"Shift+Delete"}},
      {KeybindCommand::Properties, KeybindScope::FileBrowser, "properties", "Properties", {"Alt+Return"}},
      {KeybindCommand::Terminal, KeybindScope::FileBrowser, "terminal", "Open Terminal", {"F4"}},
      {KeybindCommand::ParentFolder, KeybindScope::FileBrowser, "parent_folder", "Parent Folder", {"BackSpace"}},
      {KeybindCommand::ContextMenu, KeybindScope::FileBrowser, "context_menu", "Context Menu", {"Menu", "Shift+F10"}},
      {KeybindCommand::SelectAll, KeybindScope::FileBrowser, "select_all", "Select All", {"Ctrl+A"}},
      {KeybindCommand::InvertSelection,
       KeybindScope::FileBrowser,
       "invert_selection",
       "Invert Selection",
       {"Ctrl+Shift+I"}},
      {KeybindCommand::SelectPattern, KeybindScope::FileBrowser, "select_pattern", "Select Pattern", {"Ctrl+S"}},
      {KeybindCommand::ClearSelection, KeybindScope::FileBrowser, "clear_selection", "Clear Selection", {"Escape"}},
      {KeybindCommand::Undo, KeybindScope::FileBrowser, "undo", "Undo", {"Ctrl+Z"}},
      {KeybindCommand::Redo, KeybindScope::FileBrowser, "redo", "Redo", {"Ctrl+Shift+Z"}},
      {KeybindCommand::Settings, KeybindScope::FileBrowser, "settings", "Settings", {"Ctrl+,"}},
      {KeybindCommand::KeyboardShortcuts,
       KeybindScope::FileBrowser,
       "keyboard_shortcuts",
       "Keyboard Shortcuts",
       {"F1"}},
      {KeybindCommand::ZoomIn,
       KeybindScope::FileBrowser,
       "zoom_in",
       "Zoom In",
       {"Ctrl+equal", "Ctrl+plus", "Ctrl+Shift+plus", "Ctrl+KP_Add"}},
      {KeybindCommand::ZoomOut, KeybindScope::FileBrowser, "zoom_out", "Zoom Out", {"Ctrl+minus", "Ctrl+KP_Subtract"}},
      {KeybindCommand::ZoomReset, KeybindScope::FileBrowser, "zoom_reset", "Reset Zoom", {"Ctrl+0", "Ctrl+KP_0"}},

      {KeybindCommand::NewTab, KeybindScope::TabsAndWindows, "new_tab", "New Tab", {"Ctrl+T"}},
      {KeybindCommand::CloseTab, KeybindScope::TabsAndWindows, "close_tab", "Close Tab", {"Ctrl+W"}},
      {KeybindCommand::DuplicateTab, KeybindScope::TabsAndWindows, "duplicate_tab", "Duplicate Tab", {"Ctrl+Shift+D"}},
      {KeybindCommand::ReopenTab, KeybindScope::TabsAndWindows, "reopen_tab", "Reopen Closed Tab", {"Ctrl+Shift+T"}},
      {KeybindCommand::NextTab,
       KeybindScope::TabsAndWindows,
       "next_tab",
       "Next Tab",
       {"Ctrl+Tab", "Ctrl+Page_Down", "Ctrl+Shift+Page_Down"}},
      {KeybindCommand::PreviousTab,
       KeybindScope::TabsAndWindows,
       "previous_tab",
       "Previous Tab",
       {"Ctrl+Shift+Tab", "Ctrl+Page_Up", "Ctrl+Shift+Page_Up"}},
      {KeybindCommand::SplitView, KeybindScope::TabsAndWindows, "split_view", "Split View", {"F3"}},
      {KeybindCommand::SwitchArea, KeybindScope::TabsAndWindows, "switch_area", "Switch Area", {"F6"}},
      {KeybindCommand::ClosePane, KeybindScope::TabsAndWindows, "close_pane", "Close Pane", {"Ctrl+Alt+W"}},
      {KeybindCommand::NewWindow, KeybindScope::TabsAndWindows, "new_window", "New Window", {"Ctrl+N"}},
      {KeybindCommand::CloseWindow, KeybindScope::TabsAndWindows, "close_window", "Close Window", {"Ctrl+Shift+W"}},
      {KeybindCommand::Fullscreen, KeybindScope::TabsAndWindows, "fullscreen", "Fullscreen", {"F11"}},
      {KeybindCommand::Quit, KeybindScope::TabsAndWindows, "quit", "Quit", {"Ctrl+Q"}},
      {KeybindCommand::SelectTab1, KeybindScope::TabsAndWindows, "select_tab_1", "Select Tab 1", {"Alt+1"}},
      {KeybindCommand::SelectTab2, KeybindScope::TabsAndWindows, "select_tab_2", "Select Tab 2", {"Alt+2"}},
      {KeybindCommand::SelectTab3, KeybindScope::TabsAndWindows, "select_tab_3", "Select Tab 3", {"Alt+3"}},
      {KeybindCommand::SelectTab4, KeybindScope::TabsAndWindows, "select_tab_4", "Select Tab 4", {"Alt+4"}},
      {KeybindCommand::SelectTab5, KeybindScope::TabsAndWindows, "select_tab_5", "Select Tab 5", {"Alt+5"}},
      {KeybindCommand::SelectTab6, KeybindScope::TabsAndWindows, "select_tab_6", "Select Tab 6", {"Alt+6"}},
      {KeybindCommand::SelectTab7, KeybindScope::TabsAndWindows, "select_tab_7", "Select Tab 7", {"Alt+7"}},
      {KeybindCommand::SelectTab8, KeybindScope::TabsAndWindows, "select_tab_8", "Select Tab 8", {"Alt+8"}},
      {KeybindCommand::SelectTab9, KeybindScope::TabsAndWindows, "select_tab_9", "Select Last Tab", {"Alt+9"}},
  });
  static_assert(kKeybindSpecs.size() == luciole::kKeybindCommandCount);
  static_assert(std::ranges::all_of(kKeybindSpecs, [](const KeybindSpec& spec) {
    return !spec.fallback || (!spec.defaults[0].empty() && spec.defaults[1].empty());
  }));
  static_assert(
      []<std::size_t... I>(std::index_sequence<I...>) {
        return ((kKeybindSpecs[I].command == static_cast<KeybindCommand>(I)) && ...);
      }(std::make_index_sequence<kKeybindSpecs.size()>{}),
      "kKeybindSpecs must follow KeybindCommand order"
  );

  constexpr std::size_t kControlKeybindCount = 12;
  constexpr std::size_t kFileBrowserKeybindCount = 46;
  constexpr std::size_t kTabsAndWindowsKeybindCount = 22;
  static_assert(kControlKeybindCount + kFileBrowserKeybindCount + kTabsAndWindowsKeybindCount == kKeybindSpecs.size());
} // namespace

namespace luciole {
  std::span<const KeybindCommand> keybindCommands(KeybindScope scope) {
    static const auto commands = [] {
      std::array<KeybindCommand, kKeybindCommandCount> result{};
      for (std::size_t index = 0; index < kKeybindSpecs.size(); ++index) {
        result[index] = kKeybindSpecs[index].command;
      }
      return result;
    }();
    std::size_t offset = 0;
    std::size_t count = kControlKeybindCount;
    if (scope == KeybindScope::FileBrowser) {
      offset = kControlKeybindCount;
      count = kFileBrowserKeybindCount;
    } else if (scope == KeybindScope::TabsAndWindows) {
      offset = kControlKeybindCount + kFileBrowserKeybindCount;
      count = kTabsAndWindowsKeybindCount;
    }
    return std::span<const KeybindCommand>(commands).subspan(offset, count);
  }

  std::string_view keybindCommandKey(KeybindCommand command) {
    return kKeybindSpecs[static_cast<std::size_t>(command)].key;
  }

  std::string_view keybindCommandLabel(KeybindCommand command) {
    return kKeybindSpecs[static_cast<std::size_t>(command)].label;
  }

  KeybindScope keybindCommandScope(KeybindCommand command) {
    return kKeybindSpecs[static_cast<std::size_t>(command)].scope;
  }

  std::optional<KeybindCommand> keybindCommandFromKey(std::string_view key) {
    for (const auto& spec : kKeybindSpecs) {
      if (spec.key == key) {
        return spec.command;
      }
    }
    return std::nullopt;
  }

  KeybindChord normalizeKeybindChord(KeybindChord chord) {
    if (chord.sym == XKB_KEY_ISO_Left_Tab) {
      chord.sym = XKB_KEY_Tab;
      chord.modifiers |= KeybindMod::Shift;
    } else if (chord.sym == XKB_KEY_KP_Enter) {
      chord.sym = XKB_KEY_Return;
    } else {
      chord.sym = static_cast<std::uint32_t>(xkb_keysym_to_lower(static_cast<xkb_keysym_t>(chord.sym)));
    }
    return chord;
  }

  std::optional<KeybindChord> parseKeybindChord(std::string_view rawSpec) {
    std::string spec(trim(rawSpec));
    if (spec.empty()) {
      return std::nullopt;
    }
    if (spec.ends_with("+,")) {
      spec.replace(spec.size() - 1, 1, "comma");
    } else if (spec.ends_with("++")) {
      spec.pop_back();
      spec += "plus";
    }
    const auto chord = parseChordSpec(spec);
    return chord ? std::optional<KeybindChord>(normalizeKeybindChord(*chord)) : std::nullopt;
  }

  std::string keybindChordSpec(const KeybindChord& chord) {
    std::string spec;
    if ((chord.modifiers & KeybindMod::Ctrl) != 0) {
      spec += "Ctrl+";
    }
    if ((chord.modifiers & KeybindMod::Alt) != 0) {
      spec += "Alt+";
    }
    if ((chord.modifiers & KeybindMod::Shift) != 0) {
      spec += "Shift+";
    }
    spec += keysymName(chord.sym);
    return spec;
  }

  std::string keybindChordLabel(const KeybindChord& chord) { return chordLabel(chord); }

  bool keybindChordMatches(const KeybindChord& chord, std::uint32_t sym, std::uint32_t modifiers) noexcept {
    if (sym == XKB_KEY_KP_Enter) {
      sym = XKB_KEY_Return;
    }
    if (sym == XKB_KEY_ISO_Left_Tab) {
      sym = XKB_KEY_Tab;
      modifiers |= KeybindMod::Shift;
    } else if ((modifiers & KeybindMod::Shift) != 0) {
      sym = static_cast<std::uint32_t>(xkb_keysym_to_lower(static_cast<xkb_keysym_t>(sym)));
    }
    return chord.sym == sym && chord.modifiers == modifiers;
  }

  KeybindConfig::KeybindConfig() {
    for (std::size_t command = 0; command < kKeybindSpecs.size(); ++command) {
      for (std::size_t slot = 0; slot < kMaxKeybindChords; ++slot) {
        if (kKeybindSpecs[command].defaults[slot].empty()) {
          break;
        }
        if (const auto chord = parseKeybindChord(kKeybindSpecs[command].defaults[slot])) {
          m_chords[command][slot] = *chord;
        }
      }
    }
  }

  const KeybindChords& KeybindConfig::chords(KeybindCommand command) const {
    static constexpr KeybindChords unassigned{};
    return m_suppressedDefaults.test(static_cast<std::size_t>(command)) ? unassigned : storedChords(command);
  }

  const KeybindChords& KeybindConfig::storedChords(KeybindCommand command) const {
    return m_chords[static_cast<std::size_t>(command)];
  }

  bool KeybindConfig::hasExplicitFallback(KeybindCommand command) const {
    return m_explicitFallbacks.test(static_cast<std::size_t>(command));
  }

  void KeybindConfig::setChords(KeybindCommand command, KeybindChords chords) {
    const auto index = static_cast<std::size_t>(command);
    m_chords[index] = chords;
    m_explicitFallbacks.set(index, kKeybindSpecs[index].fallback);
    m_suppressedDefaults.reset(index);
  }

  void KeybindConfig::resolveFallbackDefaults(const KeybindConfig* inherited) {
    if (inherited != nullptr) {
      m_explicitFallbacks |= inherited->m_explicitFallbacks;
    }
    m_suppressedDefaults.reset();
    for (const auto& spec : kKeybindSpecs) {
      if (!spec.fallback || hasExplicitFallback(spec.command)) {
        continue;
      }
      const auto& chord = storedChords(spec.command).front();
      if (chord && keybindConflict(*this, spec.command, 0, *chord)) {
        m_suppressedDefaults.set(static_cast<std::size_t>(spec.command));
      }
    }
  }

  namespace {
    KeybindConfig& activeKeybindStorage() {
      static KeybindConfig config;
      return config;
    }
  } // namespace

  const KeybindConfig& activeKeybinds() { return activeKeybindStorage(); }

  void setActiveKeybinds(const KeybindConfig& config) { activeKeybindStorage() = config; }

  bool keybindMatches(const KeybindConfig& config, KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers) {
    return std::ranges::any_of(config.chords(command), [sym, modifiers](const auto& chord) {
      return chord && keybindChordMatches(*chord, sym, modifiers);
    });
  }

  bool keybindMatches(KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers) {
    return keybindMatches(activeKeybinds(), command, sym, modifiers);
  }

  std::span<const KeybindCommand> keybindMovementCommands() {
    static constexpr std::array<KeybindCommand, kMovementCommandCount> commands{
        KeybindCommand::MoveLeft,  KeybindCommand::MoveRight, KeybindCommand::MoveUp,     KeybindCommand::MoveDown,
        KeybindCommand::MoveFirst, KeybindCommand::MoveLast,  KeybindCommand::MovePageUp, KeybindCommand::MovePageDown,
    };
    return commands;
  }

  std::optional<KeybindMatch>
  keybindBestMatch(std::span<const KeybindCommand> commands, std::uint32_t sym, std::uint32_t modifiers) {
    const KeybindChord received = normalizeKeybindChord({sym, modifiers});
    std::optional<KeybindMatch> best;
    for (const auto command : commands) {
      for (const auto& stored : activeKeybinds().chords(command)) {
        if (!stored) {
          continue;
        }
        const KeybindChord chord = normalizeKeybindChord(*stored);
        if (chord.sym != received.sym || (chord.modifiers & ~received.modifiers) != 0) {
          continue;
        }
        if (best && std::popcount(chord.modifiers) <= std::popcount(best->chord.modifiers)) {
          continue;
        }
        best = KeybindMatch{command, chord, received.modifiers & ~chord.modifiers};
      }
    }
    return best;
  }

  std::string keybindShortcutLabel(const KeybindChords& chords) {
    std::string label;
    for (const auto& chord : chords) {
      if (!chord) {
        continue;
      }
      if (!label.empty()) {
        label += " / ";
      }
      label += keybindChordLabel(*chord);
    }
    return label;
  }

  std::string keybindShortcutLabel(const KeybindConfig& config, KeybindCommand command) {
    return keybindShortcutLabel(config.chords(command));
  }

  std::string keybindShortcutLabel(KeybindCommand command) { return keybindShortcutLabel(activeKeybinds(), command); }

  std::optional<KeybindCommand>
  keybindConflict(const KeybindConfig& config, KeybindCommand command, std::size_t slot, const KeybindChord& chord) {
    const KeybindChord target = normalizeKeybindChord(chord);
    for (const auto candidate : keybindCommands(keybindCommandScope(command))) {
      const auto& chords = config.chords(candidate);
      for (std::size_t index = 0; index < chords.size(); ++index) {
        if (candidate == command && index == slot) {
          continue;
        }
        if (chords[index] && normalizeKeybindChord(*chords[index]) == target) {
          return candidate;
        }
      }
    }
    return std::nullopt;
  }

  bool keybindConfigValid(const KeybindConfig& config) {
    for (const auto scope : kKeybindScopes) {
      for (const auto command : keybindCommands(scope)) {
        const bool optionalPrintable = command == KeybindCommand::Validate || command == KeybindCommand::QuickPreview;
        const auto& chords = config.chords(command);
        for (std::size_t slot = 0; slot < chords.size(); ++slot) {
          if (!chords[slot]) {
            continue;
          }
          const auto& chord = *chords[slot];
          if (printableConfigKey(chord.sym)
              && ((chord.modifiers == 0 && !optionalPrintable) || chord.modifiers == KeybindMod::Shift)) {
            return false;
          }
          if (keybindConflict(config, command, slot, chord)) {
            return false;
          }
        }
      }
    }
    return true;
  }

  void
  setKeybindChord(KeybindConfig& config, KeybindCommand command, std::size_t slot, std::optional<KeybindChord> chord) {
    const auto& chords = config.chords(command);
    if (slot >= chords.size()) {
      return;
    }
    KeybindChords compact{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < chords.size(); ++index) {
      if (index != slot && chords[index]) {
        compact[count++] = chords[index];
      }
    }
    if (chord && chord->sym != 0) {
      const std::size_t at = std::min(slot, count);
      if (at < compact.size()) {
        for (std::size_t index = std::min(count, compact.size() - 1); index > at; --index) {
          compact[index] = compact[index - 1];
        }
        compact[at] = normalizeKeybindChord(*chord);
      }
    }
    config.setChords(command, compact);
  }
} // namespace luciole
