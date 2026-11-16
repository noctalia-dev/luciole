#pragma once

#include "browser/directory_model.h"
#include "browser/selection_tools.h"
#include "core/timer_manager.h"
#include "model/directory_options.h"
#include "wayland/text_input_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace luciole {
  class BrowserController;
  class MillerColumns;
} // namespace luciole

struct KeyboardEvent;
class LocationBarController;
class MenuController;
class PageContext;
class StatusBarController;

// Keyboard use of the file list: moving the focus and selection, the list's menu and command keys, typeahead
// selection, and the text input client that feeds typed text to typeahead or the search field.
class FileKeyboardController final : public TextInputClient {
public:
  FileKeyboardController(
      PageContext& page, TextInputService& textInput, luciole::MillerColumns& miller, MenuController& menus,
      LocationBarController& locationBar, StatusBarController& status
  );

  void setTypingMode(luciole::TypingMode mode) { m_typingMode = mode; }
  // True while the list shows the keyboard focus ring; clearing it drops a pending keyboard cursor.
  bool keyboardFocus() const { return m_fileKeyboardFocus; }
  void setKeyboardFocus(bool focus);
  // The item a key moved to, which the next sync scrolls into view.
  std::optional<luciole::EntryId> takeReveal() { return std::exchange(m_keyboardReveal, std::nullopt); }
  void resetReveal() { m_keyboardReveal.reset(); }
  // Keyboard entry into a folder: selects the first row of the next visit once it loaded.
  void selectFirstOnLoad();
  void cancelPendingCursor() { m_pendingCursor.reset(); }
  // BrowserPage::syncBrowser(): applies a pending keyboard cursor once the entered folder loaded.
  void applyPendingCursor();
  // Selects the first row when nothing is selected or focused.
  void selectFirstIfNone();
  // Focus moved into the list without navigating: shows the cursor ring, on the first row if there is no cursor.
  void focusEntered();

  // Typed text, including the input method's preedit; empty while typeahead is idle.
  std::string typeaheadText() const { return m_typeahead.text() + m_typeaheadPreedit; }
  void clearTypeahead();
  // BrowserPage::syncBrowser(): ends typeahead once the folder or its selection changed.
  void syncContext();
  // Reports the client state to the input method, while the list has focus.
  void notifyStateChanged();

  // Keyboard hooks in BrowserPage::keyboard(), in this order.
  // Escape ends typeahead.
  bool escapeKey(const KeyboardEvent& event);
  // Swallows keys while the input method composes typeahead text.
  bool preeditKey(const KeyboardEvent& event);
  // Menu, Delete, F2 and Alt+Return on the focused list.
  bool commandKey(const KeyboardEvent& event);
  // Selection, cursor and folder navigation keys on the focused list (arrows, Enter, Backspace, Ctrl+Space,
  // Ctrl+Enter).
  bool listKey(const KeyboardEvent& event);
  // Printable keys: typeahead, or opening the search field.
  bool typeKey(const KeyboardEvent& event);

  TextInputState textInputState() const override;
  void textInputApplyEdit(const TextInputEdit& edit) override;
  void textInputResetPreedit() override;
  void textInputActivated(TextInputService&) override;
  void textInputDeactivated(TextInputService&) override;

private:
  bool listFocused() const;
  void goUp();
  // Folder entry and parent keys, Ctrl+Space and Ctrl+Enter.
  bool folderKey(const KeyboardEvent& event);
  void enterCursorFolder();
  void
  typeaheadChanged(std::optional<luciole::EntryId> match, TextInputChangeCause cause = TextInputChangeCause::Other);
  void applyTypeahead(
      const std::string& text, TextInputChangeCause cause = TextInputChangeCause::Other, bool composing = false
  );

  PageContext& m_page;
  TextInputService& m_textInput;
  luciole::MillerColumns& m_miller;
  MenuController& m_menus;
  LocationBarController& m_locationBar;
  StatusBarController& m_status;
  luciole::TypingMode m_typingMode = luciole::TypingMode::Search;
  luciole::TypeaheadSelection m_typeahead;
  std::string m_typeaheadPreedit;
  Timer m_typeaheadTimer;
  std::uint64_t m_typeaheadContext = 0;
  bool m_fileKeyboardFocus = false;
  std::optional<luciole::EntryId> m_keyboardReveal;
  // The browser and visit a keyboard entry started from; the first row of the next visit is selected once loaded.
  struct PendingCursor {
    const luciole::BrowserController* browser;
    std::uint64_t visit;
  };
  std::optional<PendingCursor> m_pendingCursor;
};
