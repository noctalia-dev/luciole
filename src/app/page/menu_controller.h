#pragma once

#include "browser/action_registry.h"
#include "fs/location.h"
#include "fs/trash_availability.h"
#include "model/details_columns.h"
#include "render/scene/node.h"
#include "ui/controls/context_menu.h"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

namespace luciole {
  class PathCompletion;
  class PinService;
  class ViewPreferenceWriter;
  class TerminalLauncher;
  struct Bookmark;
} // namespace luciole

struct KeyboardEvent;
class CreateController;
class DetailsHeaderController;
class ProgramController;
class LocationBarController;
class PageContext;
class PropertiesController;
class RenameController;
class SubmenuController;
class TransferController;
class ScrollView;

// Context menus: the file, place, view, sort, selection, columns and breadcrumb menus and the file menu's submenus,
// and keyboard navigation of whichever context menu the popup shows.
class MenuController {
public:
  MenuController(
      PageContext& page, luciole::PinService& pins, luciole::ViewPreferenceWriter& preferences,
      TransferController& transferController, luciole::TerminalLauncher& terminal,
      luciole::PathCompletion& siblingCompletion, CreateController& creator, SubmenuController& submenus,
      RenameController& renamer, PropertiesController& properties, DetailsHeaderController& columns,
      LocationBarController& locationBar, ProgramController& programs
  );

  // Shows `entries` on the popup; activating an entry calls `action` with its id before closing the menu.
  void showContextMenu(
      std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor,
      bool alignLeft = false, float preferredWidth = 264.0F
  );
  void openFileMenu(std::optional<luciole::EntryId> id, float x, float y);
  void openPlaceMenu(const luciole::Bookmark& bookmark, float x, float y);
  // The menu of the separator line in front of Places row `before`.
  void openSeparatorMenu(std::size_t before, float x, float y);
  void openColumnsMenu(const LayoutRect& anchor, std::optional<luciole::DetailsColumn> column = std::nullopt);
  void openBreadcrumbMenu(const luciole::Location& target, LayoutRect anchor);
  void openMenu();
  void openSortMenu(std::optional<LayoutRect> anchor = std::nullopt, bool alignLeft = true);
  void openSelectionMenu();
  // Scrolls the highlighted row into view.
  void revealHighlight();
  // Keyboard hook in BrowserPage::keyboard(): handles every key while the popup is shown.
  void key(const KeyboardEvent& event);

  // The context menu on the popup; null while the popup shows a dialog or nothing.
  ContextMenuControl* menu() const { return m_menu; }
  // BrowserPage::openPopup(): the popup now shows `menu`, or a dialog or popover when null.
  void setMenu(ContextMenuControl* menu) { m_menu = menu; }
  // BrowserPage::closePopup(): forgets the menu widgets.
  void menuClosed();

private:
  ContextMenuControlEntry
  actionEntry(luciole::BrowserAction action, const luciole::ActionRequest& request, bool radio = false) const;
  // The file menu of the empty area, or of the request's targets.
  std::vector<ContextMenuControlEntry> folderMenuEntries(const luciole::ActionRequest& request) const;
  std::vector<ContextMenuControlEntry> itemMenuEntries(const luciole::ActionRequest& request) const;
  void showOpenInMenu(const luciole::ActionRequest& request);
  // Path and URI of the request's targets, or of its folder when `targets` is false.
  void showCopyLocationMenu(const luciole::ActionRequest& request, bool targets);
  void showActionMenu(
      std::initializer_list<std::optional<luciole::BrowserAction>> actions, bool radio = false, bool back = false,
      std::optional<LayoutRect> anchor = std::nullopt, bool alignLeft = true
  );

  PageContext& m_page;
  luciole::PinService& m_pins;
  luciole::ViewPreferenceWriter& m_preferences;
  TransferController& m_transferController;
  luciole::TerminalLauncher& m_terminal;
  luciole::PathCompletion& m_siblingCompletion;
  CreateController& m_creator;
  SubmenuController& m_submenus;
  RenameController& m_renamer;
  PropertiesController& m_properties;
  DetailsHeaderController& m_columns;
  LocationBarController& m_locationBar;
  ProgramController& m_programs;
  ContextMenuControl* m_menu = nullptr;
  ScrollView* m_scroll = nullptr;
  luciole::TrashAvailability m_trashAvailability;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
