#pragma once

#include "app/operation_hub.h"
#include "app/page/about_controller.h"
#include "app/page/active_popup.h"
#include "app/page/application_chooser_controller.h"
#include "app/page/archive_controller.h"
#include "app/page/batch_rename_controller.h"
#include "app/page/create_controller.h"
#include "app/page/details_header_controller.h"
#include "app/page/dialog_dropdown.h"
#include "app/page/drag_drop_controller.h"
#include "app/page/file_keyboard_controller.h"
#include "app/page/folder_icon_controller.h"
#include "app/page/folder_view_controller.h"
#include "app/page/location_bar_controller.h"
#include "app/page/menu_controller.h"
#include "app/page/mount_controller.h"
#include "app/page/operations_controller.h"
#include "app/page/page_context.h"
#include "app/page/preview_controller.h"
#include "app/page/program_controller.h"
#include "app/page/properties_controller.h"
#include "app/page/rename_controller.h"
#include "app/page/settings_controller.h"
#include "app/page/shortcuts_controller.h"
#include "app/page/sidebar_controller.h"
#include "app/page/status_bar_controller.h"
#include "app/page/submenu_controller.h"
#include "app/page/transfer_controller.h"
#include "app/page/trash_controller.h"
#include "app/window_move_gesture.h"
#include "app/workspace_command.h"
#include "app/workspace_layout.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/folder_views.h"
#include "browser/path_completion.h"
#include "browser/selection_tools.h"
#include "browser/tab_workspace.h"
#include "clipboard/text_clipboard.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/timer_manager.h"
#include "model/details_columns.h"
#include "operations/template_catalog.h"
#include "platform/desktop_integration.h"
#include "platform/file_launcher.h"
#include "platform/terminal_launcher.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/preview_service.h"
#include "ui/browser/file_icon.h"
#include "ui/browser/file_tooltip.h"
#include "ui/browser/file_view.h"
#include "ui/browser/places_sidebar.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/dialogs/name_dialog.h"
#include "ui/popup_chrome.h"
#include "wayland/popup_surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

class BrowserPage final : public PageContext, public OperationHub::Owner {
public:
  using Action = luciole::BrowserAction;

  // `id` is the page's TabId in the workspace; the page shows the jobs `hub` routes to it. `dispatcher` is the input
  // dispatcher of the window that owns `surface`.
  BrowserPage(
      WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
      const luciole::BrowserSettings& settings, luciole::WaylandTextClipboard& clipboard,
      luciole::WindowSurface& surface, InputDispatcher& dispatcher, AnimationManager& animations,
      luciole::PinService& pins, luciole::DriveMonitor& drives, luciole::FileIconCache& icons,
      luciole::ThumbnailService& thumbnails, luciole::ViewPreferenceWriter& preferences,
      luciole::FolderViewService* folderViews, OperationHub& hub, luciole::TabId id
  );

  luciole::BrowserController& browser() override;
  const luciole::BrowserController& browser() const;
  void focusMiller(std::size_t index, bool reveal = true);
  // A FileChooser portal page: `open` receives every Open but a single folder, which still navigates; `cancel`
  // receives an Escape nothing else consumed. Set before the scene is built.
  struct ChooserHooks {
    std::function<void(const std::vector<luciole::DirectoryEntry>&)> open;
    std::function<void()> cancel;
  };
  void setChooser(ChooserHooks hooks);
  bool choosing() const override;

  ~BrowserPage();

  void iconThemeChanged();
  // The system font changed: rebuild the file views and lay out the window and its popup again.
  void fontChanged();

  bool initialize(const std::optional<std::string>& initialLocation);

  // Select `items` once the current folder finishes loading; optionally open their Properties.
  void revealItems(std::vector<luciole::Location> items, bool properties);

  // True while page-local work that outlives the window runs: launching, saving places or preferences, a paste.
  bool busy() const;
  void shutdown();
  // OperationHub::Owner: hands the job to the controller of its kind.
  void jobChanged(OperationJob& job) override;
  void activityChanged() override { m_operations.markDirty(); }
  void setOperationRequest(std::function<void(luciole::JobId, bool)> callback) {
    m_operationRequest = std::move(callback);
  }
  void reviewOperation(luciole::JobId id, bool retry);
  void transfersFinished(const std::vector<luciole::TransferResult>& results) override;
  void leaveDrive(const luciole::Location& root) override;
  bool driveReadersBusy(const luciole::Location& root) const override;
  // The shared thumbnail service changed.
  void thumbnailsChanged();

  luciole::BrowserSettings options() const;
  std::optional<luciole::Location> location() const;
  std::string title() const;
  std::optional<luciole::FileTooltipTarget> fileTooltipAt(float x, float y) const;
  void preferencesChanged(const std::string& error);
  // Per-folder views were loaded, switched on or off, or changed in any page: apply the current folder's again.
  void folderViewsChanged() { m_folderView.apply(); }
  void placesChanged();
  void driveUsageChanged();
  void clipboardChanged();
  void clearExternalDrop();
  const std::vector<luciole::Bookmark>* draggedFolders() const;
  luciole::FileDropTarget externalDrop(float x, float y, const std::vector<luciole::Bookmark>* pins);
  luciole::FileDropTarget externalDestination(const luciole::Location& destination, LayoutRect rect);
  void iconsReady();
  void reusePlacesFrom(BrowserPage& previous);
  void workspaceError(const std::string& message);
  bool owns(const Node* node) const;
  // The window dispatcher's focus moved to `focused`.
  void focusChanged(InputArea* focused);
  void windowFocusChanged(bool entered);
  bool completeLocationOnTab(std::uint32_t modifiers);
  bool sidebarKey(const KeyboardEvent& event);
  bool sidebarOverlays(float x, float y) const;
  void prepareSidebar(float width);
  void arrangeSidebar(Renderer& renderer, float top, float height);
  void stampFocusKeys(Node& node);
  void duplicateFrom(BrowserPage& source);
  void open(const luciole::Location& location);
  void setWorkspaceRequest(std::function<void(WorkspaceCommand, std::optional<luciole::Location>)> callback);
  void configurationChanged();
  void setHasOther(std::function<bool()> callback);
  void setPopupAvailable(std::function<bool()> callback);
  void setForeignDrop(std::function<std::optional<std::pair<luciole::Location, LayoutRect>>(float, float)> callback);
  bool popupOwns(wl_surface* surface) const;
  bool popupCapturesAllKeys() const;
  bool popup() const;
  bool dimsWindow() const;
  bool overlay() const override;
  bool previewVisible() const;
  int previewWidth() const;
  float previewAreaWidth() const;
  void savePreviewWidth(int width);
  void setPreviewBounds(std::optional<LayoutRect> bounds);
  void layoutNavigation(Renderer& renderer);
  bool dragging() const;
  bool contains(float x, float y) const;
  luciole::WindowSurface* boundSurface() const;
  // Moves the page to another window: its surface and that window's input dispatcher.
  void rebind(luciole::WindowSurface& surface, InputDispatcher& dispatcher);
  // `roundCorner`: this pane starts the content plane beside the sidebar and rounds its top-left corner.
  void prepare(LayoutRect bounds, bool active, bool roundCorner);
  void deactivate();
  void hide();
  void mount(Node& host);
  void unmount();
  Node* root() const;
  std::unique_ptr<Node> takeToolbar();
  void returnToolbar(std::unique_ptr<Node> node);
  void takeSidebar(Node& host);
  void returnSidebar();
  Node* sidebar() const;
  void ensureScene();

  float sidebarWidth() const;
  float sidebarControlWidth() const;
  void pumpJobs();
  void workspaceMenu(std::vector<ContextMenuControlEntry> entries, std::function<void(int)> callback, float x, float y);
  void transferTo(const luciole::Location& destination, bool copy);
  std::optional<std::pair<luciole::Location, LayoutRect>> foreignTarget(float x, float y) const;

  void outputChanged(wl_output* output);

  void pointer(const PointerEvent& event);

  void keyboard(const KeyboardEvent& event);

private:
  // The preview preference of this window kind: file picker windows remember theirs separately.
  bool previewEnabled() const;
  void togglePreview();
  // Shared popup setup after closePopup(): `scene` goes on a new child surface configured by `config`;
  // `content` is what ActivePopup::root() returns. Menus pass their root as both.
  void openPopup(
      std::unique_ptr<Flex> scene, Flex* content, ContextMenuControl* menu, PopupSurfaceConfig config,
      PopupDismiss dismiss
  );
  // The dialog's content size and shadow geometry for the current window: content height capped by the window.
  popup_chrome::Geometry dialogGeometry(Flex& content);
  // Where the dialog's surface goes: centered on open and on window resizes, else the top edge stays put.
  std::pair<int, int> dialogPosition(const popup_chrome::Geometry& chrome);
  // Resizes, positions and arranges the dialog frame on its surface for the current window.
  void layoutDialogFrame();
  luciole::ActionContext actionContext() const;
  luciole::ActionRequest actionRequest(bool withTargets) const override;
  luciole::ActionRequest requestFor(const std::vector<luciole::DirectoryEntry>& entries) const override;
  luciole::ActionState actionState(Action action, const luciole::ActionRequest& request) const override;
  void dispatchAction(Action action) override;
  void dispatchAction(Action action, const luciole::ActionRequest& request) override;
  void
  runAction(Action action, const luciole::ActionRequest& request, const std::vector<luciole::DirectoryEntry>& targets);
  void copyFiles(Action action, const std::vector<luciole::DirectoryEntry>& targets);
  void
  copyPaths(Action action, const luciole::ActionRequest& request, const std::vector<luciole::DirectoryEntry>& targets);
  void openTerminal(const luciole::Location& target);
  void sortAction(Action action);
  void openTargets(const std::vector<luciole::DirectoryEntry>& targets);
  void pinTargets(const std::vector<luciole::DirectoryEntry>& targets);
  void unpinTargets(const std::vector<luciole::DirectoryEntry>& targets);
  void openPatternSelection(const luciole::ActionRequest& request);

  void actionError(std::string message) override;
  void setActionMessage(std::string message) override;
  void revealOnReload(std::vector<luciole::Location> targets) override;
  void restorePlaceFocus(luciole::Location location) override;

  bool targetsCurrent(const std::vector<luciole::DirectoryEntry>& targets) override;
  bool requestCurrent(const luciole::ActionRequest& request, luciole::ActionScope scope) const override;
  bool operationsBusy() const override;
  std::vector<std::string> runningJobTitles() const override;
  PageJobs& jobs() override;
  bool visible() const override;
  bool active() const override;
  bool thumbnailsPaused() const override;
  bool popupAvailable() override;
  void prepareOverlay() override;
  Node& sceneRoot() override;
  const luciole::BrowserSettings& settings() const override;
  bool hasOtherPane() const override;
  void requestWorkspace(
      WorkspaceCommand command, std::optional<luciole::Location> location, std::optional<luciole::ActionRequest> pending
  ) override;
  LayoutRect menuAnchor() const override;
  Node& upButton() override;
  void requestSync() override;
  float pointerX() const override;
  float pointerY() const override;
  VirtualGridView* fileList() override;
  Flex& filePane() override;
  MillerView& millerView() override;
  luciole::ViewMode viewMode() const override;
  float gridIconSize() const override;
  float rowIconSize() const override;
  void captureViewAnchor() override;
  void resetFileView() override;

  void operationProgressTick();

  void showContaining(const luciole::Location& location);
  // Opens the folder holding `link`'s target and selects it, once a worker has read the link.
  void showLinkTarget(const luciole::Location& link);

  void applySettings(const luciole::BrowserSettings& settings, bool force) override;
  void setSearchRecursive(bool recursive) override;

  void
  showDialog(std::unique_ptr<Flex> root, DialogWidth width, PopupDismiss dismiss = PopupDismiss::Explicit) override;
  float dialogWidth(DialogWidth width) const override;

  void cancelDrag() override;

  void setViewMode(luciole::ViewMode mode);
  void showViewMode(luciole::ViewMode mode) override;
  void applyViewMode();
  float gridHeight() const;
  // Sets the default sort of every controller; folders with their own keep it.
  void applySort(luciole::SortOptions options);
  void sortChanged() override;
  void changeSort(luciole::SortOptions options);
  void changeGridZoom(int requested);
  // List and Columns rows; 0 keeps the view's default size.
  void changeRowIconSize(int size);
  // The current view's step in its icon sizes.
  int zoomStep() const;
  void changeZoomStep(int requested);
  void resetZoom();
  bool zoomFromKey(const KeyboardEvent& event);
  bool zoomFromScroll(const PointerEvent& event);
  void toggleHidden();

  // Input: pointer() and keyboard() run these hooks and the controllers' hooks in a fixed order.
  bool openInNewTab(float x, float y);
  void pointerOverlay(InputDispatcher& input, const PointerEvent& event, float x, float y);
  bool millerPointer(const PointerEvent& event, float x, float y);
  void dispatchPointer(InputDispatcher& input, const PointerEvent& event, float x, float y);
  std::uint32_t resizeEdges(float x, float y) const;
  bool escapeGesture(const KeyboardEvent& event);
  // Dispatches the first of `actions` whose table shortcut matches the key.
  bool dispatchShortcut(const KeyboardEvent& event, std::initializer_list<Action> actions);
  bool shortcutKey(const KeyboardEvent& event);
  bool windowKey(const KeyboardEvent& event);
  bool navigationKey(const KeyboardEvent& event, bool editing);
  bool millerKey(const KeyboardEvent& event);
  // Parent folder: the parent Miller column when there is one, otherwise the parent folder, selecting the folder
  // just left once it loads.
  void leaveFolder();

  // Frame: prepareFrame() syncs the page and its controllers, then layoutFrame() lays them out.
  void syncBrowser();
  void revealPendingTargets();
  void prepareFrame(bool, bool layout);
  void layoutFrame(Renderer& renderer);

  // Scene.
  void buildScene();
  std::unique_ptr<Flex> buildTitlebar();
  std::unique_ptr<Flex> buildToolbar();
  std::unique_ptr<Flex> buildFilePane();

  void dismissPopup() override;
  void closePopup() override;

  void showPopup(
      std::unique_ptr<Flex> root, ContextMenuControl* menu, LayoutRect anchor, float width, float height,
      bool alignLeft = false
  ) override;
  void showContextMenu(
      std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor
  ) override;
  PopupSurface* popupSurface() override;
  InputDispatcher& popupDispatcher() override;
  ActivePopup& activePopup() override;
  DialogDropdown& dialogDropdown() override;
  ContextMenuControl* contextMenu() override;
  std::uint32_t popupSerial() override;
  float popupMaxHeight() const override;
  luciole::WaylandTextClipboard& clipboard() override;
  luciole::WindowSurface& surface() override;
  InputDispatcher& dispatcher() override;
  void focusFileList() override;

  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  TextInputService& m_textInput;
  AnimationManager& m_animations;
  luciole::WindowSurface* m_surface;
  InputDispatcher* m_dispatcher;
  luciole::BrowserController m_browser;
  luciole::MillerColumns m_miller;
  MillerView* m_millerView = nullptr;
  VirtualGridView* m_regularList = nullptr;
  bool m_millerFocusPending = false;
  bool m_restartMiller = false;
  const luciole::BrowserController* m_lastBrowser = nullptr;
  luciole::ActivationMode m_activationMode = luciole::ActivationMode::DoubleClick;
  NameDialog* m_patternEditor = nullptr;
  std::string m_selectionPattern = "*";
  luciole::FileIconCache& m_icons;
  luciole::ThumbnailService& m_thumbnails;
  luciole::PreviewService m_preview{m_thumbnails};
  std::optional<luciole::Location> m_retiringPreview;
  bool m_thumbnailClearing = false;
  std::string m_thumbnailError;
  std::uint64_t m_thumbnailRevision = 0;
  std::uint64_t m_thumbnailContext = 0;
  luciole::FileViewAdapter m_listAdapter;
  luciole::FileViewAdapter m_gridAdapter;
  luciole::ViewPreferenceWriter& m_preferences;
  luciole::BrowserSettings m_savedSettings;
  // The open dialog's tier; unset while a menu or nothing is shown.
  std::optional<DialogWidth> m_dialogTier;
  RectNode* m_dialogShadow = nullptr;
  std::optional<std::pair<int, int>> m_dialogPosition;
  bool m_dialogResizePending = false;
  luciole::PinService& m_pins;
  luciole::FolderViewService* m_folderViews; // nullptr in file pickers, which keep their own sort and view mode.
  luciole::DriveMonitor& m_drives;
  luciole::FileLauncher m_launcher;
  std::vector<luciole::Location> m_revealTargets;
  bool m_revealProperties = false;
  float m_pointerX = 0;
  float m_pointerY = 0;
  std::uint32_t m_popupSerial = 0;
  Flex* m_filePane = nullptr;
  OperationHub& m_hub;
  PageJobs m_jobs;
  DialogDropdown m_dialogDropdown{*this, m_connection, m_renderer};
  TrashController m_trashController{*this};
  FolderViewController m_folderView{*this, m_folderViews, m_browser, m_miller};
  RenameController m_renameController{*this, m_pins};
  FolderIconController m_folderIconController{*this, m_icons};
  PropertiesController m_propertiesController{
      *this, m_icons, [this](const luciole::Location& folder, std::function<void()> onCancel) {
        m_folderIconController.open(folder, std::move(onCancel));
      }
  };
  BatchRenameController m_batchRename{*this};
  luciole::TemplateCatalog m_templates;
  SubmenuController m_submenus{*this, m_connection, m_renderer, m_animations};
  ProgramController m_programs{*this, m_submenus, m_connection};
  CreateController m_createController{*this, m_templates, m_submenus};
  MountController m_mountController{*this};
  ArchiveController m_archiveController{*this, m_launcher, m_icons, m_connection};
  TransferController m_transferController{*this, [this] { operationProgressTick(); }, m_createController};
  OperationsController m_operations{*this, m_hub, [this](luciole::JobId id, bool retry) {
                                      if (m_operationRequest) {
                                        m_operationRequest(id, retry);
                                      }
                                    }};
  MenuController m_menuController{
      *this,
      m_pins,
      m_preferences,
      m_transferController,
      m_terminal,
      m_siblingCompletion,
      m_createController,
      m_submenus,
      m_renameController,
      m_propertiesController,
      m_detailsHeader,
      m_locationBar,
      m_programs
  };
  SettingsController m_settingsController{*this, m_preferences, m_thumbnails, [this] { m_thumbnails.clearCache(); }};
  AboutController m_aboutController{*this, m_launcher};
  ShortcutsController m_shortcutsController{*this};
  ApplicationChooserController m_applicationChooser{*this, m_launcher, m_icons};
  PreviewController m_previewController{*this, m_preview, m_thumbnails, m_icons, m_renderer, m_applicationChooser};
  DetailsHeaderController m_detailsHeader{*this, m_preferences, m_listAdapter, m_gridAdapter, m_menuController};
  LocationBarController m_locationBar{*this, m_connection, m_fileKeys, m_menuController};
  SidebarController m_sidebar{*this,        m_pins,           m_drives,           m_preferences,
                              m_animations, m_menuController, m_renameController, m_propertiesController,
                              m_dragDrop};
  DragDropController m_dragDrop{*this,   m_connection, m_pins,    m_transferController, m_thumbnails,
                                m_icons, m_miller,     m_sidebar, m_locationBar};
  FileKeyboardController m_fileKeys{*this, m_textInput, m_miller, m_menuController, m_locationBar, m_statusBar};
  StatusBarController m_statusBar{*this, m_animations, m_pins, m_settingsController, m_fileKeys};
  std::size_t m_gridZoom = 1;
  int m_listIconSize = 0;
  int m_millerIconSize = 0;
  double m_zoomScroll = 0;
  luciole::ViewMode m_viewMode;
  luciole::WaylandTextClipboard& m_clipboard;
  luciole::TerminalLauncher m_terminal;
  luciole::PathCompletion m_siblingCompletion;
  std::optional<luciole::ViewMode> m_appliedViewMode;
  std::optional<luciole::EntryId> m_viewAnchor;
  bool m_sortScrollPending = false;
  std::uint64_t m_visitSerial = 0;
  std::optional<luciole::BrowseViewport> m_pendingHistoryViewport;
  Node* m_root = nullptr;
  std::unique_ptr<Node> m_detachedRoot;
  Flex* m_body = nullptr;
  Flex* m_paneHeader = nullptr;
  Label* m_paneTitle = nullptr;
  Flex* m_toolbar = nullptr;
  LayoutRect m_pageBounds{};
  std::optional<InputDispatcher::TabFocusSnapshot> m_stashedFocus;
  std::optional<luciole::BrowseViewport> m_inactiveViewport;
  std::function<void(WorkspaceCommand, std::optional<luciole::Location>)> m_workspaceRequest;
  bool m_visible = false, m_active = false;
  bool m_pendingProviderRefresh = false;
  luciole::TabId m_workspaceId;
  std::uint64_t m_nextFocusKey = 0;
  std::function<bool()> m_popupAvailable, m_hasOther;
  std::optional<luciole::ActionRequest> m_pendingWorkspaceRequest;
  std::function<void(luciole::JobId, bool)> m_operationRequest;
  std::optional<std::pair<luciole::JobId, bool>> m_pendingOperation;
  ChooserHooks m_chooser;

  Flex* m_titlebar = nullptr;
  WindowMoveGesture m_titlebarMove;
  Flex* m_paneContentFrame = nullptr;
  VirtualGridView* m_list = nullptr;
  Segmented* m_viewSwitch = nullptr;
  Button* m_menuButton = nullptr;
  Button* m_back = nullptr;
  Button* m_forward = nullptr;
  Button* m_up = nullptr;
  Button* m_newFolderButton = nullptr;
  Button* m_sortButton = nullptr;
  Button* m_emptyTrashButton = nullptr;
  Button* m_closePaneButton = nullptr;
  bool m_browserDirty = true;
  std::unique_ptr<PopupSurface> m_popupSurface;
  PopupDismiss m_popupDismiss = PopupDismiss::OutsideClick;
  ActivePopup m_activePopup;
  InputDispatcher m_popupDispatcher;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
