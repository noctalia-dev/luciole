#include "app/page/shortcuts_controller.h"

#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/action_registry.h"
#include "config/keybinds.h"
#include "config/settings.h"
#include "i18n/i18n.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using Shortcut = std::pair<std::string, std::string>;
  using K = luciole::KeybindCommand;

  std::string joinShortcuts(std::initializer_list<K> commands, std::string_view separator) {
    std::string label;
    for (const auto command : commands) {
      const std::string part = luciole::keybindShortcutLabel(command);
      if (part.empty()) {
        continue;
      }
      if (!label.empty()) {
        label += separator;
      }
      label += part;
    }
    return label;
  }

  std::vector<Shortcut> fileShortcuts() {
    return {
        {"Move the cursor", joinShortcuts({K::MoveUp, K::MoveDown}, " / ")},
        {"Enter folder (List, Columns)", luciole::keybindShortcutLabel(K::MoveRight)},
        {"Parent folder (List, Columns)", luciole::keybindShortcutLabel(K::MoveLeft)},
        {"Parent folder", luciole::keybindShortcutLabel(K::ParentFolder)},
        {"Open folder in a new tab", luciole::keybindShortcutLabel(K::OpenInNewTab)},
        {"Open folder in a new tab, opposite focus preference",
         luciole::keybindShortcutLabel(K::OpenInNewTabBackground)},
        {"Open folder in the other pane", luciole::keybindShortcutLabel(K::OpenInOtherPane)},
        {"Open folder in a new window", luciole::keybindShortcutLabel(K::OpenInNewWindow)},
        {"First / last item", joinShortcuts({K::MoveFirst, K::MoveLast}, " / ")},
        {"Previous / next page", joinShortcuts({K::MovePageUp, K::MovePageDown}, " / ")},
        {"Extend the selection", "Shift+Arrow keys"},
        {"Move the cursor without selecting", "Ctrl+Arrow keys"},
        {"Select or unselect the item under the cursor", luciole::keybindShortcutLabel(K::ToggleSelection)},
        {"Item menu", luciole::keybindShortcutLabel(K::ContextMenu)},
        {"Return to the file list", luciole::keybindShortcutLabel(K::Cancel)},
        {"Clear selection", luciole::keybindShortcutLabel(K::ClearSelection)},
        {"Next / previous control", joinShortcuts({K::FocusNext, K::FocusPrevious}, " / ")},
        {"Search files", luciole::keybindShortcutLabel(K::Search)},
        {"Icon size", joinShortcuts({K::ZoomIn, K::ZoomOut, K::ZoomReset}, " / ")},
    };
  }

  std::vector<Shortcut> windowShortcuts() {
    return {
        {"New tab / new window", joinShortcuts({K::NewTab, K::NewWindow}, " / ")},
        {"Duplicate tab / reopen closed tab", joinShortcuts({K::DuplicateTab, K::ReopenTab}, " / ")},
        {"Close tab / close focused split pane", joinShortcuts({K::CloseTab, K::ClosePane}, " / ")},
        {"Quit / close window", joinShortcuts({K::Quit, K::CloseWindow}, " / ")},
        {"Next / previous tab", joinShortcuts({K::NextTab, K::PreviousTab}, " / ")},
        {"Go to tab (9: last)",
         joinShortcuts(
             {K::SelectTab1, K::SelectTab2, K::SelectTab3, K::SelectTab4, K::SelectTab5, K::SelectTab6, K::SelectTab7,
              K::SelectTab8, K::SelectTab9},
             " … "
         )},
        {"Toggle split / switch pane", joinShortcuts({K::SplitView, K::SwitchArea}, " / ")},
        {"Full screen", luciole::keybindShortcutLabel(K::Fullscreen)},
    };
  }

  std::unique_ptr<Label> plainLabel(std::string_view text, float size, ColorSpec color, float grow) {
    return ui::label(
        {.text = i18n::text(text),
         .fontSize = size,
         .color = color,
         .maxLines = 2,
         .flexGrow = grow,
         .configure = [](Label& label) { label.setUseMarkup(false); }}
    );
  }
} // namespace

ShortcutsController::ShortcutsController(PageContext& page) : m_page(page) {}

void ShortcutsController::open() {
  ScrollView* scroll = nullptr;
  auto body = ui::scrollView(
      {.out = &scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F}
  );
  auto* content = body->content();
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceSm);
  const auto heading = [&](std::string_view text) {
    content->addChild(
        ui::label(
            {.text = std::string(text),
             .fontSize = Style::fontSizeBody,
             .fontWeight = FontWeight::SemiBold,
             .configure = [](Label& label) { label.setUseMarkup(false); }}
        )
    );
  };
  const auto row = [&](std::string_view text, std::string_view keys) {
    if (keys.empty()) {
      return;
    }
    content->addChild(
        ui::row(
            {.justify = FlexJustify::SpaceBetween, .gap = Style::spaceMd},
            plainLabel(text, Style::fontSizeBody, colorSpecFromRole(ColorRole::OnSurface), 1.0F),
            plainLabel(keys, Style::fontSizeCaption, colorSpecFromRole(ColorRole::OnSurfaceVariant), 0.0F)
        )
    );
  };
  const bool singleWindow = m_page.settings().singleWindow;
  heading(i18n::text("Files and Folders"));
  for (const auto& [text, keys] : fileShortcuts()) {
    row(i18n::text(singleWindow && text == "Open folder in a new window" ? "Open folder in a new tab" : text), keys);
  }
  heading(i18n::text("Tabs and Windows"));
  for (const auto& [text, keys] : windowShortcuts()) {
    row(i18n::text(singleWindow && text == "New tab / new window" ? "New tab" : text), keys);
  }
  heading(i18n::text("Commands"));
  for (const auto& definition : luciole::ActionRegistry::definitions()) {
    const std::string keys = luciole::actionShortcutLabel(definition.action);
    if (!keys.empty()) {
      row(i18n::tr(definition.label), keys);
    }
  }
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("Keyboard Shortcuts", "keyboard", [this] { m_page.dismissPopup(); }), std::move(body)
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Medium, PopupDismiss::OutsideClick);
  if (m_page.popupSurface()) {
    m_page.activePopup().bind(ActivePopup::Kind::KeyboardShortcuts, [this] { m_scroll = nullptr; });
    m_scroll = scroll;
  }
}

bool ShortcutsController::key(const KeyboardEvent& event) {
  if (m_scroll == nullptr || !event.pressed || m_page.activePopup().kind() != ActivePopup::Kind::KeyboardShortcuts) {
    return false;
  }
  using K = luciole::KeybindCommand;
  const float page = m_scroll->contentViewportHeight() * 0.9F;
  if (luciole::keybindMatches(K::MoveUp, event.sym, event.modifiers)) {
    m_scroll->scrollBy(-Style::scrollWheelStep);
  } else if (luciole::keybindMatches(K::MoveDown, event.sym, event.modifiers)) {
    m_scroll->scrollBy(Style::scrollWheelStep);
  } else if (luciole::keybindMatches(K::MovePageUp, event.sym, event.modifiers)) {
    m_scroll->scrollBy(-page);
  } else if (luciole::keybindMatches(K::MovePageDown, event.sym, event.modifiers)) {
    m_scroll->scrollBy(page);
  } else if (luciole::keybindMatches(K::MoveFirst, event.sym, event.modifiers)) {
    m_scroll->setScrollOffset(0.0F);
  } else if (luciole::keybindMatches(K::MoveLast, event.sym, event.modifiers)) {
    m_scroll->setScrollOffset(m_scroll->maxScrollOffset());
  } else {
    return false;
  }
  m_page.popupSurface()->requestLayout();
  return true;
}
