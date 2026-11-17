#pragma once

#include "render/scene/node.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace luciole {
  class PinService;
} // namespace luciole

class FileKeyboardController;
class Flex;
class Glyph;
class Label;
class PageContext;
class Renderer;
class SettingsController;

// The floating status pill and the empty-folder notice: the last
// action message, search and selection summaries, free space, and the notice shown in place of an empty file list.
// The pill shows only when there is something to report unless the Always show status bar setting keeps the idle
// folder count visible.
class StatusBarController {
public:
  StatusBarController(
      PageContext& page, luciole::PinService& pins, SettingsController& settings, FileKeyboardController& keys
  );

  // The last action message; shown alongside search/selection information.
  void setMessage(std::string message) { m_actionError = std::move(message); }
  void clearMessage() { m_actionError.clear(); }

  // Scene: the status pill (an overlay child of the page body) and the notice inside the file pane.
  std::unique_ptr<Flex> build();
  std::unique_ptr<Flex> buildNotice();

  // BrowserPage::syncBrowser(): status text, free space and the notice for an empty list.
  void sync(bool miller);
  // Layout hook in BrowserPage::prepareFrame(): free space shows only in a pane at least that wide.
  void layout(float paneWidth);
  // Places the pill at the bottom-right of the page body (`width` x `height`, body coordinates).
  void arrange(Renderer& renderer, float width, float height);
  // True while the notice replaces the file list.
  bool noticeShown() const;
  // Scene rectangle of the status text, where the input method places its candidates.
  LayoutRect statusRect() const;

private:
  PageContext& m_page;
  luciole::PinService& m_pins;
  SettingsController& m_settings;
  FileKeyboardController& m_keys;
  std::string m_actionError;
  std::uint64_t m_statusContext = 0;
  bool m_contextLoaded = false;
  std::optional<std::uint64_t> m_freeBytes;
  Flex* m_pill = nullptr;
  Label* m_status = nullptr;
  Label* m_freeSpace = nullptr;
  bool m_freeSpaceAvailable = false;
  bool m_needed = false;
  Glyph* m_noticeGlyph = nullptr;
  Label* m_notice = nullptr;
  Label* m_noticeDetail = nullptr;
  Flex* m_noticeArea = nullptr;
};
