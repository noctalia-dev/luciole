#include "app/page/file_keyboard_controller.h"

#include "app/page/location_bar_controller.h"
#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "app/page/status_bar_controller.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/miller_columns.h"
#include "config/keybinds.h"
#include "config/settings.h"
#include "core/input/key_symbols.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/controls/input.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <glib.h>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

namespace {
  using K = luciole::KeybindCommand;
} // namespace

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

void FileKeyboardController::setKeyboardFocus(bool focus) {
  m_fileKeyboardFocus = focus;
  if (!focus) {
    m_pendingCursor.reset();
  }
}

void FileKeyboardController::selectFirstOnLoad() {
  if (m_page.choosing()) {
    return;
  }
  m_pendingCursor = PendingCursor{.browser = &m_page.browser(), .visit = m_page.browser().visitSerial()};
}

void FileKeyboardController::applyPendingCursor() {
  if (!m_pendingCursor) {
    return;
  }
  auto& browser = m_page.browser();
  if (&browser == m_pendingCursor->browser && browser.visitSerial() == m_pendingCursor->visit) {
    return;
  }
  const auto status = browser.model().snapshot().status;
  if (status == luciole::DirectoryStatus::Loading || status == luciole::DirectoryStatus::Idle) {
    return;
  }
  m_pendingCursor.reset();
  selectFirstIfNone();
}

void FileKeyboardController::selectFirstIfNone() {
  auto& model = m_page.browser().model();
  const auto& selection = model.selection();
  if (model.rows().empty() || selection.focused || !selection.selected.empty()) {
    return;
  }
  const auto first = model.rows().front();
  if (model.select(first)) {
    m_keyboardReveal = first;
    m_fileKeyboardFocus = true;
    m_page.requestSync();
  }
}

void FileKeyboardController::focusEntered() {
  m_fileKeyboardFocus = true;
  auto& model = m_page.browser().model();
  if (!model.selection().focused && !model.rows().empty() && model.setFocus(model.rows().front())) {
    m_keyboardReveal = model.rows().front();
  }
  m_page.requestSync();
}

void FileKeyboardController::goUp() {
  if (m_page.actionState(Action::Up, m_page.actionRequest(false)).enabled) {
    m_page.dispatchAction(Action::Up);
  }
}

void FileKeyboardController::enterCursorFolder() {
  const auto& model = m_page.browser().model();
  const auto focused = model.selection().focused;
  const auto* entry = focused ? model.entry(*focused) : nullptr;
  if (entry == nullptr || !entry->isFolderLink()) {
    return;
  }
  auto request = m_page.actionRequest(false);
  request.targets.push_back({entry->id, entry->location()});
  selectFirstOnLoad();
  m_page.dispatchAction(Action::Open, request);
}

bool FileKeyboardController::escapeKey(const KeyboardEvent& event) {
  if (event.pressed
      && luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers)
      && (!m_typeahead.text().empty() || !m_typeaheadPreedit.empty())) {
    clearTypeahead();
    return true;
  }
  return false;
}

bool FileKeyboardController::preeditKey(const KeyboardEvent& event) {
  if (!m_typeaheadPreedit.empty()) {
    if (luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers)) {
      clearTypeahead();
    }
    return true;
  }
  return false;
}

bool FileKeyboardController::commandKey(const KeyboardEvent& event) {
  if (m_page.fileList() != nullptr
      && m_page.dispatcher().focusedArea() == m_page.fileList()->focusArea()
      && luciole::keybindMatches(luciole::KeybindCommand::ContextMenu, event.sym, event.modifiers)) {
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
    const bool permanently =
        luciole::keybindMatches(luciole::KeybindCommand::DeletePermanently, event.sym, event.modifiers);
    const bool trash = luciole::keybindMatches(luciole::KeybindCommand::Trash, event.sym, event.modifiers);
    if (permanently || trash) {
      const auto& folder = m_page.browser().model().snapshot().location;
      if (permanently || (folder && (folder->isTrash() || folder->isAdmin()))) {
        m_page.dispatchAction(Action::DeletePermanently);
      } else {
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

bool FileKeyboardController::folderKey(const KeyboardEvent& event) {
  auto& model = m_page.browser().model();
  const auto focused = model.selection().focused;
  const bool left = luciole::keybindMatches(K::MoveLeft, event.sym, event.modifiers);
  const bool right = luciole::keybindMatches(K::MoveRight, event.sym, event.modifiers);
  if ((left || right) && m_page.viewMode() == luciole::ViewMode::List) {
    if (right != Style::rtl()) {
      if (!event.repeat) {
        enterCursorFolder();
      }
    } else if (m_page.browser().searchQuery().empty()) {
      goUp();
    }
    return true;
  }
  if (luciole::keybindMatches(luciole::KeybindCommand::ParentFolder, event.sym, event.modifiers)
      && m_typeahead.text().empty()
      && m_typeaheadPreedit.empty()
      && !m_locationBar.filterOpen()
      && m_page.browser().searchQuery().empty()) {
    goUp();
    return true;
  }
  if (luciole::keybindMatches(luciole::KeybindCommand::ToggleSelection, event.sym, event.modifiers)) {
    if (focused) {
      if (m_miller.enabled()) {
        m_miller.interact(m_miller.focused());
      }
      (void)model.select(*focused, luciole::SelectionMode::Toggle);
      m_keyboardReveal = focused;
      m_pendingCursor.reset();
    }
    return true;
  }
  const bool openInNewTab = luciole::keybindMatches(luciole::KeybindCommand::OpenInNewTab, event.sym, event.modifiers);
  const bool openInNewTabBackground =
      luciole::keybindMatches(luciole::KeybindCommand::OpenInNewTabBackground, event.sym, event.modifiers);
  const bool openInOtherPane = luciole::keybindMatches(K::OpenInOtherPane, event.sym, event.modifiers);
  const bool openInNewWindow = luciole::keybindMatches(K::OpenInNewWindow, event.sym, event.modifiers);
  if (openInNewTab || openInNewTabBackground || openInOtherPane || openInNewWindow) {
    const auto* entry = focused ? model.entry(*focused) : nullptr;
    if (!event.repeat && !m_page.choosing() && entry != nullptr && entry->isFolderLink()) {
      const auto command = openInOtherPane ? WorkspaceCommand::OpenOther
          : openInNewWindow                ? WorkspaceCommand::NewWindow
          : openInNewTabBackground
          ? (m_page.settings().switchToNewTab ? WorkspaceCommand::NewBackgroundTab : WorkspaceCommand::NewTab)
          : WorkspaceCommand::OpenTab;
      m_page.requestWorkspace(command, entry->navigationLocation(), std::nullopt);
    }
    return true;
  }
  return false;
}

bool FileKeyboardController::listKey(const KeyboardEvent& event) {
  const bool left = luciole::keybindMatches(K::MoveLeft, event.sym, event.modifiers);
  const bool right = luciole::keybindMatches(K::MoveRight, event.sym, event.modifiers);
  if (listFocused() && event.pressed && (left || right) && m_page.browser().treeKey(right != Style::rtl())) {
    m_keyboardReveal = m_page.browser().model().selection().focused;
    m_page.requestSync();
    return true;
  }
  if (event.pressed && folderKey(event)) {
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
  if (luciole::keybindMatches(luciole::KeybindCommand::Open, event.sym, event.modifiers)) {
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
      if (request.targets.size() == 1) {
        const auto* entry = model.entry(request.targets.front().id);
        if (entry != nullptr && entry->isFolderLink()) {
          selectFirstOnLoad();
        }
      }
      m_page.dispatchAction(Action::Open, request);
    }
    return true;
  }
  if (model.rows().empty()) {
    return false;
  }
  std::array<K, luciole::kMovementCommandCount> commands{};
  std::size_t commandCount = 0;
  for (const K command : luciole::keybindMovementCommands()) {
    if ((command == K::MoveLeft || command == K::MoveRight) && m_page.viewMode() != luciole::ViewMode::Grid) {
      continue;
    }
    commands[commandCount++] = command;
  }
  const auto match =
      luciole::keybindBestMatch(std::span<const K>(commands.data(), commandCount), event.sym, event.modifiers);
  if (!match || (match->extraModifiers & (KeyMod::Alt | KeyMod::Super)) != 0) {
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
  switch (match->command) {
  case K::MoveDown:
    index += hasFocus ? columns : 0;
    break;
  case K::MoveUp:
    index += hasFocus ? -columns : 0;
    break;
  case K::MoveRight:
    index += hasFocus ? (Style::rtl() ? -1 : 1) : 0;
    break;
  case K::MoveLeft:
    index += hasFocus ? (Style::rtl() ? 1 : -1) : 0;
    break;
  case K::MoveFirst:
    index = 0;
    break;
  case K::MoveLast:
    index = static_cast<std::int64_t>(model.rows().size()) - 1;
    break;
  case K::MovePageDown:
    index += static_cast<std::int64_t>(m_page.fileList()->pageItemStride());
    break;
  case K::MovePageUp:
    index -= static_cast<std::int64_t>(m_page.fileList()->pageItemStride());
    break;
  default:
    return false;
  }
  const auto row =
      static_cast<std::size_t>(std::clamp(index, std::int64_t{0}, static_cast<std::int64_t>(model.rows().size()) - 1));
  const auto id = model.rows()[row];
  const bool ctrl = (match->extraModifiers & KeyMod::Ctrl) != 0;
  const bool shift = (match->extraModifiers & KeyMod::Shift) != 0;
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
  m_pendingCursor.reset();
  m_keyboardReveal = id;
  return true;
}
