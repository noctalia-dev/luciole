#include "app/page/file_keyboard_controller.h"

#include "app/page/location_bar_controller.h"
#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "app/page/status_bar_controller.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/miller_columns.h"
#include "core/input/key_symbols.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/controls/input.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <glib.h>
#include <iterator>
#include <utility>

using Action = luciole::BrowserAction;

FileKeyboardController::FileKeyboardController(
    PageContext& page, TextInputService& textInput, luciole::MillerColumns& miller, MenuController& menus,
    LocationBarController& locationBar, StatusBarController& status
)
    : m_page(page), m_textInput(textInput), m_miller(miller), m_menus(menus), m_locationBar(locationBar),
      m_status(status) {}

bool FileKeyboardController::listFocused() const {
  return m_page.fileList() != nullptr && m_page.dispatcher().focusedArea() == m_page.fileList()->focusArea();
}

void FileKeyboardController::syncContext() {
  if (m_typeaheadContext != m_page.browser().contextSerial() || !m_page.browser().model().selectionAvailable()) {
    clearTypeahead();
  }
}

void FileKeyboardController::notifyStateChanged() {
  if (listFocused()) {
    m_textInput.notifyClientStateChanged(this, TextInputChangeCause::Other);
  }
}

bool FileKeyboardController::escapeKey(const KeyboardEvent& event) {
  if (event.pressed
      && event.modifiers == 0
      && KeySymbol::isEscape(event.sym)
      && (!m_typeahead.text().empty() || !m_typeaheadPreedit.empty())) {
    clearTypeahead();
    return true;
  }
  return false;
}

bool FileKeyboardController::preeditKey(const KeyboardEvent& event) {
  if (!m_typeaheadPreedit.empty()) {
    if (KeySymbol::isEscape(event.sym)) {
      clearTypeahead();
    }
    return true;
  }
  return false;
}

bool FileKeyboardController::commandKey(const KeyboardEvent& event) {
  if (m_page.fileList() != nullptr
      && m_page.dispatcher().focusedArea() == m_page.fileList()->focusArea()
      && (event.sym == XKB_KEY_Menu || (event.sym == XKB_KEY_F10 && event.modifiers == KeyMod::Shift))) {
    auto id = m_page.browser().model().selection().focused;
    float x = 240, y = 100;
    if (id) {
      if (!m_page.browser().model().selection().selected.contains(*id)) {
        m_page.browser().model().select(*id);
      }
      const auto& rows = m_page.browser().model().rows();
      const auto found = std::ranges::find(rows, *id);
      if (found != rows.end()) {
        if (const auto rect = m_page.fileList()->itemRect(static_cast<std::size_t>(found - rows.begin()))) {
          x = rect->x + rect->width * 0.5F;
          y = rect->y + rect->height * 0.5F;
        }
      }
    }
    m_menus.openFileMenu(id, x, y);
    return true;
  }
  if (m_page.fileList() != nullptr
      && m_page.dispatcher().focusedArea() == m_page.fileList()->focusArea()
      && !event.repeat) {
    if (event.sym == XKB_KEY_Delete && (event.modifiers == 0 || event.modifiers == KeyMod::Shift)) {
      const auto& folder = m_page.browser().model().snapshot().location;
      if (folder && folder->isTrash()) {
        m_page.dispatchAction(Action::DeletePermanently);
      } else if (event.modifiers == 0) {
        m_page.dispatchAction(Action::Trash);
      }
      return true;
    }
    if (const auto action = luciole::shortcutAction({Action::Rename, Action::Properties}, event.sym, event.modifiers)) {
      m_page.dispatchAction(*action);
      return true;
    }
  }
  return false;
}

bool FileKeyboardController::typeKey(const KeyboardEvent& event) {
  if (event.pressed
      && !event.preedit
      && m_typingMode == luciole::TypingMode::Typeahead
      && m_page.fileList() != nullptr
      && m_page.dispatcher().focusedArea() == m_page.fileList()->focusArea()
      && (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0) {
    if (g_unichar_isprint(event.utf32)) {
      char text[8]{};
      const int length = g_unichar_to_utf8(event.utf32, text);
      applyTypeahead(std::string(text, static_cast<std::size_t>(length)));
      return true;
    }
    if (event.sym == XKB_KEY_BackSpace && !m_typeahead.text().empty()) {
      const auto& text = m_typeahead.text();
      const char* previous = g_utf8_find_prev_char(text.data(), text.data() + text.size());
      typeaheadChanged(m_typeahead.erase(
          m_page.browser().model(), static_cast<std::uint32_t>(text.data() + text.size() - previous),
          luciole::TypeaheadSelection::Clock::now()
      ));
      return true;
    }
  }
  if (event.pressed
      && !event.preedit
      && m_typingMode != luciole::TypingMode::Typeahead
      && m_page.fileList() != nullptr
      && m_page.dispatcher().focusedArea() == m_page.fileList()->focusArea()
      && (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0
      && (g_unichar_isprint(event.utf32) || (m_locationBar.filterOpen() && event.sym == XKB_KEY_BackSpace))) {
    m_locationBar.openFilter(false);
  }
  return false;
}

void FileKeyboardController::clearTypeahead() {
  if (m_typeahead.text().empty() && m_typeaheadPreedit.empty()) {
    return;
  }
  m_typeahead.clear();
  m_typeaheadPreedit.clear();
  m_typeaheadTimer.stop();
  m_page.requestSync();
  m_textInput.notifyClientStateChanged(this, TextInputChangeCause::Other);
}

void FileKeyboardController::typeaheadChanged(std::optional<luciole::EntryId> match, TextInputChangeCause cause) {
  if (match) {
    m_keyboardReveal = match;
  }
  m_typeaheadContext = m_page.browser().contextSerial();
  m_fileKeyboardFocus = true;
  m_page.requestSync();
  m_typeaheadTimer.start(luciole::TypeaheadSelection::timeout, [this] {
    if (m_typeaheadPreedit.empty()) {
      clearTypeahead();
    }
  });
  m_textInput.notifyClientStateChanged(this, cause);
}

void FileKeyboardController::applyTypeahead(const std::string& text, TextInputChangeCause cause, bool composing) {
  m_page.dispatcher().cancelPointerCapture();
  m_page.fileList()->cancelPointerGesture();
  typeaheadChanged(
      m_typeahead.append(m_page.browser().model(), text, luciole::TypeaheadSelection::Clock::now(), composing), cause
  );
}

TextInputState FileKeyboardController::textInputState() const {
  if (m_typingMode != luciole::TypingMode::Typeahead) {
    return m_locationBar.filterInput().textInputState();
  }
  const auto bounds = m_status.statusRect();
  return {
      .surroundingText = m_typeahead.text(),
      .cursor = static_cast<std::int32_t>(m_typeahead.text().size()),
      .anchor = static_cast<std::int32_t>(m_typeahead.text().size()),
      .cursorRectX = static_cast<int>(bounds.x),
      .cursorRectY = static_cast<int>(bounds.y),
      .cursorRectHeight = std::max(1, static_cast<int>(bounds.height))
  };
}

void FileKeyboardController::textInputApplyEdit(const TextInputEdit& edit) {
  if (m_typingMode == luciole::TypingMode::Typeahead) {
    const auto now = luciole::TypeaheadSelection::Clock::now();
    if (edit.hasDelete && edit.deleteBeforeLength != 0 && edit.deleteAfterLength == 0 && !m_typeahead.text().empty()) {
      typeaheadChanged(
          m_typeahead.erase(m_page.browser().model(), edit.deleteBeforeLength, now, !m_typeaheadPreedit.empty()),
          TextInputChangeCause::InputMethod
      );
    }
    if (edit.hasCommitText && !edit.commitText.empty()) {
      const bool composing = !m_typeaheadPreedit.empty();
      m_typeaheadPreedit.clear();
      applyTypeahead(edit.commitText, TextInputChangeCause::InputMethod, composing);
    }
    if (edit.hasPreedit
        && (!edit.preeditText.empty() || !m_typeaheadPreedit.empty())
        && edit.preeditText.size() <= 256
        && g_utf8_validate(edit.preeditText.data(), static_cast<gssize>(edit.preeditText.size()), nullptr)) {
      m_typeaheadPreedit = edit.preeditText;
      m_typeaheadContext = m_page.browser().contextSerial();
      if (!m_typeaheadPreedit.empty()) {
        m_typeaheadTimer.stop();
      } else {
        typeaheadChanged(std::nullopt, TextInputChangeCause::InputMethod);
      }
      m_page.requestSync();
    }
    return;
  }
  if ((edit.hasCommitText && !edit.commitText.empty())
      || (edit.hasPreedit && !edit.preeditText.empty())
      || (edit.hasDelete && !m_page.browser().searchQuery().empty())) {
    m_locationBar.openFilter(false, true);
    m_locationBar.filterInput().textInputApplyEdit(edit);
  }
}

void FileKeyboardController::textInputResetPreedit() {
  if (!m_typeaheadPreedit.empty()) {
    m_typeaheadPreedit.clear();
    m_page.requestSync();
  }
}

void FileKeyboardController::textInputActivated(TextInputService&) {}

void FileKeyboardController::textInputDeactivated(TextInputService&) { clearTypeahead(); }

bool FileKeyboardController::listKey(const KeyboardEvent& event) {
  if (listFocused()
      && event.pressed
      && event.modifiers == 0
      && (KeySymbol::isLeft(event.sym) || KeySymbol::isRight(event.sym))
      && m_page.browser().treeKey(KeySymbol::isRight(event.sym) != Style::rtl())) {
    m_keyboardReveal = m_page.browser().model().selection().focused;
    m_page.requestSync();
    return true;
  }
  auto& model = m_page.browser().model();
  if (const auto action =
          luciole::shortcutAction({Action::SelectAll, Action::ClearSelection}, event.sym, event.modifiers)) {
    m_page.dispatchAction(*action);
    return true;
  }
  if (const auto action =
          luciole::shortcutAction({Action::InvertSelection, Action::SelectPattern}, event.sym, event.modifiers)) {
    if (!event.repeat) {
      m_page.dispatchAction(*action);
    }
    return true;
  }
  if (event.modifiers == 0 && KeySymbol::isEnter(event.sym)) {
    const auto& selection = model.selection();
    const bool hasOpenTarget = !selection.selected.empty()
        || (selection.focused && (m_fileKeyboardFocus || selection.selected.contains(*selection.focused)));
    if (!event.repeat && hasOpenTarget) {
      auto request = m_page.actionRequest(true);
      if (request.targets.empty() && selection.focused) {
        const auto* entry = model.entry(*selection.focused);
        if (entry) {
          request.targets.push_back({entry->id, entry->location()});
        }
      }
      m_page.dispatchAction(Action::Open, request);
    }
    return true;
  }
  if (model.rows().empty() || (event.modifiers & (KeyMod::Alt | KeyMod::Super)) != 0) {
    return false;
  }
  auto index = std::int64_t{0};
  if (model.selection().focused) {
    const auto found = std::ranges::find(model.rows(), *model.selection().focused);
    if (found != model.rows().end()) {
      index = std::distance(model.rows().begin(), found);
    }
  }
  const bool hasFocus = model.selection().focused.has_value();
  const auto columns = static_cast<std::int64_t>(std::max<std::size_t>(1, m_page.fileList()->layoutColumnCount()));
  if (event.sym == XKB_KEY_Down) {
    index += hasFocus ? columns : 0;
  } else if (event.sym == XKB_KEY_Up) {
    index += hasFocus ? -columns : 0;
  } else if (m_page.viewMode() == luciole::ViewMode::Grid && event.sym == XKB_KEY_Right) {
    index += hasFocus ? (Style::rtl() ? -1 : 1) : 0;
  } else if (m_page.viewMode() == luciole::ViewMode::Grid && event.sym == XKB_KEY_Left) {
    index += hasFocus ? (Style::rtl() ? 1 : -1) : 0;
  } else if (event.sym == XKB_KEY_Home) {
    index = 0;
  } else if (event.sym == XKB_KEY_End) {
    index = static_cast<std::int64_t>(model.rows().size()) - 1;
  } else if (event.sym == XKB_KEY_Page_Down) {
    index += static_cast<std::int64_t>(m_page.fileList()->pageItemStride());
  } else if (event.sym == XKB_KEY_Page_Up) {
    index -= static_cast<std::int64_t>(m_page.fileList()->pageItemStride());
  } else {
    return false;
  }
  const auto row =
      static_cast<std::size_t>(std::clamp(index, std::int64_t{0}, static_cast<std::int64_t>(model.rows().size()) - 1));
  const auto id = model.rows()[row];
  const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
  const bool shift = (event.modifiers & KeyMod::Shift) != 0;
  if (ctrl && !shift) {
    (void)model.setFocus(id);
  } else {
    if (m_miller.enabled()) {
      m_miller.interact(m_miller.focused());
    }
    (void)model.select(
        id,
        shift ? (ctrl ? luciole::SelectionMode::AddRange : luciole::SelectionMode::Range)
              : luciole::SelectionMode::Replace
    );
  }
  m_keyboardReveal = id;
  return true;
}
