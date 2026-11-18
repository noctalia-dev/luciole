#include "app/page/shortcuts_controller.h"

#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/action_registry.h"
#include "core/input/key_symbols.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace {
  using Shortcut = std::pair<std::string_view, std::string_view>;

  constexpr auto kFiles = std::to_array<Shortcut>({
      {"Move the cursor", "Arrow keys"},
      {"Enter folder (List, Columns)", "Right"},
      {"Parent folder (List, Columns)", "Left"},
      {"Parent folder", "Backspace"},
      {"Open folder in a new tab", "Ctrl+Enter"},
      {"Open folder in a new tab, opposite focus preference", "Ctrl+Shift+Enter"},
      {"First / last item", "Home / End"},
      {"Previous / next page", "Page Up / Page Down"},
      {"Extend the selection", "Shift+Arrow keys"},
      {"Move the cursor without selecting", "Ctrl+Arrow keys"},
      {"Select or unselect the item under the cursor", "Ctrl+Space"},
      {"Item menu", "Menu / Shift+F10"},
      {"Return to the file list", "Escape"},
      {"Next / previous control", "Tab / Shift+Tab"},
      {"Search files", "Ctrl+K"},
      {"Icon size", "Ctrl+Plus / Ctrl+Minus / Ctrl+0"},
  });

  constexpr auto kWindows = std::to_array<Shortcut>({
      {"New tab / new window", "Ctrl+T / Ctrl+N"},
      {"Duplicate tab / reopen closed tab", "Ctrl+Shift+D / Ctrl+Shift+T"},
      {"Close tab / close focused split pane", "Ctrl+W / Ctrl+Alt+W"},
      {"Close window", "Ctrl+Q / Ctrl+Shift+W"},
      {"Next / previous tab", "Ctrl+Tab / Ctrl+Shift+Tab"},
      {"Go to tab (Alt+9: last)", "Alt+1…Alt+9"},
      {"Toggle split / switch pane", "F3 / F6"},
      {"Full screen", "F11"},
  });

  std::unique_ptr<Label> plainLabel(std::string_view text, float size, ColorSpec color, float grow) {
    return ui::label(
        {.text = std::string(text),
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
    content->addChild(
        ui::row(
            {.justify = FlexJustify::SpaceBetween, .gap = Style::spaceMd},
            plainLabel(text, Style::fontSizeBody, colorSpecFromRole(ColorRole::OnSurface), 1.0F),
            plainLabel(keys, Style::fontSizeCaption, colorSpecFromRole(ColorRole::OnSurfaceVariant), 0.0F)
        )
    );
  };
  heading("Files and folders");
  for (const auto& [text, keys] : kFiles) {
    row(text, keys);
  }
  heading("Tabs and windows");
  for (const auto& [text, keys] : kWindows) {
    row(text, keys);
  }
  heading("Commands");
  for (const auto& definition : luciole::ActionRegistry::definitions()) {
    if (!definition.shortcut.empty()) {
      row(definition.label, definition.shortcut);
    }
  }
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("Keyboard shortcuts", "keyboard", [this] { m_page.dismissPopup(); }), std::move(body)
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Medium, PopupDismiss::OutsideClick);
  if (m_page.popupSurface()) {
    m_page.activePopup().bind(ActivePopup::Kind::KeyboardShortcuts, [this] { m_scroll = nullptr; });
    m_scroll = scroll;
  }
}

bool ShortcutsController::key(const KeyboardEvent& event) {
  if (m_scroll == nullptr
      || !event.pressed
      || event.modifiers != 0
      || m_page.activePopup().kind() != ActivePopup::Kind::KeyboardShortcuts) {
    return false;
  }
  const float page = m_scroll->contentViewportHeight() * 0.9F;
  if (KeySymbol::isUp(event.sym) || KeySymbol::isDown(event.sym)) {
    m_scroll->scrollBy(KeySymbol::isUp(event.sym) ? -Style::scrollWheelStep : Style::scrollWheelStep);
  } else if (KeySymbol::isPageUp(event.sym) || KeySymbol::isPageDown(event.sym)) {
    m_scroll->scrollBy(KeySymbol::isPageUp(event.sym) ? -page : page);
  } else if (KeySymbol::isHome(event.sym)) {
    m_scroll->setScrollOffset(0.0F);
  } else if (KeySymbol::isEnd(event.sym)) {
    m_scroll->setScrollOffset(m_scroll->maxScrollOffset());
  } else {
    return false;
  }
  m_page.popupSurface()->requestLayout();
  return true;
}
