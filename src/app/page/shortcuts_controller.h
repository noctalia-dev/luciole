#pragma once

struct KeyboardEvent;
class PageContext;
class ScrollView;

// The keyboard shortcuts dialog.
class ShortcutsController {
public:
  explicit ShortcutsController(PageContext& page);

  void open();
  // Popup keyboard hook: Up, Down, Page Up, Page Down, Home and End scroll the list.
  bool key(const KeyboardEvent& event);

private:
  PageContext& m_page;
  ScrollView* m_scroll = nullptr;
};
