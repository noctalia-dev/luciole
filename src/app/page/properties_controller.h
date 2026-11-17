#pragma once

#include <memory>
#include <vector>

namespace luciole {
  class FileIconCache;
  class WindowSurface;
  struct DirectoryEntry;
} // namespace luciole

class OperationJob;
class PageContext;
class PropertiesWindow;

// Properties: one window per show(), so several items can be compared side by side.
class PropertiesController {
public:
  PropertiesController(PageContext& page, luciole::FileIconCache& icons);
  ~PropertiesController();
  PropertiesController(const PropertiesController&) = delete;
  PropertiesController& operator=(const PropertiesController&) = delete;

  // Opens a new window for `targets`. `captureCurrentLocation` reads the permissions of the first target's location
  // instead of its listed entry.
  void show(const std::vector<luciole::DirectoryEntry>& targets, bool captureCurrentLocation = false);
  // Owned permission job change: sync the window that owns it, or report a job whose window closed.
  void changed(OperationJob& job);
  // Operation progress tick: keep syncing while permissions are read or applied.
  void progressTick();
  // BrowserPage::syncBrowser() hook: Apply is enabled only while no file operation runs.
  void updateApply();
  // File icons finished loading: redraw the windows that show them.
  void iconsReady();
  // The page moved to another browser window.
  void setParent(luciole::WindowSurface& parent);
  // Closes every window, for a page that closes or whose browser window closed.
  void closeAll();

private:
  PropertiesWindow* owner(const OperationJob& job) const;
  // A job whose window closed: report its outcome once.
  void reportClosed(OperationJob& job);

  PageContext& m_page;
  luciole::FileIconCache& m_icons;
  std::vector<std::unique_ptr<PropertiesWindow>> m_windows;
};
