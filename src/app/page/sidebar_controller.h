#pragma once

#include "app/workspace_layout.h"
#include "browser/job_routing.h"
#include "fs/location.h"
#include "fs/trash_availability.h"
#include "model/adaptive_sidebar.h"
#include "model/directory_options.h"
#include "render/scene/input_dispatcher.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luciole {
  class DriveMonitor;
  class PinService;
  class PlacesSidebar;
  class ViewPreferenceWriter;
  struct Bookmark;
  struct BrowserSettings;
} // namespace luciole

class AnimationManager;
class Box;
class DragDropController;
class InputArea;
class MenuController;
class PageContext;
class PropertiesController;
class RenameController;
class Renderer;
class Flex;
class Button;
struct KeyboardEvent;

// The places sidebar: its pins and devices, showing and hiding it, resizing it by pointer or keyboard, the pin
// drop line, and handing the sidebar to the window, which lays it out beside the pages.
class SidebarController {
public:
  static constexpr float kHandleWidth = 6.0F;

  SidebarController(
      PageContext& page, luciole::PinService& pins, luciole::DriveMonitor& drives,
      luciole::ViewPreferenceWriter& preferences, AnimationManager& animations, MenuController& menus,
      RenameController& renamer, PropertiesController& properties, DragDropController& drags
  );
  ~SidebarController();

  // Takes the sidebar settings of a new page.
  void load(const luciole::BrowserSettings& settings);
  // Takes the sidebar settings that changed; `force` takes them all.
  void applySettings(const luciole::BrowserSettings& before, const luciole::BrowserSettings& settings, bool force);

  bool visible() const { return m_state.visible(); }
  void setVisible(bool visible, bool persist = true);
  void toggle();
  void dismissDrawer();
  bool drawerOpen() const { return m_state.compact() && m_state.visible(); }
  bool drawerContains(float x, float y) const;
  bool key(const KeyboardEvent& event);
  void prepareWidth(float width);
  void arrange(Renderer& renderer, float top, float height);
  std::unique_ptr<Button> buildToggle();
  // Width the window reserves for the sidebar, and the width of the sidebar controls in it.
  float width() const;
  float controlWidth() const;
  // True while the resize handle is dragged.
  bool resizing() const { return m_resizingSidebar; }
  // False until the scene is built.
  bool hasPlaces() const { return m_places != nullptr; }
  luciole::PlacesSidebar& places() { return *m_places; }
  const luciole::PlacesSidebar& places() const { return *m_places; }
  // The sidebar and its resize handle, detached from the page.
  Node* bundle() const { return m_sidebarBundle; }
  void take(Node& host);
  void giveBack();
  // Swaps in the places list of `previous`, which keeps its loaded state, and gives it this page's list.
  void reuseFrom(SidebarController& previous);
  void configureCallbacks();
  // Pins or devices changed: rebuild the list on the next layout.
  void markDirty() { m_placesDirty = true; }
  // Drive usage changed: updates the usage bars in place.
  void updateDriveUsage();
  // A Trash, Restore or Empty Trash job settled: recounts the trash once per job.
  void refreshTrashAvailabilityAfterJob(luciole::JobId jobId);
  // Focuses the entry for `location` once no popup or overlay is open.
  void focusPlace(luciole::Location location) { m_restorePlaceFocus = std::move(location); }
  // Pins `bookmarks` before the pin at `before`, or reports why pins cannot change.
  void pinFolders(std::vector<luciole::Bookmark> bookmarks, std::size_t before);

  // Scene: the places list and the resize handle, which the page places beside the file pane and detaches once
  // the page root exists, and the pin drop line for the drag overlay.
  std::unique_ptr<Node> buildPlaces();
  std::unique_ptr<Node> buildResizeHandle();
  void detach();
  std::unique_ptr<Box> buildDropLine();
  void hideDropLine();
  void showDropLine(Renderer& renderer, std::size_t slot);

  // BrowserPage::syncBrowser(): highlights the current folder.
  void sync(const std::optional<luciole::Location>& location);
  // Frame slot in BrowserPage::prepareFrame(): the resize line follows hover and focus.
  void onFrame();
  // Layout hooks in BrowserPage::prepareFrame(), in this order.
  // Rebuilds a changed list; returns the focus inside it to restore afterwards.
  std::optional<InputDispatcher::TabFocusSnapshot> invalidate();
  void layout();
  void layoutResizeLine(Renderer& renderer);
  // Restores the focus `invalidate()` captured, then a pending place focus unless `covered`.
  void restoreFocus(std::optional<InputDispatcher::TabFocusSnapshot> focus, bool covered);

private:
  static constexpr float kGapInset = (kWorkspaceInset - kHandleWidth) * 0.5F;

  int maximumWidth() const;
  void revealDrawer();
  void focusDrawer();
  void resize(int width, bool persist);
  void openDrive(const std::string& key);
  void removeDrive(const std::string& key);
  void openDriveMenu(const std::string& key, float x, float y);
  // Recounts the trash for the trash place's empty or full icon.
  void refreshTrashAvailability();

  PageContext& m_page;
  luciole::PinService& m_pins;
  luciole::DriveMonitor& m_drives;
  luciole::ViewPreferenceWriter& m_preferences;
  AnimationManager& m_animations;
  MenuController& m_menus;
  RenameController& m_renamer;
  PropertiesController& m_properties;
  DragDropController& m_drags;
  luciole::PlacesSidebar* m_places = nullptr;
  bool m_placesDirty = true;
  int m_sidebarWidth = luciole::kDefaultSidebarWidth;
  luciole::AdaptiveSidebar m_state;
  float m_reveal = 0;
  float m_scrimOpacity = 0;
  bool m_focusDrawer = false;
  Flex* m_sidebarPanel = nullptr;
  InputArea* m_drawerScrim = nullptr;
  Box* m_scrimFill = nullptr;
  Button* m_toggle = nullptr;
  bool m_sidebarTree = false;
  bool m_showDrives = true;
  bool m_showNetwork = true;
  bool m_removingDrive = false;
  int m_sidebarStartWidth = luciole::kDefaultSidebarWidth;
  int m_sidebarStartVisibleWidth = luciole::kDefaultSidebarWidth;
  // Expires with this controller so drive callbacks that finish later do nothing.
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  luciole::TrashAvailability m_trashAvailability;
  std::optional<luciole::Location> m_trashQueryLocation;
  std::unordered_set<luciole::JobId> m_trashJobsRefreshed;
  float m_sidebarPressX = 0;
  bool m_resizingSidebar = false;
  InputArea* m_sidebarResize = nullptr;
  Box* m_sidebarResizeLine = nullptr;
  Box* m_placeDropLine = nullptr;
  Node* m_sidebarBundle = nullptr;
  std::unique_ptr<Node> m_detachedSidebar;
  std::optional<luciole::Location> m_restorePlaceFocus;
};
