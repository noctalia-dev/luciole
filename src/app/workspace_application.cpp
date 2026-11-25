#include "app/workspace_application.h"

#include "app/file_manager_service.h"
#include "app/startup_trace.h"
#include "browser/location_presentation.h"
#include "browser/tab_workspace.h"
#include "clipboard/text_clipboard.h"
#include "config/settings.h"
#include "config/state_store.h"
#include "config/view_preference_writer.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fs/gio_ptr.h"
#include "platform/desktop_integration.h"
#include "render/animation/motion_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/builders.h"
#include "ui/controls/acl_editor.h"
#include "ui/controls/bulk_acl_editor.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/file_preview_view.h"
#include "ui/controls/input.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <format>
#include <linux/input-event-codes.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

volatile std::sig_atomic_t g_stopRequested = 0;

namespace {
  constexpr std::array<luciole::KeybindCommand, 9> kSelectTabCommands{
      luciole::KeybindCommand::SelectTab1, luciole::KeybindCommand::SelectTab2, luciole::KeybindCommand::SelectTab3,
      luciole::KeybindCommand::SelectTab4, luciole::KeybindCommand::SelectTab5, luciole::KeybindCommand::SelectTab6,
      luciole::KeybindCommand::SelectTab7, luciole::KeybindCommand::SelectTab8, luciole::KeybindCommand::SelectTab9,
  };
  // A tab being dragged fades in the strip while its copy follows the pointer.
  constexpr float kDraggedTabOpacity = 0.4F;
  constexpr float kTabGhostOpacity = 0.92F;
  // Below this a dropped tab opens in a new window rather than moving within the strip.
  constexpr float kTabDetachY = kTabStripHeight + kNavigationToolbarHeight - Style::spaceXs;
  // Width of the zone at each end of the tab bar that scrolls it while a tab is dragged, and the fastest step.
  constexpr float kTabEdgeZone = 48.0F;
  constexpr float kTabEdgeStep = 14.0F;
} // namespace

WorkspaceApplication::WorkspaceApplication(
    WaylandConnection& connection, RenderContext& renderer, TextInputService& input,
    luciole::WaylandTextClipboard& clipboard, luciole::BrowserSettings settings
)
    : m_connection(connection), m_renderer(renderer), m_input(input), m_clipboard(clipboard),
      m_defaults(std::move(settings)), m_palette(m_defaults.colorScheme),
      m_preferences(luciole::ConfigPaths::fromEnvironment()), m_pins(luciole::StateStore::pathFromEnvironment()),
      m_folderViews(luciole::FolderViewStore::pathFromEnvironment(), m_defaults.rememberFolderViews) {
  m_preferences.setChangedCallback([this](const std::string& error) {
    if (m_preferences.settings()) {
      m_defaults = m_preferences.settings()->browser;
      luciole::setActiveKeybinds(m_defaults.keybinds);
      // Strip tooltips embed shortcut labels; clear the cache so rebuildStrip regenerates them.
      for (auto& [id, host] : m_hosts) {
        (void)id;
        host->labels.clear();
      }
      // Live Style bindings update open widgets; controls built afterwards pick the values up directly.
      Style::setCornerRadiusScale(static_cast<float>(m_defaults.cornerRadiusScale));
      Style::setCloseButtonsEnabled(m_defaults.showCloseButtons);
      MotionService::instance().setEnabled(!m_defaults.reduceAnimations);
      m_palette.setScheme(m_defaults.colorScheme);
      m_folderViews.setEnabled(m_defaults.rememberFolderViews);
    }
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->preferencesChanged(error);
    }
    // Unchanged windows ignore the request; changed ones rebuild their strips when the compositor answers.
    for (auto& [id, host] : m_hosts) {
      (void)id;
      host->surface.setUseSSD(m_defaults.useSSD);
    }
  });
  m_clipboard.setChangedCallback([this] {
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->clipboardChanged();
    }
  });
  m_clipboard.setDropCallbacks(
      [this](wl_surface* surface, float x, float y) {
        clearExternalDrops();
        const std::vector<luciole::Bookmark>* pins = nullptr;
        for (const auto& [id, page] : m_pages) {
          (void)id;
          if (const auto* folders = page->draggedFolders()) {
            pins = folders;
          }
        }
        for (const auto& [windowId, host] : m_hosts) {
          if (host->closed || host->surface.wlSurface() != surface) {
            continue;
          }
          if (host->chooser) {
            return luciole::FileDropTarget{};
          }
          if (host->tooltip) {
            host->tooltip->dismiss();
          }
          const auto found = m_model.windows().find(windowId);
          if (found == m_model.windows().end()) {
            return luciole::FileDropTarget{};
          }
          for (const auto& [tab, button] : host->buttons) {
            if (host->tabScroll->containsScenePoint(x, y) && button->containsScenePoint(x, y)) {
              if (const auto layout = m_model.layout(tab)) {
                if (const auto destination = m_pages.at(layout->active)->location()) {
                  float bx = 0, by = 0;
                  Node::absolutePosition(button, bx, by);
                  return m_pages.at(found->second.active)
                      ->externalDestination(*destination, LayoutRect{bx, by, button->width(), button->height()});
                }
              }
            }
          }
          if (found->second.active && m_pages.at(found->second.active)->sidebarOverlays(x, y)) {
            return m_pages.at(found->second.active)->externalDrop(x, y, pins);
          }
          for (const auto pageId : host->mounted) {
            if (m_pages.at(pageId)->contains(x, y)) {
              return m_pages.at(pageId)->externalDrop(x, y, pins);
            }
          }
          if (found->second.active) {
            return m_pages.at(found->second.active)->externalDrop(x, y, pins);
          }
        }
        return luciole::FileDropTarget{};
      },
      [this] { clearExternalDrops(); }
  );
  m_preferences.startWatching();
  m_folderViews.setChangedCallback([this] {
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->folderViewsChanged();
    }
  });
  m_pins.setChangedCallback([this] {
    if (m_pendingWindows && m_pins.editable()) {
      auto windows = std::exchange(m_pendingWindows, std::nullopt);
      if (!m_pins.saveWindows(*windows)) {
        m_pendingWindows = std::move(*windows);
      }
    }
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->placesChanged();
    }
  });
  m_drives.setChangedCallback([this] {
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->placesChanged();
    }
  });
  m_drives.setUsageChangedCallback([this] {
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->driveUsageChanged();
    }
  });
  m_icons.setChangedCallback([this] {
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->iconsReady();
    }
  });
  updateIconColorScheme();
  m_paletteConnection = paletteChanged().connect([this] {
    if (updateIconColorScheme()) {
      iconsChanged();
    }
  });
  m_thumbnails.setOptions(m_defaults.thumbnails);
  m_thumbnails.setChangedCallback([this] {
    for (auto& [id, page] : m_pages) {
      (void)id;
      page->thumbnailsChanged();
    }
  });
  luciole::StateStore state(luciole::StateStore::pathFromEnvironment());
  if (state.load()) {
    m_savedWindows = state.state().windows;
  }
  m_pins.reload();
}

SharedTextureCache& WorkspaceApplication::imageTextures() { return m_icons.textures(); }

WorkspaceApplication::~WorkspaceApplication() {
  m_lifetime.reset();
  m_clipboard.setChangedCallback({});
  m_clipboard.setDropCallbacks({}, {});
  m_preferences.setChangedCallback({});
  m_pins.setChangedCallback({});
  m_drives.setChangedCallback({});
  m_drives.setUsageChangedCallback({});
  m_icons.setChangedCallback({});
  m_thumbnails.setChangedCallback({});
  for (auto& [id, host] : m_hosts) {
    (void)id;
    releaseChrome(*host);
    for (auto tab : host->mounted) {
      m_pages.at(tab)->unmount();
    }
    host->mounted.clear();
    host->surface.setSceneRoot(nullptr);
    host->root.reset();
  }
  m_pages.clear();
}

void WorkspaceApplication::start(std::optional<luciole::Location> initial) {
  const auto id = newWindow();
  if (!id) {
    throw std::runtime_error("Cannot create a Luciole window.");
  }
  if (!initial && restoreLastWindow(id)) {
    return;
  }
  newTab(id, std::move(initial));
}

void WorkspaceApplication::open(const luciole::InstanceRequest& request) {
  const auto existing = m_defaults.singleWindow ? browsingWindow() : luciole::WindowId{0};
  if (existing && request.locations.empty()) {
    // A bare launch only raises the window.
    auto& host = *m_hosts.at(existing);
    select(existing, m_model.windows().at(existing).selected);
    m_connection.extension<luciole::DesktopIntegration>().activate(request.activationToken, host.surface.wlSurface());
    return;
  }
  const auto window = existing ? existing : newWindow();
  if (!window) {
    throw std::runtime_error("Cannot create a Luciole window.");
  }
  auto& host = *m_hosts.at(window);
  luciole::TabId first = 0;
  if (request.locations.empty()) {
    if (restoreLastWindow(window)) {
      first = m_model.windows().at(window).selected;
    } else {
      first = newTab(window, std::nullopt);
    }
  }
  for (const auto& location : request.locations) {
    const auto tab = newTab(window, location);
    if (!tab) {
      break;
    }
    first = first ? first : tab;
  }
  if (!first) {
    if (!existing) {
      host.surface.close();
    }
    throw std::runtime_error(
        existing ? "Close a tab before opening another." : "Close a tab before opening another window."
    );
  }
  select(window, first);
  m_connection.extension<luciole::DesktopIntegration>().activate(request.activationToken, host.surface.wlSurface());
}

void WorkspaceApplication::show(const luciole::FileManagerRequest& request) {
  std::vector<std::pair<luciole::Location, std::vector<luciole::Location>>> groups;
  for (const auto& location : request.locations) {
    auto parent = request.kind == luciole::FileManagerRequest::Kind::Folders ? std::nullopt : location.parent();
    if (!parent) {
      groups.emplace_back(location, std::vector<luciole::Location>{});
      continue;
    }
    auto folder = std::move(*parent);
    const auto found = std::ranges::find(groups, folder, [](const auto& group) { return group.first; });
    if (found != groups.end() && !found->second.empty()) {
      found->second.push_back(location);
    } else {
      groups.emplace_back(std::move(folder), std::vector{location});
    }
  }
  const auto existing = m_defaults.singleWindow ? browsingWindow() : luciole::WindowId{0};
  const auto window = existing ? existing : newWindow();
  if (!window) {
    throw std::runtime_error("Cannot create a Luciole window.");
  }
  luciole::TabId first = 0;
  for (auto& [folder, items] : groups) {
    const auto tab = newTab(window, folder);
    if (!tab) {
      break;
    }
    first = first ? first : tab;
    if (!items.empty()) {
      m_pages.at(tab)->revealItems(std::move(items), request.kind == luciole::FileManagerRequest::Kind::ItemProperties);
    }
  }
  if (first) {
    select(window, first);
    if (existing) {
      m_connection.activateSurface(m_hosts.at(window)->surface.wlSurface());
    }
  }
}

void WorkspaceApplication::choose(
    luciole::FileChooserRequest request, const std::shared_ptr<luciole::FileChooserCall>& call
) {
  auto folder = request.currentFolder;
  // The application's suggestion wins; the small private state file is read here like at startup.
  if (!folder && m_defaults.chooserStartFolder == luciole::ChooserStartFolder::Last) {
    luciole::StateStore state(luciole::StateStore::pathFromEnvironment());
    if (state.load()) {
      folder = state.state().chooserFolder;
    }
  }
  const auto window =
      newWindow(std::make_unique<FileChooserSession>(std::move(request), call, m_connection, m_renderer));
  if (!window) {
    call->fail();
    throw std::runtime_error("Cannot create a Luciole window.");
  }
  auto& host = *m_hosts.at(window);
  const auto tab = newTab(window, std::move(folder));
  if (!tab) {
    call->fail();
    host.surface.close();
    throw std::runtime_error("Too many Luciole pages are open.");
  }
  auto footer =
      host.chooser->attach(*m_pages.at(tab), host.surface, host.dispatcher, [&host] { host.surface.close(); });
  footer->setParticipatesInLayout(false);
  footer->setZIndex(40);
  host.chooserFooter = footer.get();
  host.pendingFooter = std::move(footer);
  select(window, tab);
}

bool WorkspaceApplication::hasWindows() const {
  return std::ranges::any_of(m_hosts, [](const auto& item) { return item.second->surface.isRunning(); });
}

void WorkspaceApplication::focus(wl_surface* surface, bool entered) {
  for (const auto& [id, host] : m_hosts) {
    if (host->surface.wlSurface() != surface) {
      continue;
    }
    const auto window = m_model.windows().find(id);
    auto* page =
        window != m_model.windows().end() && window->second.active ? m_pages.at(window->second.active).get() : nullptr;
    if (page) {
      page->windowFocusChanged(entered);
    }
    if (!entered) {
      if (host->tooltip) {
        host->tooltip->dismiss();
      }
      host->surface.requestUpdate();
      return;
    }
    m_focused = id;
    if (!page || !page->popup()) {
      host->dispatcher.setTextInputContext(host->surface.wlSurface(), &m_input);
    }
  }
}

void WorkspaceApplication::outputChanged(wl_output* output) {
  for (auto& [id, host] : m_hosts) {
    (void)id;
    if (m_connection.outputForSurface(host->surface.wlSurface()) == output) {
      host->surface.onSurfaceOutputEnter(host->surface.wlSurface(), output);
    }
  }
  for (auto& [id, page] : m_pages) {
    (void)id;
    page->outputChanged(output);
  }
}

void WorkspaceApplication::iconsChanged() {
  updateIconColorScheme();
  m_icons.invalidate();
  for (auto& [id, page] : m_pages) {
    (void)id;
    page->iconThemeChanged();
  }
}

void WorkspaceApplication::fontChanged() {
  for (auto& [id, page] : m_pages) {
    (void)id;
    page->fontChanged();
  }
  for (auto& [id, host] : m_hosts) {
    (void)id;
    host->surface.requestLayout();
  }
}

bool WorkspaceApplication::updateIconColorScheme() {
  auto roots = IconResolver::colorSchemeThemeRoots();
  if (roots == m_iconColorSchemeRoots && (roots.empty() || palette == m_iconColorSchemePalette)) {
    return false;
  }
  m_icons.setColorScheme(roots, iconColorSchemeStylesheet(palette));
  m_iconColorSchemeRoots = std::move(roots);
  m_iconColorSchemePalette = palette;
  return true;
}

bool WorkspaceApplication::running() {
  bool live = false;
  for (auto& [id, host] : m_hosts) {
    if (g_stopRequested && host->surface.isRunning()) {
      host->surface.close();
    }
    if (!host->surface.isRunning() && !host->closed) {
      host->closed = true;
      if (host->tooltip) {
        host->tooltip->dismiss();
      }
      // A chooser closed without an answer cancels its request.
      if (host->chooser) {
        host->chooser->windowClosed();
      }
      // Leave the workspace before the pages close their dialogs, so a job that applies changes keeps running.
      if (auto found = m_model.windows().find(id); found != m_model.windows().end()) {
        // A chooser window never replaces the saved browsing session.
        if (!host->chooser) {
          if (m_defaults.restoreLastWindow) {
            if (auto saved = savedWindow(id)) {
              saveWindows({std::move(*saved)});
            }
          } else {
            saveWindows({});
          }
        }
        const auto tabs = found->second.tabs;
        for (auto tab : tabs) {
          m_model.close(tab);
        }
        m_model.removeWindow(id);
      }
      releaseChrome(*host);
      for (auto tab : host->mounted) {
        m_pages.at(tab)->hide();
        m_pages.at(tab)->unmount();
      }
      host->mounted.clear();
    }
    live |= host->surface.isRunning();
  }
  // Without a window every job is cancelled, as closing the last window always did; the application exits once
  // they stop.
  if (!live) {
    m_hub.shutdown();
  }
  m_hub.update(m_focused);
  bool busy = m_hub.busy() || m_thumbnails.clearing() || m_pins.busy();
  for (auto& [id, page] : m_pages) {
    if (!live) {
      page->shutdown();
    } else if (!m_model.owner(id)) {
      for (auto& [window, host] : m_hosts) {
        if (host->surface.isRunning()) {
          (void)window;
          if (page->boundSurface() != &host->surface) {
            page->rebind(host->surface, host->dispatcher);
          }
          break;
        }
      }
    }
    bool visible = false;
    for (const auto& [window, host] : m_hosts) {
      (void)window;
      visible |= std::ranges::contains(host->mounted, id);
    }
    if (live && !visible) {
      page->pumpJobs();
    }
    busy |= page->busy();
  }
  // Closed pages go now: their jobs belong to the hub, which already handed them to another page.
  std::vector<luciole::TabId> retired;
  for (auto& [id, page] : m_pages) {
    const auto tab = m_model.tabFor(id);
    const bool referenced = tab
        && std::ranges::contains(m_model.pages(*tab), id)
        && (m_model.owner(id) || std::ranges::contains(m_model.closed(), *tab));
    const bool mounted = std::ranges::any_of(m_hosts, [id](const auto& item) {
      return std::ranges::contains(item.second->mounted, id);
    });
    if (!referenced && !mounted) {
      page->shutdown();
      retired.push_back(id);
    }
  }
  for (auto id : retired) {
    m_pages.erase(id);
  }
  if (live) {
    std::vector<luciole::WindowId> retiredHosts;
    for (const auto& [id, host] : m_hosts) {
      if (host->closed && std::ranges::none_of(m_pages, [&](const auto& item) {
            return item.second->boundSurface() == &host->surface;
          })) {
        retiredHosts.push_back(id);
      }
    }
    for (auto id : retiredHosts) {
      m_hosts.erase(id);
    }
  }
  return live || busy;
}

void WorkspaceApplication::keyboard(const KeyboardEvent& event) {
  if (event.pressed) {
    for (const auto& [id, host] : m_hosts) {
      (void)id;
      if (host->tooltip) {
        host->tooltip->dismiss();
      }
      host->surface.requestUpdate();
    }
  }
  const BrowserPage* focusedPage = nullptr;
  if (const auto focusedHost = m_hosts.find(m_focused); focusedHost != m_hosts.end() && !focusedHost->second->closed) {
    const auto record = m_model.windows().find(focusedHost->second->id);
    if (record != m_model.windows().end()) {
      const auto pageIt = m_pages.find(record->second.active);
      if (pageIt != m_pages.end()) {
        focusedPage = pageIt->second.get();
      }
    }
  }
  const bool capturingPopup = focusedPage != nullptr && focusedPage->popupCapturesAllKeys();
  // Quit is handled before popup routing, unless a popup captures every key.
  if (event.pressed && !event.preedit && !capturingPopup) {
    if (const auto focused = m_hosts.find(m_focused); focused != m_hosts.end()
        && !focused->second->closed
        && !focused->second->chooser
        && luciole::keybindMatches(luciole::KeybindCommand::Quit, event.sym, event.modifiers)) {
      g_stopRequested = 1;
      focused->second->surface.requestUpdate();
      return;
    }
  }
  if (const auto focused = m_hosts.find(m_focused);
      focused != m_hosts.end() && focused->second->chooser && focused->second->chooser->keyboard(event)) {
    return;
  }
  for (auto& [id, page] : m_pages) {
    if (page->popupOwns(m_connection.lastKeyboardSurface())) {
      (void)id;
      page->keyboard(event);
      return;
    }
  }
  auto hostIt = m_hosts.find(m_focused);
  if (hostIt == m_hosts.end() || hostIt->second->closed) {
    return;
  }
  auto& host = *hostIt->second;
  const auto record = m_model.windows().find(host.id);
  if (record == m_model.windows().end() || !record->second.active) {
    return;
  }
  auto& page = *m_pages.at(record->second.active);
  if (page.popup()) {
    page.keyboard(event);
    return;
  }
  if (event.pressed && !event.preedit) {
    if (!host.chooser && luciole::keybindMatches(luciole::KeybindCommand::CloseWindow, event.sym, event.modifiers)) {
      host.surface.close();
      return;
    }
    std::optional<WorkspaceCommand> command;
    if (luciole::keybindMatches(luciole::KeybindCommand::ClosePane, event.sym, event.modifiers)) {
      command = WorkspaceCommand::ClosePane;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::NewTab, event.sym, event.modifiers)) {
      command = WorkspaceCommand::NewTab;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::ReopenTab, event.sym, event.modifiers)) {
      command = WorkspaceCommand::Reopen;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::CloseTab, event.sym, event.modifiers)) {
      command = WorkspaceCommand::Close;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::NewWindow, event.sym, event.modifiers)) {
      command = WorkspaceCommand::NewWindow;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::SplitView, event.sym, event.modifiers)) {
      command = WorkspaceCommand::Split;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::SwitchArea, event.sym, event.modifiers)) {
      command = WorkspaceCommand::SwitchArea;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::DuplicateTab, event.sym, event.modifiers)) {
      command = WorkspaceCommand::Duplicate;
    }
    if (command) {
      defer(host.id, record->second.active, *command);
      return;
    }
    if (!page.overlay() && page.sidebarKey(event)) {
      return;
    }
    const bool nextTab = luciole::keybindMatches(luciole::KeybindCommand::NextTab, event.sym, event.modifiers);
    const bool previousTab = luciole::keybindMatches(luciole::KeybindCommand::PreviousTab, event.sym, event.modifiers);
    if (nextTab || previousTab) {
      const auto& tabs = record->second.tabs;
      if (tabs.empty()) {
        return;
      }
      const auto found = std::ranges::find(tabs, record->second.selected);
      const auto index = found == tabs.end() ? std::size_t{0} : static_cast<std::size_t>(found - tabs.begin());
      select(host.id, tabs[(index + tabs.size() + (previousTab ? -1 : 1)) % tabs.size()]);
      return;
    }
    for (std::size_t index = 0; index < kSelectTabCommands.size(); ++index) {
      if (!luciole::keybindMatches(kSelectTabCommands[index], event.sym, event.modifiers)) {
        continue;
      }
      const auto& tabs = record->second.tabs;
      const auto tab = index + 1 == kSelectTabCommands.size() ? tabs.size() - 1 : index;
      if (!tabs.empty() && tab < tabs.size()) {
        select(host.id, tabs[tab]);
      }
      return;
    }

    if (!page.overlay()
        && KeySymbol::isTab(event.sym)
        && (event.modifiers == 0 || event.modifiers == KeyMod::Shift)
        && page.completeLocationOnTab(event.modifiers)) {
      return;
    }
    const bool focusNext = luciole::keybindMatches(luciole::KeybindCommand::FocusNext, event.sym, event.modifiers);
    const bool focusPrevious =
        luciole::keybindMatches(luciole::KeybindCommand::FocusPrevious, event.sym, event.modifiers);
    if (!page.overlay() && (focusNext || focusPrevious)) {
      (void)host.dispatcher.cycleTabFocus(focusPrevious);
      host.surface.requestRedraw();
      return;
    }
    const bool cancel = luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers);
    if (cancel && host.stripMove.pending()) {
      host.stripMove.cancel();
      return;
    }
    if (cancel && host.pressedTab) {
      host.pressedTab.reset();
      host.tabDrag = false;
      host.surface.requestLayout();
      return;
    }
    if (cancel && host.resizing) {
      m_model.resize(host.id, host.resizeStart);
      host.resizing = false;
      host.surface.requestLayout();
      return;
    }
    if (cancel && host.previewDragWidth) {
      host.previewDragWidth.reset();
      host.surface.requestLayout();
      return;
    }
  }
  page.keyboard(event);
}

void WorkspaceApplication::pointer(const PointerEvent& event) {
  for (auto& [id, host] : m_hosts) {
    (void)id;
    if (host->chooser && host->chooser->pointer(event)) {
      return;
    }
  }
  for (auto& [id, page] : m_pages) {
    if (page->popupOwns(event.surface)) {
      (void)id;
      page->pointer(event);
      return;
    }
  }
  Host* host = nullptr;
  for (auto& [id, value] : m_hosts) {
    (void)id;
    if (value->surface.wlSurface() == event.surface) {
      host = value.get();
      break;
    }
  }
  if (!host || host->closed) {
    return;
  }
  if (host->tooltip) {
    if (!event.touch && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
      host->tooltip->resume();
    } else {
      host->tooltip->dismiss();
    }
    host->surface.requestUpdate();
  }
  host->pointerX = static_cast<float>(event.sx);
  host->pointerY = static_cast<float>(event.sy);
  if ((event.type == PointerEvent::Type::Button && event.pressed)
      || (event.type != PointerEvent::Type::Leave && m_connection.lastKeyboardSurface() == event.surface)) {
    m_focused = host->id;
  }
  const auto found = m_model.windows().find(host->id);
  if (found == m_model.windows().end() || !found->second.active) {
    return;
  }
  const auto& window = found->second;
  auto& active = *m_pages.at(window.active);
  // A button held on the tab strip stays with the strip.
  if (host->dispatcher.pointerCaptured() && !host->pointerPage) {
    dispatch(host->dispatcher, event);
    return;
  }
  for (auto& [tab, page] : m_pages) {
    (void)tab;
    if (page->popup() && page->boundSurface() == &host->surface) {
      page->pointer(event);
      return;
    }
  }
  for (auto tab : host->mounted) {
    if (m_pages.at(tab)->dragging()) {
      m_pages.at(tab)->pointer(event);
      return;
    }
  }
  // A button held in a page stays with that page, wherever the pointer moves.
  if (host->dispatcher.pointerCaptured() && std::ranges::contains(host->mounted, host->pointerPage)) {
    m_pages.at(host->pointerPage)->pointer(event);
    return;
  }
  const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  if (active.sidebarOverlays(x, y)) {
    host->pointerPage = window.active;
    active.pointer(event);
    return;
  }
  if (host->previewDragWidth) {
    if (event.type == PointerEvent::Type::Motion) {
      host->previewDragWidth = std::clamp(static_cast<int>(std::lround(host->surface.width() - x)), 208, 800);
      host->surface.requestLayout();
    } else if (event.type == PointerEvent::Type::Button && !event.pressed && event.button == BTN_LEFT) {
      active.savePreviewWidth(*host->previewDragWidth);
      host->previewDragWidth.reset();
    } else if (event.type == PointerEvent::Type::Leave) {
      host->previewDragWidth.reset();
      host->surface.requestLayout();
    }
    return;
  }
  if (host->resizing) {
    if (event.type == PointerEvent::Type::Motion) {
      m_model.resize(host->id, (x - host->browsingBounds.x) / std::max(1.0F, host->browsingBounds.width));
      host->surface.requestLayout();
    }
    if (event.type == PointerEvent::Type::Button && !event.pressed) {
      host->resizing = false;
    }
    return;
  }
  if (stripMovePointer(*host, event)) {
    return;
  }
  if (tabDragPointer(*host, event, x, y)) {
    return;
  }
  if (event.type == PointerEvent::Type::Button) {
    // Wayland uses evdev codes, not GTK/X11's numbered mouse buttons.
    const bool back = event.button == BTN_SIDE || event.button == BTN_BACK;
    const bool forward = event.button == BTN_EXTRA || event.button == BTN_FORWARD;
    if (back || forward) {
      if (event.pressed && !host->pressedTab && !active.overlay()) {
        auto target = window.active;
        for (auto tab : host->mounted) {
          if (m_pages.at(tab)->contains(x, y)) {
            target = tab;
            break;
          }
        }
        auto& page = *m_pages.at(target);
        if (!page.overlay()) {
          if (target != window.active) {
            m_model.focus(host->id, target);
            host->surface.requestLayout();
            prepare(*host);
          }
          const auto action = back ? luciole::BrowserAction::Back : luciole::BrowserAction::Forward;
          PageContext& actions = page;
          if (actions.actionState(action, actions.actionRequest(false)).enabled) {
            actions.dispatchAction(action);
          }
        }
      }
      return;
    }
  }
  if (host->strip && host->strip->visible() && host->strip->containsScenePoint(x, y)) {
    host->pointerPage = 0;
    for (const auto& [tab, button] : host->buttons) {
      if (host->tabScroll->containsScenePoint(x, y)
          && button->containsScenePoint(x, y)
          && (event.button == BTN_MIDDLE
              || !host->closeButtons.at(tab).first->visible()
              || !host->closeButtons.at(tab).first->containsScenePoint(x, y))
          && event.type == PointerEvent::Type::Button
          && event.pressed) {
        if (event.button == BTN_MIDDLE) {
          defer(host->id, tab, WorkspaceCommand::Close);
          return;
        }
        if (event.button == BTN_RIGHT) {
          tabMenu(*host, tab, x, y);
          return;
        }
        if (event.button == BTN_LEFT) {
          host->pressedTab = tab;
          host->pressX = x;
          host->pressY = y;
          float bx = 0, by = 0;
          Node::absolutePosition(button, bx, by);
          host->grabX = x - bx;
          host->grabY = y - by;
          return;
        }
      }
    }
    if (event.type == PointerEvent::Type::Button
        && event.pressed
        && event.button == BTN_LEFT
        && host->tabScroll->containsScenePoint(x, y)) {
      bool child = host->newTabButton->containsScenePoint(x, y);
      for (const auto& [tab, button] : host->buttons) {
        (void)tab;
        float bx = 0, by = 0;
        Node::absolutePosition(button, bx, by);
        child |= x >= bx && x <= bx + button->width();
      }
      if (!child) {
        host->stripMove.press(event.serial, x, y);
        return;
      }
    }
    dispatch(host->dispatcher, event);
    return;
  }
  if (host->previewDivider
      && host->previewDivider->visible()
      && host->previewDivider->containsScenePoint(x, y)
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && event.button == BTN_LEFT) {
    host->previewDragWidth = active.previewWidth();
    return;
  }
  if (host->divider
      && host->divider->visible()
      && host->divider->containsScenePoint(x, y)
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && event.button == BTN_LEFT) {
    host->resizing = true;
    host->resizeStart = window.fraction;
    return;
  }
  // Ordinary presses focus the pane first. Middle-click folder opening chooses tab activation separately.
  if (!active.overlay() && event.type == PointerEvent::Type::Button && event.pressed && event.button != BTN_MIDDLE) {
    for (auto tab : host->mounted) {
      if (tab != window.active && m_pages.at(tab)->contains(x, y)) {
        m_model.focus(host->id, tab);
        host->surface.requestLayout();
        prepare(*host);
        break;
      }
    }
  }
  // Pane content goes to the page under the pointer; the chrome around the panes belongs to the active page.
  auto target = m_model.windows().at(host->id).active;
  for (auto tab : host->mounted) {
    if (m_pages.at(tab)->contains(x, y)) {
      target = tab;
      break;
    }
  }
  host->pointerPage = target;
  m_pages.at(target)->pointer(event);
}

void WorkspaceApplication::clearExternalDrops() {
  for (auto& [id, page] : m_pages) {
    (void)id;
    page->clearExternalDrop();
  }
}

void WorkspaceApplication::dispatch(InputDispatcher& dispatcher, const PointerEvent& event) {
  const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter:
    dispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    dispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    dispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
}

luciole::WindowId WorkspaceApplication::newWindow(std::unique_ptr<FileChooserSession> chooser) {
  const auto id = m_model.addWindow();
  if (!id) {
    return 0;
  }
  auto host = std::make_unique<Host>(id, m_connection);
  if (chooser) {
    host->surface.setParent(chooser->request().parentWindow, chooser->request().modal);
    host->chooser = std::move(chooser);
  }
  auto* raw = host.get();
  host->surface.setRenderContext(&m_renderer);
  host->surface.setAnimationManager(&m_animations);
  host->surface.setPrepareFrameCallback([this, raw](bool, bool) { prepare(*raw); });
  host->surface.setUpdateCallback([] {});
  host->surface.setFrameCallbacks(
      [] { luciole::startupRendered(); },
      [this] {
        luciole::startupFrameDone();
        // Device rows follow the first frame instead of delaying it.
        if (!m_drivesStarted) {
          m_drivesStarted = true;
          m_drives.start();
          luciole::startupTrace("drives-started");
        }
      }
  );
  host->surface.setConfigureCallback([this, raw](std::uint32_t, std::uint32_t) {
    if (raw->tooltip) {
      raw->tooltip->dismiss();
    }
    raw->previewDragWidth.reset();
    for (auto& [tab, page] : m_pages) {
      (void)tab;
      if (page->boundSurface() == &raw->surface) {
        page->configurationChanged();
      }
    }
    raw->surface.requestLayout();
  });
  host->dispatcher.setCursorShapeCallback([this](auto serial, auto shape) {
    m_connection.extension<luciole::DesktopIntegration>().setCursorShape(serial, shape);
  });
  host->dispatcher.setFocusChangeCallback([this, raw](InputArea*, InputArea* focused) { focusChanged(*raw, focused); });
  const bool choosing = host->chooser != nullptr;
  host->surface.setUseSSD(m_defaults.useSSD);
  if (!host->surface.initialize(
          nullptr,
          {.width = choosing ? 900U : 1080U,
           .height = choosing ? 600U : 720U,
           .minWidth = 620,
           .minHeight = 400,
           .title = choosing ? host->chooser->windowTitle() : "Luciole",
           .appId = choosing ? "dev.noctalia.luciole-chooser" : "dev.noctalia.luciole"}
      )) {
    throw std::runtime_error("Cannot initialize a Luciole window.");
  }
  host->dispatcher.setTextInputContext(host->surface.wlSurface(), &m_input);
  m_hosts.emplace(id, std::move(host));
  m_focused = id;
  return id;
}

luciole::WindowId WorkspaceApplication::browsingWindow() const {
  luciole::WindowId first = 0;
  for (const auto& [id, host] : m_hosts) {
    if (host->closed || host->chooser || !host->surface.isRunning() || !m_model.windows().contains(id)) {
      continue;
    }
    if (id == m_focused) {
      return id;
    }
    if (!first) {
      first = id;
    }
  }
  return first;
}

luciole::TabId WorkspaceApplication::newTab(
    luciole::WindowId window, std::optional<luciole::Location> location, luciole::TabId duplicate, bool activate
) {
  if (m_pages.size() >= pageLimit) {
    return 0;
  }
  const auto previous = m_model.windows().at(window).active;
  const auto id = m_model.addTab(window, activate);
  if (!id) {
    return 0;
  }
  return createPage(window, id, std::move(location), duplicate, previous);
}

luciole::TabId WorkspaceApplication::newPane(
    luciole::WindowId window, std::optional<luciole::Location> location, luciole::TabId duplicate
) {
  if (m_pages.size() >= pageLimit) {
    return 0;
  }
  const auto previous = m_model.windows().at(window).active;
  const auto id = m_model.addPane(window);
  if (!id) {
    return 0;
  }
  return createPage(window, id, std::move(location), duplicate, previous);
}

luciole::TabId WorkspaceApplication::createPage(
    luciole::WindowId window, luciole::TabId id, std::optional<luciole::Location> location, luciole::TabId duplicate,
    luciole::TabId previous
) {
  auto& host = *m_hosts.at(window);
  auto settings = duplicate                    ? m_pages.at(duplicate)->options()
      : previous && m_pages.contains(previous) ? m_pages.at(previous)->options()
                                               : m_defaults;
  // File pickers keep their own layout, apart from the browser's.
  if (host.chooser) {
    settings.sort = settings.chooserSort;
    settings.viewMode = settings.chooserViewMode;
    settings.showHidden = settings.chooserShowHidden;
    settings.gridIconSize = settings.chooserGridIconSize;
    settings.listIconSize = settings.chooserListIconSize;
  }
  auto page = std::make_unique<BrowserPage>(
      m_connection, m_renderer, m_input, settings, m_clipboard, host.surface, host.dispatcher, m_animations, m_pins,
      m_drives, m_icons, m_thumbnails, m_preferences, host.chooser ? nullptr : &m_folderViews, m_hub, id
  );
  page->setHasOther([this, id] {
    const auto owner = m_model.owner(id);
    return owner && m_model.other(*owner).has_value();
  });
  page->setWorkspaceRequest([this, id](WorkspaceCommand command, std::optional<luciole::Location> destination) {
    if (auto owner = m_model.owner(id)) {
      defer(*owner, id, command, std::move(destination));
    }
  });
  page->setOperationRequest([this](luciole::JobId target, bool retry) {
    const std::weak_ptr<int> lifetime = m_lifetime;
    DeferredCall::callLater([this, lifetime, target, retry] {
      if (lifetime.expired()) {
        return;
      }
      const auto* job = m_hub.find(target);
      const auto page = job ? m_hub.owner(*job) : 0;
      const auto window = m_model.owner(page);
      const auto tab = m_model.tabFor(page);
      if (!window || !tab || !m_pages.contains(page)) {
        return;
      }
      select(*window, *tab);
      (void)m_model.focus(*window, page);
      m_pages.at(page)->reviewOperation(target, retry);
      m_hosts.at(*window)->surface.requestLayout();
      m_connection.activateSurface(m_hosts.at(*window)->surface.wlSurface());
    });
  });
  page->setPopupAvailable([this, id] {
    const auto page = m_pages.find(id);
    if (page == m_pages.end()) {
      return false;
    }
    for (const auto& [other, value] : m_pages) {
      if (other != id && value->boundSurface() == page->second->boundSurface() && value->popup()) {
        return false;
      }
    }
    return true;
  });
  page->setForeignDrop([this, id](float x, float y) -> std::optional<std::pair<luciole::Location, LayoutRect>> {
    const auto owner = m_model.owner(id);
    if (!owner) {
      return std::nullopt;
    }
    auto& host = *m_hosts.at(*owner);
    if (const auto window = m_model.windows().find(*owner);
        window != m_model.windows().end() && m_pages.at(window->second.active)->sidebarOverlays(x, y)) {
      return std::nullopt;
    }
    for (auto other : host.mounted) {
      if (other != id) {
        if (auto target = m_pages.at(other)->foreignTarget(x, y)) {
          return target;
        }
      }
    }
    for (const auto& [tab, button] : host.buttons) {
      if (tab != id && host.tabScroll->containsScenePoint(x, y) && button->containsScenePoint(x, y)) {
        if (auto destination = m_pages.at(m_model.layout(tab)->active)->location()) {
          float bx = 0, by = 0;
          Node::absolutePosition(button, bx, by);
          return std::pair(*destination, LayoutRect{bx, by, button->width(), button->height()});
        }
      }
    }
    return std::nullopt;
  });
  page->initialize(location ? std::optional(location->uri()) : std::nullopt);
  if (duplicate) {
    page->duplicateFrom(*m_pages.at(duplicate));
  }
  m_pages.emplace(id, std::move(page));
  host.labels.clear();
  host.surface.requestLayout();
  return id;
}

void WorkspaceApplication::select(luciole::WindowId window, luciole::TabId tab) {
  if (m_model.select(window, tab)) {
    m_hosts.at(window)->surface.requestLayout();
    m_focused = window;
  }
}

void WorkspaceApplication::defer(
    luciole::WindowId window, luciole::TabId tab, WorkspaceCommand command, std::optional<luciole::Location> location
) {
  if (m_hosts.at(window)->chooser) {
    return;
  }
  if ((command == WorkspaceCommand::CopyOther || command == WorkspaceCommand::MoveOther) && !location) {
    if (auto other = m_model.other(window)) {
      location = m_pages.at(*other)->location();
    }
  }
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime, window, tab, command, location = std::move(location)] {
    if (!lifetime.expired() && m_model.owner(tab) == window) {
      execute(window, tab, command, location);
    }
  });
}

void WorkspaceApplication::execute(
    luciole::WindowId window, luciole::TabId tab, WorkspaceCommand command,
    const std::optional<luciole::Location>& destination
) {
  auto& host = *m_hosts.at(window);
  const auto logical = m_model.tabFor(tab);
  const auto layout = m_model.layout(tab);
  const auto source = (logical && *logical == tab && layout) ? layout->active : tab;
  auto& page = *m_pages.at(source);
  if (logical
      && (command == WorkspaceCommand::Duplicate
          || command == WorkspaceCommand::Split
          || command == WorkspaceCommand::Swap)) {
    m_model.select(window, *logical);
  }
  switch (command) {
  case WorkspaceCommand::ClosePane:
    m_model.closePane(window, source);
    break;
  case WorkspaceCommand::NewTab:
  case WorkspaceCommand::OpenTab:
  case WorkspaceCommand::NewBackgroundTab: {
    const bool activate =
        command == WorkspaceCommand::NewTab || (command == WorkspaceCommand::OpenTab && page.options().switchToNewTab);
    if (!newTab(window, destination ? destination : page.location(), 0, activate)) {
      page.workspaceError("Close a tab before opening another.");
    }
    break;
  }
  case WorkspaceCommand::Duplicate: {
    const auto record = m_model.windows().at(window);
    const auto cloned = newTab(window, m_pages.at(record.primary)->location(), record.primary);
    if (cloned && record.secondary) {
      const auto auxiliary = newPane(window, m_pages.at(*record.secondary)->location(), *record.secondary);
      m_model.resize(window, record.fraction);
      if (record.active == record.primary) {
        m_model.focus(window, cloned);
      } else if (auxiliary) {
        m_model.focus(window, auxiliary);
      }
    }
    break;
  }
  case WorkspaceCommand::Close:
    if (m_model.windows().at(window).tabs.size() == 1) {
      saveWindows({});
    }
    m_model.close(tab);
    break;
  case WorkspaceCommand::Reopen:
    if (m_model.reopen(window)) {
      for (auto& [pageId, value] : m_pages) {
        if (m_model.tabFor(pageId) == m_model.windows().at(window).selected) {
          value->rebind(host.surface, host.dispatcher);
        }
      }
    }
    break;
  case WorkspaceCommand::NewWindow: {
    // Single-window mode turns an explicit new window into a tab of the current one.
    if (m_defaults.singleWindow) {
      if (!newTab(window, destination ? destination : page.location())) {
        page.workspaceError("Close a tab before opening another.");
      }
      break;
    }
    if (auto id = newWindow()) {
      const bool restored = !destination && restoreLastWindow(id);
      if (!restored && !newTab(id, destination ? destination : page.location())) {
        m_hosts.at(id)->surface.close();
        page.workspaceError("Close a tab before opening another window.");
      }
    } else {
      page.workspaceError("The eight-window limit has been reached.");
    }
    break;
  }
  case WorkspaceCommand::Detach:
  case WorkspaceCommand::MoveWindow: {
    luciole::WindowId destinationWindow = 0;
    if (command == WorkspaceCommand::Detach) {
      destinationWindow = m_defaults.singleWindow ? luciole::WindowId{0} : newWindow();
    } else {
      for (const auto& [id, target] : m_hosts) {
        if (id > window && !target->closed) {
          destinationWindow = id;
          break;
        }
      }
      if (!destinationWindow) {
        for (const auto& [id, target] : m_hosts) {
          if (id != window && !target->closed) {
            destinationWindow = id;
            break;
          }
        }
      }
    }
    if (destinationWindow) {
      std::vector<luciole::TabId> pages;
      for (const auto& [pageId, value] : m_pages) {
        (void)value;
        if (m_model.tabFor(pageId) == logical) {
          pages.push_back(pageId);
        }
      }
      releaseChrome(host);
      for (auto pageId : pages) {
        m_pages.at(pageId)->hide();
        m_pages.at(pageId)->unmount();
        std::erase(host.mounted, pageId);
      }
      if (m_model.move(tab, destinationWindow, m_model.windows().at(destinationWindow).tabs.size())) {
        if (m_model.windows().at(window).tabs.empty()) {
          saveWindows({});
        }
        auto& target = *m_hosts.at(destinationWindow);
        for (auto pageId : pages) {
          m_pages.at(pageId)->rebind(target.surface, target.dispatcher);
        }
        target.surface.requestLayout();
      }
    }
    break;
  }
  case WorkspaceCommand::Split:
    if (m_model.windows().at(window).secondary) {
      m_model.unsplit(window);
    } else if (!newPane(window, page.location(), source)) {
      page.workspaceError("The browser-page limit has been reached.");
    }
    break;
  case WorkspaceCommand::SwitchArea:
    if (auto other = m_model.other(window)) {
      m_model.focus(window, *other);
    }
    break;
  case WorkspaceCommand::Swap:
    m_model.swap(window);
    break;
  case WorkspaceCommand::OpenOther:
    if (auto other = m_model.other(window)) {
      if (destination) {
        m_pages.at(*other)->open(*destination);
      }
    } else {
      newPane(window, destination ? destination : page.location());
    }
    break;
  case WorkspaceCommand::CopyOther:
  case WorkspaceCommand::MoveOther:
    if (destination) {
      page.transferTo(*destination, command == WorkspaceCommand::CopyOther);
    }
    break;
  }
  host.labels.clear();
  host.surface.requestLayout();
  if (m_model.windows().at(window).tabs.empty()) {
    host.surface.close();
  }
}

void WorkspaceApplication::releaseChrome(Host& host) {
  if (!host.chromeOwner) {
    return;
  }
  auto& page = *m_pages.at(host.chromeOwner);
  page.deactivate();
  if (host.toolbarNode && host.toolbarNode->parent()) {
    page.returnToolbar(host.toolbarNode->parent()->removeChild(host.toolbarNode));
  }
  host.toolbarNode = nullptr;
  page.returnSidebar();
  host.chromeOwner = 0;
}

std::optional<luciole::SavedWindow> WorkspaceApplication::savedWindow(luciole::WindowId window) const {
  const auto found = m_model.windows().find(window);
  if (found == m_model.windows().end() || found->second.tabs.empty()) {
    return std::nullopt;
  }
  luciole::SavedWindow saved;
  const auto selected = std::ranges::find(found->second.tabs, found->second.selected);
  if (selected != found->second.tabs.end()) {
    saved.activeTab = static_cast<std::size_t>(selected - found->second.tabs.begin());
  }
  for (const auto tab : found->second.tabs) {
    const auto layout = m_model.layout(tab);
    if (!layout || !m_pages.contains(layout->primary)) {
      return std::nullopt;
    }
    const auto primary = m_pages.at(layout->primary)->location();
    if (!primary) {
      return std::nullopt;
    }
    luciole::SavedTab value{.primary = *primary, .secondary = std::nullopt, .activePane = luciole::ActivePane::Primary};
    if (layout->secondary) {
      if (!m_pages.contains(*layout->secondary)) {
        return std::nullopt;
      }
      const auto secondary = m_pages.at(*layout->secondary)->location();
      if (!secondary) {
        return std::nullopt;
      }
      value.secondary = *secondary;
      value.activePane =
          layout->active == *layout->secondary ? luciole::ActivePane::Secondary : luciole::ActivePane::Primary;
    }
    saved.tabs.push_back(std::move(value));
  }
  return saved;
}

bool WorkspaceApplication::restoreWindow(luciole::WindowId window, const luciole::SavedWindow& saved) {
  std::size_t openTabs = 0;
  for (const auto& [id, record] : m_model.windows()) {
    (void)id;
    openTabs += record.tabs.size();
  }
  const auto requiredPages =
      std::ranges::count_if(saved.tabs, [](const auto& tab) { return tab.secondary.has_value(); });
  if (saved.tabs.size() > luciole::TabWorkspace::tabLimit - openTabs
      || saved.tabs.size() + requiredPages > pageLimit - m_pages.size()) {
    return false;
  }
  std::vector<luciole::TabId> tabs;
  for (const auto& savedTab : saved.tabs) {
    const auto tab = newTab(window, savedTab.primary);
    if (!tab) {
      break;
    }
    tabs.push_back(tab);
    if (savedTab.secondary) {
      if (const auto pane = newPane(window, savedTab.secondary)) {
        (void)m_model.focus(window, savedTab.activePane == luciole::ActivePane::Secondary ? pane : tab);
      }
    }
  }
  if (tabs.empty()) {
    return false;
  }
  select(window, tabs[std::min(saved.activeTab, tabs.size() - 1)]);
  return true;
}

bool WorkspaceApplication::restoreLastWindow(luciole::WindowId window) {
  if (!m_defaults.restoreLastWindow || m_savedWindows.empty()) {
    return false;
  }
  const auto saved = m_savedWindows.back();
  if (!restoreWindow(window, saved)) {
    return false;
  }
  saveWindows({});
  return true;
}

void WorkspaceApplication::saveWindows(std::vector<luciole::SavedWindow> windows) {
  m_savedWindows = windows;
  if (!m_pins.saveWindows(windows)) {
    m_pendingWindows = std::move(windows);
  }
}

bool WorkspaceApplication::stripMovePointer(Host& host, const PointerEvent& event) {
  if (!host.stripMove.pending()) {
    return false;
  }
  if (event.type == PointerEvent::Type::Motion) {
    if (const auto serial = host.stripMove.motion(static_cast<float>(event.sx), static_cast<float>(event.sy))) {
      host.surface.beginMove(*serial);
    }
    return true;
  }
  if (event.type == PointerEvent::Type::Button || event.type == PointerEvent::Type::Leave) {
    host.stripMove.cancel();
    // The press never reached the strip's controls, so neither does its release.
    return event.type == PointerEvent::Type::Button && !event.pressed;
  }
  return false;
}

void WorkspaceApplication::buildTabDrag(Host& host) {
  auto ghost = ui::row(
      {.out = &host.tabGhost,
       .align = FlexAlign::Center,
       .paddingH = Style::spaceSm,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerHighest),
       .radius = Style::radiusMd,
       .border = colorSpecFromRole(ColorRole::OutlineVariant),
       .opacity = kTabGhostOpacity,
       .visible = false,
       .participatesInLayout = false},
      ui::label(
          {.out = &host.tabGhostLabel,
           .color = colorSpecFromRole(ColorRole::OnSurface),
           .maxLines = 1,
           .textAlign = TextAlign::Center,
           .ellipsize = TextEllipsize::End,
           .flexGrow = 1}
      )
  );
  ghost->setZIndex(60);
  host.root->addChild(std::move(ghost));
  auto marker = ui::box({.out = &host.tabDropMarker, .fill = colorSpecFromRole(ColorRole::Primary), .visible = false});
  marker->setParticipatesInLayout(false);
  marker->setRadius(1.5F);
  // Under the copy, which usually covers the gap: the marker shows above and below it.
  marker->setZIndex(55);
  host.root->addChild(std::move(marker));
}

bool WorkspaceApplication::tabDragPointer(Host& host, const PointerEvent& event, float x, float y) {
  if (!host.pressedTab) {
    return false;
  }
  if (event.type == PointerEvent::Type::Motion) {
    host.tabDrag |= std::abs(x - host.pressX) > 8 || std::abs(y - host.pressY) > 8;
    if (host.tabDrag) {
      updateTabEdgeScroll(host);
      host.surface.requestLayout();
    }
    return true;
  }
  if (event.type != PointerEvent::Type::Button || event.pressed || event.button != BTN_LEFT) {
    return false;
  }
  const auto tab = *std::exchange(host.pressedTab, std::nullopt);
  if (!std::exchange(host.tabDrag, false)) {
    select(host.id, tab);
    return true;
  }
  if (tabDetaches(host, y)) {
    defer(host.id, tab, WorkspaceCommand::Detach);
  } else {
    m_model.move(tab, host.id, tabDropIndex(host, tab, x));
    host.labels.clear();
  }
  host.surface.requestLayout();
  return true;
}

bool WorkspaceApplication::tabDetaches(const Host& host, float y) const {
  // Single Window has no detach, and detaching a lone tab would only reopen its window.
  return !m_defaults.singleWindow && m_model.windows().at(host.id).tabs.size() > 1 && y > kTabDetachY;
}

float WorkspaceApplication::tabEdgeScrollSpeed(const Host& host) const {
  if (!host.pressedTab || !host.tabDrag || tabDetaches(host, host.pointerY) || !host.tabScroll->scrollable()) {
    return 0;
  }
  float left = 0, top = 0;
  Node::absolutePosition(host.tabScroll, left, top);
  const float right = left + host.tabScroll->width();
  const float offset = host.tabScroll->scrollOffset();
  // Faster the nearer the pointer is to the end, and at full speed past it. Nothing once the bar is at that end,
  // so the timer stops instead of relaying out the window with nothing to scroll.
  if (host.pointerX < left + kTabEdgeZone) {
    return offset <= 0.0F
        ? 0.0F
        : -kTabEdgeStep * std::clamp((left + kTabEdgeZone - host.pointerX) / kTabEdgeZone, 0.0F, 1.0F);
  }
  if (host.pointerX > right - kTabEdgeZone) {
    return offset >= host.tabScroll->maxScrollOffset()
        ? 0.0F
        : kTabEdgeStep * std::clamp((host.pointerX - right + kTabEdgeZone) / kTabEdgeZone, 0.0F, 1.0F);
  }
  return 0;
}

void WorkspaceApplication::updateTabEdgeScroll(Host& host) {
  if (tabEdgeScrollSpeed(host) == 0) {
    host.tabEdgeScroll.stop();
    return;
  }
  if (host.tabEdgeScroll.active()) {
    return;
  }
  host.tabEdgeScroll.startRepeating(std::chrono::milliseconds(16), [this, &host] {
    const float speed = tabEdgeScrollSpeed(host);
    if (speed == 0) {
      host.tabEdgeScroll.stop();
      return;
    }
    auto* scroll = host.tabScroll;
    scroll->requestScrollToOffset(std::clamp(scroll->scrollOffset() + speed, 0.0F, scroll->maxScrollOffset()));
    host.surface.requestLayout();
  });
}

std::size_t WorkspaceApplication::tabDropIndex(const Host& host, luciole::TabId tab, float x) {
  // The dragged tab leaves its place first, so only the other tabs count.
  std::size_t index = 0;
  for (const auto& [id, button] : host.buttons) {
    float bx = 0, by = 0;
    Node::absolutePosition(button, bx, by);
    if (id != tab && x > bx + button->width() * 0.5F) {
      ++index;
    }
  }
  return index;
}

void WorkspaceApplication::layoutTabDrag(Host& host, Renderer& renderer) const {
  const auto dragged = host.pressedTab && host.tabDrag ? host.pressedTab : std::nullopt;
  Button* source = nullptr;
  for (const auto& [id, button] : host.buttons) {
    button->setOpacity(id == dragged ? kDraggedTabOpacity : 1.0F);
    if (id == dragged) {
      source = button;
    }
  }
  host.tabGhost->setVisible(source != nullptr);
  host.tabDropMarker->setVisible(false);
  if (source == nullptr) {
    return;
  }
  const auto label = std::ranges::find(host.labels, *dragged, &std::pair<luciole::TabId, std::string>::first);
  host.tabGhostLabel->setText(label != host.labels.end() ? label->second : std::string{});
  const float width = source->width(), height = source->height();
  const auto surfaceWidth = static_cast<float>(host.surface.width());
  const auto surfaceHeight = static_cast<float>(host.surface.height());
  const bool detach = tabDetaches(host, host.pointerY);
  float sx = 0, sy = 0;
  Node::absolutePosition(source, sx, sy);
  // Within the strip the copy slides along it; pulled out, it follows the pointer freely.
  const float x = std::clamp(host.pointerX - host.grabX, 0.0F, std::max(0.0F, surfaceWidth - width));
  const float y = detach ? std::clamp(host.pointerY - host.grabY, 0.0F, std::max(0.0F, surfaceHeight - height)) : sy;
  host.tabGhost->arrange(renderer, {x, y, width, height});
  if (detach) {
    return;
  }
  // The marker sits in the gap before the tab the dragged one would land in front of.
  const auto index = tabDropIndex(host, *dragged, host.pointerX);
  std::size_t position = 0;
  float markerX = -1;
  for (const auto& [id, button] : host.buttons) {
    if (id == dragged) {
      continue;
    }
    float bx = 0, by = 0;
    Node::absolutePosition(button, bx, by);
    if (position == index) {
      markerX = bx - Style::spaceXs * 0.5F;
      break;
    }
    markerX = bx + button->width() + Style::spaceXs * 0.5F;
    ++position;
  }
  const auto current = static_cast<std::size_t>(
      std::ranges::find(host.buttons, *dragged, &std::pair<luciole::TabId, Button*>::first) - host.buttons.begin()
  );
  // Dropping there would leave the tab where it is.
  if (markerX < 0 || index == current) {
    return;
  }
  host.tabDropMarker->setVisible(true);
  host.tabDropMarker->arrange(renderer, {markerX - 1.5F, 0.0F, 3.0F, kTabStripHeight});
}

void WorkspaceApplication::tabMenu(Host& host, luciole::TabId tab, float x, float y) {
  auto& page = *m_pages.at(m_model.windows().at(host.id).active);
  std::vector<ContextMenuControlEntry> entries{
      {.id = 0, .label = "New Tab", .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::NewTab)},
      {.id = 1,
       .label = "Duplicate Tab",
       .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::DuplicateTab)},
      {.id = 2,
       .label = "Close Tab",
       .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::CloseTab)},
      {.id = 3,
       .label = "Reopen Closed Tab",
       .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::ReopenTab)},
      {.label = "", .separator = true},
      {.id = 4,
       .label = "Split View",
       .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::SplitView)},
      {.id = 5, .label = "Swap Panes"}
  };
  if (!m_defaults.singleWindow) {
    entries.push_back({.id = 6, .label = "Move Tab to New Window"});
    entries.push_back({.id = 7, .label = "Move Tab to Next Window"});
  }
  const auto window = host.id;
  page.workspaceMenu(
      std::move(entries),
      [this, window, tab](int id) {
        const std::array commands{WorkspaceCommand::NewTab, WorkspaceCommand::Duplicate, WorkspaceCommand::Close,
                                  WorkspaceCommand::Reopen, WorkspaceCommand::Split,     WorkspaceCommand::Swap,
                                  WorkspaceCommand::Detach, WorkspaceCommand::MoveWindow};
        if (id >= 0 && static_cast<std::size_t>(id) < commands.size()) {
          defer(window, tab, commands[static_cast<std::size_t>(id)]);
        }
      },
      x, y
  );
}

Button::ButtonPalette WorkspaceApplication::closeTabPalette(ColorRole foreground) {
  // Palette roles only: the selected tab sits on Primary, others on the strip's SurfaceContainerLow chrome.
  const bool selected = foreground == ColorRole::OnPrimary;
  const auto background =
      colorSpecFromRole(selected ? ColorRole::PrimaryContainer : ColorRole::SurfaceContainerHighest);
  const auto label = colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface);
  auto palette = Button::defaultPalette(ButtonVariant::Ghost);
  palette.normal.label = colorSpecFromRole(foreground);
  palette.hover.bg = background;
  palette.hover.label = label;
  palette.pressed.bg = background;
  palette.pressed.label = label;
  return palette;
}

void WorkspaceApplication::rebuildStrip(Host& host, const luciole::TabWindow& window) {
  std::vector<std::pair<luciole::TabId, std::string>> labels;
  labels.reserve(window.tabs.size());
  for (auto id : window.tabs) {
    labels.emplace_back(id, m_pages.at(m_model.layout(id)->active)->title());
  }
  const bool decorated = !host.surface.serverDecorated() && !host.surface.fullscreen();
  // Client-side window controls keep the strip when tabs are hidden.
  const bool tabsHidden = host.chooser || (labels.size() <= 1 && !m_defaults.alwaysShowTabBar);
  const bool stripVisible = !tabsHidden || decorated;
  if (host.strip
      && labels == host.labels
      && decorated == host.decorated
      && stripVisible == host.stripVisible
      && tabsHidden == host.tabsHidden) {
    return;
  }
  host.decorated = decorated;
  host.stripVisible = stripVisible;
  host.tabsHidden = tabsHidden;
  const float scrollOffset = host.tabScroll ? host.tabScroll->scrollOffset() : 0.0F;
  // Only a press held on the strip is lost with it; presses held in the panes survive the rebuild.
  if (host.dispatcher.pointerCaptured() && !host.pointerPage) {
    host.dispatcher.cancelPointerCapture();
  }
  if (host.strip) {
    auto removed = host.root->removeChild(host.strip);
  }
  host.buttons.clear();
  host.closeButtons.clear();
  host.revealedTab = 0;
  host.labels = std::move(labels);
  auto scroll = ui::scrollView(
      {.out = &host.tabScroll,
       .orientation = ScrollOrientation::Horizontal,
       .scrollbarVisible = false,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .fillHeight = true,
       .flexGrow = 1}
  );
  scroll->setVisible(!host.tabsHidden);
  auto tabs = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs, .height = kTabStripHeight});
  scroll->content()->setDirection(FlexDirection::Horizontal);
  const auto windowId = host.id;
  auto tabPalette = Button::defaultPalette(ButtonVariant::Ghost);
  tabPalette.hover.bg = colorSpecFromRole(ColorRole::SurfaceContainerHigh);
  tabPalette.hover.label = colorSpecFromRole(ColorRole::OnSurface);
  tabPalette.pressed.bg = colorSpecFromRole(ColorRole::SurfaceContainerHighest);
  tabPalette.pressed.label = colorSpecFromRole(ColorRole::OnSurface);
  for (const auto& [id, label] : host.labels) {
    Button* button = nullptr;
    auto tab = ui::button(
        {.out = &button,
         .text = label,
         .controlHeight = Style::controlHeightSm,
         .contentAlign = ButtonContentAlign::Start,
         .variant = ButtonVariant::Ghost,
         .customPalette = tabPalette,
         .minWidth = 96.0F,
         .maxWidth = 200.0F,
         .paddingV = 0.0F,
         .paddingH = Style::spaceXs,
         .gap = Style::spaceXs,
         .onClick = [this, windowId, id] { select(windowId, id); },
         .configure =
             [](Button& button) {
               button.label()->setUseMarkup(false);
               button.label()->setFlexGrow(1);
               button.label()->setTextAlign(TextAlign::Center);
             }}
    );
    host.buttons.emplace_back(id, button);
    Button* close = nullptr;
    const auto foreground = id == window.selected ? ColorRole::OnPrimary : ColorRole::OnSurface;
    tab->addChild(
        ui::button(
            {.out = &close,
             .glyph = "x",
             .glyphSize = Style::baseGlyphSize,
             .controlHeight = 24.0F,
             .variant = ButtonVariant::Ghost,
             .customPalette = closeTabPalette(foreground),
             .tooltip = "Close tab",
             .followsCloseButtons = true,
             .minWidth = 24.0F,
             .maxWidth = 24.0F,
             .padding = 0.0F,
             .onClick = [this, windowId, id] { defer(windowId, id, WorkspaceCommand::Close); },
             .configure = [](Button& button) { button.setZIndex(2); }}
        )
    );
    host.closeButtons.emplace(id, std::pair(close, foreground));
    tabs->addChild(std::move(tab));
  }
  tabs->addChild(
      ui::button(
          {.out = &host.newTabButton,
           .glyph = "plus",
           .glyphSize = Style::baseGlyphSize,
           .controlHeight = Style::controlHeightSm,
           .variant = ButtonVariant::Ghost,
           .tooltip = luciole::shortcutTooltip("New tab", luciole::KeybindCommand::NewTab),
           .onClick = [this, windowId] {
             defer(windowId, m_model.windows().at(windowId).active, WorkspaceCommand::NewTab);
           }}
      )
  );
  scroll->content()->addChild(std::move(tabs));
  scroll->requestScrollToOffset(scrollOffset);
  auto strip = ui::row(
      {.out = &host.strip,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs,
       .paddingH = Style::spaceSm,
       .height = kTabStripHeight,
       .participatesInLayout = false},
      std::move(scroll),
      ui::button(
          {.out = &host.splitButton,
           .glyph = "layout-columns",
           .glyphSize = Style::baseGlyphSize,
           .controlHeight = Style::controlHeightSm,
           .selected = window.secondary.has_value(),
           .variant = ButtonVariant::Ghost,
           .tooltip = luciole::shortcutTooltip("Split view", luciole::KeybindCommand::SplitView),
           .onClick = [this, windowId] {
             defer(windowId, m_model.windows().at(windowId).active, WorkspaceCommand::Split);
           }}
      )
  );
  if (!host.surface.serverDecorated() && !host.surface.fullscreen()) {
    strip->addChild(
        ui::button(
            {.glyph = "minus",
             .glyphSize = Style::baseGlyphSize,
             .controlHeight = Style::controlHeightSm,
             .variant = ButtonVariant::Ghost,
             .tooltip = "Minimize window",
             .onClick = [&host] { host.surface.minimize(); }}
        )
    );
    strip->addChild(
        ui::button(
            {.glyph = "square",
             .glyphSize = Style::baseGlyphSize,
             .controlHeight = Style::controlHeightSm,
             .variant = ButtonVariant::Ghost,
             .tooltip = "Toggle maximized",
             .onClick = [&host] { host.surface.toggleMaximized(); }}
        )
    );
    strip->addChild(
        ui::button(
            {.glyph = "x",
             .glyphSize = Style::baseGlyphSize,
             .controlHeight = Style::controlHeightSm,
             .variant = ButtonVariant::Ghost,
             .tooltip = luciole::shortcutTooltip("Close window", luciole::KeybindCommand::CloseWindow),
             .onClick = [&host] { host.surface.close(); }}
        )
    );
  }
  host.splitButton->setVisible(!host.tabsHidden);
  strip->setJustify(host.tabsHidden ? FlexJustify::End : FlexJustify::Start);
  strip->setVisible(host.stripVisible);
  strip->setZIndex(50);
  host.root->addChild(std::move(strip));
}

void WorkspaceApplication::syncTooltip(Host& host) {
  if (!host.tooltip) {
    return;
  }
  if (host.resizing
      || host.previewDragWidth
      || host.pressedTab
      || host.dispatcher.pointerCaptured()
      || std::ranges::any_of(
          m_pages,
          [&host](const auto& item) { return item.second->boundSurface() == &host.surface && item.second->popup(); }
      )
      || std::ranges::any_of(host.mounted, [this](auto tab) { return m_pages.at(tab)->dragging(); })) {
    host.tooltip->dismiss();
    return;
  }
  std::optional<luciole::FileTooltipTarget> file;
  for (auto tab : host.mounted) {
    file = m_pages.at(tab)->fileTooltipAt(host.pointerX, host.pointerY);
    if (file) {
      break;
    }
  }
  host.tooltip->sync(
      host.surface.renderTarget().renderer(), *host.root, host.pointerX, host.pointerY,
      {static_cast<float>(host.surface.width()), static_cast<float>(host.surface.height())}, std::move(file)
  );
}

void WorkspaceApplication::focusChanged(Host& host, InputArea* focused) {
  for (auto tab : host.mounted) {
    m_pages.at(tab)->focusChanged(focused);
  }
  const auto found = m_model.windows().find(host.id);
  if (host.closed || focused == nullptr || found == m_model.windows().end()) {
    return;
  }
  // Keyboard focus decides the active pane: a press or Tab into the other pane activates it on the next frame,
  // which keeps the focus it just received.
  for (auto tab : host.mounted) {
    if (tab != found->second.active && m_pages.at(tab)->owns(focused)) {
      m_model.focus(host.id, tab);
      host.surface.requestLayout();
      return;
    }
  }
}

void WorkspaceApplication::prepare(Host& host) {
  if (host.closed
      || !host.surface.isRunning()
      || !m_model.windows().contains(host.id)
      || !m_renderer.makeCurrent(host.surface.renderTarget())) {
    return;
  }
  UiPhaseScope phase(UiPhase::Layout);
  const auto window = m_model.windows().at(host.id);
  if (!window.active) {
    return;
  }
  if (!host.root) {
    host.root = ui::column({.out = &host.background, .align = FlexAlign::Stretch});
    host.root->setAnimationManager(&m_animations);
    host.surface.setSceneRoot(host.root.get());
    host.dispatcher.setSceneRoot(host.root.get());
    auto tooltip = std::make_unique<TooltipPresenter>([&host] { host.surface.requestUpdate(); });
    host.tooltip = tooltip.get();
    host.root->addChild(std::move(tooltip));
    auto divider = std::make_unique<InputArea>();
    divider->setParticipatesInLayout(false);
    divider->setZIndex(40);
    divider->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
    divider->setFocusable(true);
    divider->setTabFocusKey(std::format("window:{}:split", host.id));
    divider->setOnKeyDown([this, &host](const InputArea::KeyData& key) {
      if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
        m_model.resize(
            host.id, m_model.windows().at(host.id).fraction + (KeySymbol::isLeft(key.sym) ? -0.025F : 0.025F)
        );
      } else if (key.sym == XKB_KEY_Home) {
        m_model.resize(host.id, 0.5F);
      }
      host.surface.requestLayout();
    });
    divider->setOnEnter([&host](const auto&) { host.surface.requestLayout(); });
    divider->setOnLeave([&host] { host.surface.requestLayout(); });
    host.divider = divider.get();
    auto line = ui::box({.fill = colorSpecFromRole(ColorRole::OutlineVariant), .width = 1.0F});
    divider->addChild(std::move(line));
    host.root->addChild(std::move(divider));
    auto preview = std::make_unique<InputArea>();
    preview->setParticipatesInLayout(false);
    preview->setZIndex(40);
    preview->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
    preview->setFocusable(true);
    preview->setTabFocusKey(std::format("window:{}:preview", host.id));
    preview->setOnKeyDown([this, &host](const InputArea::KeyData& key) {
      if (!key.pressed) {
        return;
      }
      auto& page = *m_pages.at(m_model.windows().at(host.id).active);
      if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
        page.savePreviewWidth(page.previewWidth() + (KeySymbol::isLeft(key.sym) ? 16 : -16));
      } else if (key.sym == XKB_KEY_Home) {
        page.savePreviewWidth(320);
      }
    });
    preview->setOnEnter([&host](const auto&) { host.surface.requestLayout(); });
    preview->setOnLeave([&host] { host.surface.requestLayout(); });
    host.previewDivider = preview.get();
    preview->addChild(
        ui::box({.out = &host.previewResizeLine, .fill = colorSpecFromRole(ColorRole::OutlineVariant), .width = 1})
    );
    host.root->addChild(std::move(preview));
    buildTabDrag(host);
    auto bar = ui::box({.out = &host.activePaneBar, .fill = colorSpecFromRole(ColorRole::Primary), .visible = false});
    bar->setParticipatesInLayout(false);
    bar->setRadius(1.0F);
    bar->setZIndex(40);
    host.root->addChild(std::move(bar));
    auto backdrop =
        ui::box({.out = &host.dialogBackdrop, .fill = colorSpecFromRole(ColorRole::Scrim), .visible = false});
    backdrop->setParticipatesInLayout(false);
    backdrop->setHitTestVisible(false);
    backdrop->setOpacity(0.5F);
    backdrop->setZIndex(100);
    host.root->addChild(std::move(backdrop));
  }
  if (host.backgroundOpacity != m_defaults.backgroundOpacity) {
    host.backgroundOpacity = m_defaults.backgroundOpacity;
    host.background->setFill(
        colorSpecFromRole(ColorRole::SurfaceContainerLow, static_cast<float>(host.backgroundOpacity))
    );
  }
  if (host.pendingFooter) {
    host.root->addChild(std::move(host.pendingFooter));
  }
  auto& active = *m_pages.at(window.active);
  active.ensureScene();
  if (host.chromeOwner != window.active) {
    auto* previous = host.chromeOwner ? m_pages.at(host.chromeOwner).get() : nullptr;
    releaseChrome(host);
    if (previous) {
      active.reusePlacesFrom(*previous);
    }
    host.toolbar = active.takeToolbar();
    host.toolbarNode = host.toolbar.get();
    host.toolbar->setParticipatesInLayout(false);
    host.toolbar->setVisible(true);
    host.toolbar->setZIndex(40);
    host.root->addChild(std::move(host.toolbar));
    active.takeSidebar(*host.root);
    active.sidebar()->setParticipatesInLayout(false);
    active.sidebar()->setZIndex(30);
    host.chromeOwner = window.active;
  }
  rebuildStrip(host, window);
  const float stripHeight = host.stripVisible ? kTabStripHeight : 0.0F;
  host.splitButton->setSelected(window.secondary.has_value());
  std::vector<luciole::TabId> visible{window.primary};
  if (window.secondary && !active.overlay()) {
    visible.push_back(*window.secondary);
  }
  if (active.overlay()) {
    visible = {window.active};
  }
  for (auto tab : host.mounted) {
    if (!std::ranges::contains(visible, tab)) {
      auto& page = *m_pages.at(tab);
      page.hide();
      page.unmount();
    }
  }
  host.mounted = visible;
  for (auto tab : visible) {
    auto& page = *m_pages.at(tab);
    page.ensureScene();
    page.mount(*host.root);
    page.root()->setParticipatesInLayout(false);
    page.root()->setZIndex(tab == window.active ? 10 : 0);
  }
  const bool dragging = std::ranges::any_of(host.mounted, [this](auto tab) { return m_pages.at(tab)->dragging(); });
  for (const auto& [tab, button] : host.buttons) {
    const bool selected =
        tab == window.selected || (dragging && button->containsScenePoint(host.pointerX, host.pointerY));
    button->setSelected(selected);
    auto& [close, role] = host.closeButtons.at(tab);
    const auto foreground = selected ? ColorRole::OnPrimary : ColorRole::OnSurface;
    if (foreground != role) {
      close->setCustomPalette(closeTabPalette(foreground));
      role = foreground;
    }
    if (tab == window.selected && (host.revealedTab != tab || host.revealedTabWidth != host.surface.width())) {
      host.tabScroll->requestScrollIntoView(tab == window.tabs.back() ? host.newTabButton : button);
      host.revealedTab = tab;
      host.revealedTabWidth = host.surface.width();
    }
  }
  auto& renderer = host.surface.renderTarget().renderer();
  const auto width = static_cast<float>(host.surface.width()), height = static_cast<float>(host.surface.height());
  host.root->arrange(renderer, {0, 0, width, height});
  host.strip->arrange(renderer, {0, 0, width, kTabStripHeight});
  layoutTabDrag(host, renderer);
  const float footer = host.chooser ? kChooserFooterHeight : 0.0F;
  if (host.chooserFooter) {
    host.chooser->sync();
    host.chooserFooter->arrange(renderer, {0, height - footer, width, footer});
  }
  host.toolbarNode->setVisible(!active.overlay());
  active.sidebar()->setVisible(!active.overlay());
  if (active.overlay()) {
    host.divider->setVisible(false);
    host.activePaneBar->setVisible(false);
    host.previewDivider->setVisible(false);
    active.setPreviewBounds(std::nullopt);
    active.prepare({0, stripHeight, width, height - stripHeight - footer}, true, false);
    syncTooltip(host);
    return;
  }
  host.toolbarNode->arrange(renderer, {0, stripHeight, width, kNavigationToolbarHeight});
  // Chrome (tabs, toolbar, sidebar) wraps one flush content plane: panes and the docked preview touch, separated
  // only by 1px lines; the plane's top-left corner is rounded where it meets the sidebar.
  const float top = stripHeight + kNavigationToolbarHeight;
  const float bodyHeight = std::max(1.0F, height - top - footer);
  active.prepareSidebar(width);
  const float sidebar = active.sidebarWidth();
  active.arrangeSidebar(renderer, top, bodyHeight);
  const float left = sidebar;
  const float right = width;
  const bool split = visible.size() == 2;
  const float previewBudget = right - left - kMinimumPaneWidth * (split ? 2.0F : 1.0F);
  const float previewWidth = active.previewVisible() && previewBudget >= kMinimumPreviewWidth
      ? std::min(static_cast<float>(host.previewDragWidth.value_or(active.previewWidth())), previewBudget)
      : 0;
  host.previewDivider->setVisible(previewWidth > 0);
  const auto lineRole = [](bool strong, bool hovered) {
    return colorSpecFromRole(
        strong        ? ColorRole::Primary
            : hovered ? ColorRole::OnSurfaceVariant
                      : ColorRole::OutlineVariant
    );
  };
  host.previewResizeLine->setFill(
      lineRole(host.previewDragWidth || host.previewDivider->focused(), host.previewDivider->hovered())
  );
  host.previewDivider->arrange(renderer, {right - previewWidth - 3, top, 6, bodyHeight});
  host.previewDivider->children().front()->arrange(renderer, {2.5F, 0, 1, bodyHeight});
  for (auto tab : visible) {
    m_pages.at(tab)->setPreviewBounds(
        tab == window.active && previewWidth > 0
            ? std::optional(LayoutRect{right - previewWidth, top, previewWidth, bodyHeight})
            : std::nullopt
    );
  }
  const float content = std::max(1.0F, right - left - previewWidth);
  host.browsingBounds = {left, top, content, bodyHeight};
  const float minimumPane = std::min(kMinimumPaneWidth, content * 0.5F);
  const float first = split ? std::clamp(content * window.fraction, minimumPane, content - minimumPane) : content;
  host.divider->setVisible(split);
  host.divider->arrange(renderer, {left + first - 3, top, 6, bodyHeight});
  auto* splitLine = host.divider->children().front().get();
  static_cast<Box*>(splitLine)->setFill(lineRole(host.resizing || host.divider->focused(), host.divider->hovered()));
  splitLine->arrange(renderer, {2.5F, 0, 1.0F, bodyHeight});
  // The active pane of a split view carries a Primary bar along its top edge.
  host.activePaneBar->setVisible(split);
  for (std::size_t index = 0; index < visible.size(); ++index) {
    const auto tab = visible[index];
    const LayoutRect bounds{left + (index ? first : 0), top, index ? content - first : first, bodyHeight};
    if (split && tab == window.active) {
      // Inset by the corner radius on both ends so it reads as an indicator, not a clipped border.
      const float radius = workspaceRadius();
      host.activePaneBar->arrange(renderer, {bounds.x + radius, top, std::max(0.0F, bounds.width - radius * 2), 2});
    }
    m_pages.at(tab)->prepare(bounds, tab == window.active, index == 0 && left > 0);
  }
  active.layoutNavigation(renderer);
  syncTooltip(host);
  host.dialogBackdrop->setVisible(std::ranges::any_of(host.mounted, [this](auto tab) {
    return m_pages.at(tab)->dimsWindow();
  }));
  host.dialogBackdrop->arrange(renderer, {0, 0, width, height});
}
