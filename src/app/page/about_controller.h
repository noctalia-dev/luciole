#pragma once

#include <string>

namespace luciole {
  class FileLauncher;
} // namespace luciole

class InputArea;
class Image;
class Label;
class PageContext;
class ScrollView;

// The About dialog.
class AboutController {
public:
  AboutController(PageContext& page, luciole::FileLauncher& launcher);

  void open();
  // Launcher change: shows why a link did not open, or hides the message once one did.
  void launcherChanged(const std::string& error);
  // Popup prepare-frame hook: loads the logo once the popup renderer is current.
  void layoutDialog();
  // Focus hook of the popup dispatcher: scrolls the focused control into view.
  void revealFocus(InputArea* focused);

private:
  PageContext& m_page;
  luciole::FileLauncher& m_launcher;
  Image* m_logo = nullptr;
  int m_logoPixels = 0;
  ScrollView* m_scroll = nullptr;
  Label* m_message = nullptr;
};
