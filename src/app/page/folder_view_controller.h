#pragma once

#include "fs/location.h"
#include "model/directory_options.h"

class PageContext;

namespace luciole {
  class BrowserController;
  class FolderViewService;
  class MillerColumns;
} // namespace luciole

// Per-folder sort and view mode: while they are on, the page's sort and Grid/List changes go to the current folder
// instead of the defaults, and every controller of the page follows the folder's settings when they change. File
// pickers pass no service: they keep their own sort and view mode.
class FolderViewController {
public:
  FolderViewController(
      PageContext& page, luciole::FolderViewService* views, luciole::BrowserController& browser,
      luciole::MillerColumns& miller
  );

  bool enabled() const;
  // The current folder has its own sort or view mode.
  bool customized() const;
  bool sortCustomized() const;
  // Saves `sort` as the current folder's. False when per-folder views are off or no folder is open: the change is
  // then the default's.
  bool saveSort(const luciole::SortOptions& sort);
  // Saves `mode` as the current folder's. False when it changes the default instead: per-folder views are off, or
  // the change enters or leaves column view, which is window-wide.
  bool saveViewMode(luciole::ViewMode mode);
  void reset(const luciole::Location& folder);
  // The service loaded, was switched on or off, or changed in any page: apply the current folder's settings again.
  void apply();

private:
  std::optional<luciole::Location> target() const;

  PageContext& m_page;
  luciole::FolderViewService* m_views;
  luciole::BrowserController& m_browser;
  luciole::MillerColumns& m_miller;
};
