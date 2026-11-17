#pragma once

#include "app/file_chooser_session.h"
#include "app/file_manager_service.h"
#include "app/instance_service.h"
#include "app/operation_hub.h"
#include "app/page/browser_page.h"
#include "app/palette_watcher.h"
#include "app/window_move_gesture.h"
#include "browser/tab_workspace.h"
#include "config/state_store.h"
#include "thumbnails/thumbnail_service.h"

#include <csignal>
#include <map>
#include <memory>

// Set by SIGINT, SIGTERM and Ctrl+Q; every window closes and the main loop stops once it is non-zero.
extern volatile std::sig_atomic_t g_stopRequested;

class WorkspaceApplication {
  static constexpr std::size_t pageLimit = 64;
  struct Host {
    luciole::WindowId id;
    luciole::WindowSurface surface;
    std::unique_ptr<Node> root;
    // The root paints the one window background behind every area, so no seam or corner shows through.
    Flex* background = nullptr;
    double backgroundOpacity = -1.0;
    std::unique_ptr<Node> toolbar;
    Node* toolbarNode = nullptr;
    Flex* strip = nullptr;
    ScrollView* tabScroll = nullptr;
    Button* newTabButton = nullptr;
    Button* splitButton = nullptr;
    InputArea* divider = nullptr;
    InputArea* previewDivider = nullptr;
    Box* previewResizeLine = nullptr;
    Box* activePaneBar = nullptr;
    Box* dialogBackdrop = nullptr;
    // The window's only scene input: tab strip, chrome and every mounted page share it.
    InputDispatcher dispatcher;
    // Page that took the last pointer event outside the tab strip, 0 for the strip; a held button stays with it.
    luciole::TabId pointerPage = 0;
    TooltipPresenter* tooltip = nullptr;
    std::vector<std::pair<luciole::TabId, Button*>> buttons;
    std::map<luciole::TabId, std::pair<Button*, ColorRole>> closeButtons;
    luciole::TabId revealedTab = 0;
    std::uint32_t revealedTabWidth = 0;
    std::vector<std::pair<luciole::TabId, std::string>> labels;
    std::vector<luciole::TabId> mounted;
    luciole::TabId chromeOwner = 0;
    std::optional<luciole::TabId> pressedTab;
    float pressX = 0, pressY = 0;
    // A left press on empty tab strip, until it becomes a window move.
    WindowMoveGesture stripMove;
    bool tabDrag = false, resizing = false, closed = false;
    bool decorated = false;
    bool stripVisible = true;
    bool tabsHidden = false;
    float pointerX = 0, pointerY = 0;
    float resizeStart = 0.5F;
    std::optional<int> previewDragWidth;
    LayoutRect browsingBounds{};
    // A FileChooser window: one tab, no workspace commands, never saved. Its footer waits in `pendingFooter` until
    // the window's scene root exists. Last member: it goes before the scene its footer pointers point into.
    Node* chooserFooter = nullptr;
    std::unique_ptr<Node> pendingFooter;
    std::unique_ptr<FileChooserSession> chooser;
    Host(luciole::WindowId value, WaylandConnection& connection) : id(value), surface(connection) {}
  };

public:
  WorkspaceApplication(
      WaylandConnection& connection, RenderContext& renderer, TextInputService& input,
      luciole::WaylandTextClipboard& clipboard, luciole::BrowserSettings settings
  );
  SharedTextureCache& imageTextures();

  ~WorkspaceApplication();
  void start(std::optional<luciole::Location> initial);
  // Another launch of Luciole: one new window with a tab per location, focused with the launch's token.
  void open(const luciole::InstanceRequest& request);
  // FileManager1 request: one new window, one tab per folder; items are selected in their parent.
  void show(const luciole::FileManagerRequest& request);
  // FileChooser portal request: one chooser window that answers `call`.
  void choose(luciole::FileChooserRequest request, const std::shared_ptr<luciole::FileChooserCall>& call);
  bool hasWindows() const;
  void focus(wl_surface* surface, bool entered);
  void outputChanged(wl_output* output);
  void iconsChanged();
  bool running();
  void keyboard(const KeyboardEvent& event);
  void pointer(const PointerEvent& event);

private:
  void clearExternalDrops();
  static void dispatch(InputDispatcher& dispatcher, const PointerEvent& event);
  luciole::WindowId newWindow(std::unique_ptr<FileChooserSession> chooser = nullptr);
  luciole::TabId newTab(
      luciole::WindowId window, std::optional<luciole::Location> location, luciole::TabId duplicate = 0,
      bool activate = true
  );
  luciole::TabId
  newPane(luciole::WindowId window, std::optional<luciole::Location> location, luciole::TabId duplicate = 0);
  luciole::TabId createPage(
      luciole::WindowId window, luciole::TabId id, std::optional<luciole::Location> location, luciole::TabId duplicate,
      luciole::TabId previous
  );
  void select(luciole::WindowId window, luciole::TabId tab);
  void defer(
      luciole::WindowId window, luciole::TabId tab, WorkspaceCommand command,
      std::optional<luciole::Location> location = std::nullopt
  );
  void execute(
      luciole::WindowId window, luciole::TabId tab, WorkspaceCommand command,
      const std::optional<luciole::Location>& destination
  );
  void releaseChrome(Host& host);
  std::optional<luciole::SavedWindow> savedWindow(luciole::WindowId window) const;
  bool restoreLastWindow(luciole::WindowId window);
  bool restoreWindow(luciole::WindowId window, const luciole::SavedWindow& saved);
  void saveWindows(std::vector<luciole::SavedWindow> windows);
  // A pending move from an empty tab strip press; true when it consumed the event.
  bool stripMovePointer(Host& host, const PointerEvent& event);
  void tabMenu(Host& host, luciole::TabId tab, float x, float y);
  static Button::ButtonPalette closeTabPalette(ColorRole foreground);
  void rebuildStrip(Host& host, const luciole::TabWindow& window);
  void syncTooltip(Host& host);
  // The window dispatcher's focus moved: the pages hear it and the pane that holds it becomes the active one.
  void focusChanged(Host& host, InputArea* focused);
  void prepare(Host& host);
  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  TextInputService& m_input;
  luciole::WaylandTextClipboard& m_clipboard;
  luciole::BrowserSettings m_defaults;
  // Applies colors.toml before any surface is built, then follows its changes.
  luciole::PaletteWatcher m_palette;
  AnimationManager m_animations;
  luciole::ViewPreferenceWriter m_preferences;
  luciole::PinService m_pins;
  std::vector<luciole::SavedWindow> m_savedWindows;
  std::optional<std::vector<luciole::SavedWindow>> m_pendingWindows;
  luciole::DriveMonitor m_drives;
  bool m_drivesStarted = false;
  luciole::FileIconCache m_icons;
  // One thumbnail pipeline for every page.
  luciole::ThumbnailService m_thumbnails;
  luciole::TabWorkspace m_model;
  // File jobs outlive the page that started them; pages must go before the hub.
  OperationHub m_hub{m_model};
  std::map<luciole::TabId, std::unique_ptr<BrowserPage>> m_pages;
  std::map<luciole::WindowId, std::unique_ptr<Host>> m_hosts;
  luciole::WindowId m_focused = 0;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
