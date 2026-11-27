#include "ui/browser/action_shortcuts.h"

#include "config/keybinds.h"
#include "i18n/i18n.h"

#include <format>
#include <string>
#include <string_view>

bool luciole::shortcutMatches(BrowserAction action, std::uint32_t sym, std::uint32_t modifiers) {
  const auto command = ActionRegistry::definition(action).keybind;
  return command && keybindMatches(*command, sym, modifiers);
}

std::optional<luciole::BrowserAction>
luciole::shortcutAction(std::initializer_list<BrowserAction> actions, std::uint32_t sym, std::uint32_t modifiers) {
  for (const auto action : actions) {
    if (shortcutMatches(action, sym, modifiers)) {
      return action;
    }
  }
  return std::nullopt;
}

std::string luciole::actionShortcutLabel(BrowserAction action) {
  const auto command = ActionRegistry::definition(action).keybind;
  return command ? keybindShortcutLabel(*command) : std::string();
}

std::string luciole::shortcutTooltip(std::string_view text, BrowserAction action) {
  const std::string shortcut = actionShortcutLabel(action);
  const auto translated = i18n::text(text);
  return shortcut.empty() ? translated : std::format("{} ({})", translated, shortcut);
}

std::string luciole::shortcutTooltip(std::string_view text, KeybindCommand command) {
  const std::string shortcut = keybindShortcutLabel(command);
  const auto translated = i18n::text(text);
  return shortcut.empty() ? translated : std::format("{} ({})", translated, shortcut);
}
