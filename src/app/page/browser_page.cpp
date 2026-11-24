#include "app/page/browser_page.h"

#include "app/startup_trace.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "browser/path_completion.h"
#include "browser/selection_tools.h"
#include "browser/tab_workspace.h"
#include "clipboard/text_clipboard.h"
#include "config/keybinds.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fs/background_job.h"
#include "fs/gio_ptr.h"
#include "fs/link_target.h"
#include "model/details_columns.h"
#include "operations/archive_service.h"
#include "operations/batch_rename_service.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/permission_service.h"
#include "operations/rename_service.h"
#include "operations/restore_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"
#include "platform/desktop_integration.h"
#include "platform/file_launcher.h"
#include "platform/terminal_launcher.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/rect_node.h"
#include "thumbnails/preview_service.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/browser/file_icon.h"
#include "ui/browser/file_view.h"
#include "ui/browser/file_view_metrics.h"
#include "ui/browser/places_sidebar.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/side_list_metrics.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/dialogs/name_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {
  // Dialog width tiers (see DialogWidth) and the gap a dialog keeps to each window edge.
  constexpr float kDialogWidthSmall = 440.0F;
  constexpr float kDialogWidthMedium = 600.0F;
  constexpr float kDialogWidthLarge = 880.0F;
  constexpr float kDialogMargin = 32.0F;
  // The lib's default popup shadow, as on its context menus.
  constexpr ShadowStyle kDialogShadow{};
} // namespace

BrowserPage::BrowserPage(
    WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
    const luciole::BrowserSettings& settings, luciole::WaylandTextClipboard& clipboard, luciole::WindowSurface& surface,
    InputDispatcher& dispatcher, AnimationManager& animations, luciole::PinService& pins, luciole::DriveMonitor& drives,
    luciole::FileIconCache& icons, luciole::ThumbnailService& thumbnails, luciole::ViewPreferenceWriter& preferences,
    luciole::FolderViewService* folderViews, OperationHub& hub, luciole::TabId id
)
    : m_connection(connection), m_renderer(renderer), m_textInput(textInput), m_animations(animations),
      m_surface(&surface), m_dispatcher(&dispatcher),
      m_browser(settings.sort, settings.showHidden, settings.showBackups), m_icons(icons), m_thumbnails(thumbnails),
      m_listAdapter(
          m_browser, m_icons, m_thumbnails, luciole::ViewMode::List,
          [this] { return m_connection.keyboardModifiers(); },
          [this] { return m_fileKeys.keyboardFocus() && m_list != nullptr && m_list->focusArea()->focused(); }
      ),
      m_gridAdapter(
          m_browser, m_icons, m_thumbnails, luciole::ViewMode::Grid,
          [this] { return m_connection.keyboardModifiers(); },
          [this] { return m_fileKeys.keyboardFocus() && m_list != nullptr && m_list->focusArea()->focused(); }
      ),
      m_preferences(preferences), m_savedSettings(settings), m_pins(pins), m_folderViews(folderViews), m_drives(drives),
      m_hub(hub), m_jobs(hub, id), m_viewMode(settings.viewMode), m_clipboard(clipboard), m_workspaceId(id) {
  if (m_preferences.settings()) {
    m_savedSettings = m_preferences.settings()->browser;
  }
  m_gridZoom = static_cast<std::size_t>(
      std::ranges::find(luciole::kGridIconSizes, settings.gridIconSize) - luciole::kGridIconSizes.begin()
  );
  browser().setViewDefaults(settings.viewMode, settings.gridIconSize);
  m_browser.setFolderViews(m_folderViews);
  m_listIconSize = settings.listIconSize;
  m_millerIconSize = settings.millerIconSize;
  m_preview.setChangedCallback([this] {
    m_surface->requestLayout();
    if (m_popupSurface) {
      m_popupSurface->requestLayout();
    }
  });
  m_thumbnailClearing = m_thumbnails.clearing();
  m_thumbnailError = m_thumbnails.error();
  m_browser.setCaptureViewport([this] {
    luciole::BrowseViewport viewport{.mode = m_viewMode, .iconSize = static_cast<int>(gridIconSize())};
    if (m_list) {
      viewport.vertical = m_list->scrollView().scrollOffset();
      viewport.horizontal = m_detailsHeader.offset();
      viewport.withinRow = m_list->firstVisibleOffset();
      if (const auto first = m_list->firstVisibleIndex(); first && *first < browser().model().rows().size()) {
        viewport.first = browser().model().rows()[*first];
      }
    }
    return viewport;
  });
  m_browser.setExpandableFolders(settings.expandableFolders && settings.viewMode == luciole::ViewMode::List);
  m_terminal.setCommand(settings.terminalCommand);
  m_launcher.setTerminalCommand(settings.terminalCommand);
  m_sidebar.load(settings);
  m_detailsHeader.load(settings);
  m_activationMode = settings.activationMode;
  m_fileKeys.setTypingMode(settings.typingMode);
  m_browser.setSearchRecursive(settings.typingMode != luciole::TypingMode::Filter);
  m_browser.setChangedCallback([this] {
    m_browserDirty = true;
    if (const auto& snapshot = m_browser.model().snapshot(); snapshot.status == luciole::DirectoryStatus::Ready) {
      luciole::startupListingReady(snapshot.entries.size());
    }
    m_surface->requestUpdate();
  });
  m_browser.setFileActivatedCallback([this](const luciole::Location& location) { m_launcher.open(location); });
  m_folderIconController.setOnChanged([this](const luciole::Location& folder) {
    m_propertiesController.refreshFolderIcon(folder);
  });
  m_miller.defaultWidth = settings.millerColumnWidth;
  m_miller.setChangedCallback([this] {
    m_browserDirty = true;
    m_surface->requestUpdate();
  });
  m_miller.setFileActivatedCallback([this](const luciole::Location& target) { m_launcher.open(target); });
  m_launcher.setActivationProvider([this](const std::string& appId, luciole::FileLauncher::ActivationReply reply) {
    m_connection.extension<luciole::DesktopIntegration>().requestActivation(
        m_surface->wlSurface(), m_connection.lastInputSerial(), appId, std::move(reply)
    );
  });
  m_launcher.setChangedCallback([this](const std::string& error) {
    m_statusBar.setMessage(error.empty() ? "" : std::format("Could not open the file: {}", error));
    m_aboutController.launcherChanged(error);
    m_browserDirty = true;
    m_surface->requestUpdate();
  });
  for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
    adapter->setCutPredicate([this](const luciole::Location& location) { return m_clipboard.isCut(location); });
    adapter->setActivationMode(m_activationMode);
    adapter->setActivateCallback([this](luciole::EntryId id) {
      auto request = actionRequest(false);
      if (const auto* entry = browser().model().entry(id)) {
        request.targets.push_back({id, entry->location()});
      }
      dispatchAction(m_connection.keyboardModifiers() == KeyMod::Alt ? Action::Properties : Action::Open, request);
    });
    adapter->setContextCallback([this](std::optional<luciole::EntryId> id, float x, float y) {
      m_menuController.openFileMenu(id, x, y);
    });
    adapter->setDragCallback(
        [this](const std::vector<luciole::EntryId>& ids) { m_dragDrop.startFileDrag(ids); },
        [this] { m_dragDrop.cancelFileDrag(); }
    );
  }
  m_listAdapter.setFolderSizeEnabled(true);
  m_hub.attach(m_workspaceId, *this);
}

luciole::BrowserController& BrowserPage::browser() { return m_miller.enabled() ? m_miller.active() : m_browser; }

const luciole::BrowserController& BrowserPage::browser() const {
  return m_miller.enabled() ? m_miller.active() : m_browser;
}

void BrowserPage::focusMiller(std::size_t index, bool reveal) {
  if (!m_millerView || index >= m_miller.columns().size()) {
    return;
  }
  const bool changed = m_miller.focus(index);
  if (auto* view = m_millerView->focusedView()) {
    m_list = view;
  } else {
    m_millerFocusPending = true;
  }
  if (changed) {
    m_fileKeys.clearTypeahead();
    m_visitSerial = 0;
    m_pendingHistoryViewport.reset();
    m_fileKeys.resetReveal();
    m_thumbnailContext = 0;
    m_browserDirty = true;
    if (reveal) {
      m_millerView->revealFocused();
    }
    m_surface->requestUpdate();
  }
}

BrowserPage::~BrowserPage() {
  m_hub.detach(m_workspaceId);
  m_dragDrop.stop();
  m_transferController.stopProgress();
  m_browser.setChangedCallback({});
  m_miller.setChangedCallback({});
  m_miller.setFileActivatedCallback({});
  m_browser.setFileActivatedCallback({});
  m_launcher.setChangedCallback({});
  m_preview.setChangedCallback({});
  m_preview.clear();
  closePopup();
  m_archiveController.close();
  unmount();
  m_root = nullptr;
  m_detachedRoot.reset();
}

void BrowserPage::iconThemeChanged() {
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::fontChanged() {
  m_browserDirty = true;
  m_surface->requestLayout();
  if (m_popupSurface) {
    m_popupSurface->requestLayout();
  }
}

bool BrowserPage::initialize(const std::optional<std::string>& initialLocation) {
  if (initialLocation) {
    (void)browser().openText(*initialLocation);
  } else {
    browser().home();
  }
  return true;
}

void BrowserPage::revealItems(std::vector<luciole::Location> items, bool properties) {
  m_revealTargets = std::move(items);
  m_revealProperties = properties;
  focusFileList();
}

bool BrowserPage::busy() const {
  return m_preview.busy()
      || m_preferences.busy()
      || m_pins.busy()
      || m_launcher.busy()
      || m_programs.busy()
      || m_transferController.pastePending()
      || m_transferController.extractionPending();
}

void BrowserPage::shutdown() {
  m_programs.cancel();
  m_preview.clear();
  m_transferController.cancelPaste();
  m_transferController.cancelExtraction();
  m_launcher.cancelApplications();
}

void BrowserPage::jobChanged(OperationJob& job) {
  m_transferController.updateOperationProgress();
  switch (job.kind()) {
  case JobKind::Transfer:
    m_transferController.transferChanged(job);
    break;
  case JobKind::Archive:
    m_archiveController.changed(job);
    break;
  case JobKind::Trash:
    m_trashController.trashChanged(job);
    break;
  case JobKind::Restore:
    m_trashController.restoreChanged(job);
    break;
  case JobKind::EmptyTrash:
    m_trashController.emptyTrashChanged(job);
    break;
  case JobKind::Rename:
    m_renameController.changed(job);
    break;
  case JobKind::BatchRename:
    m_batchRename.changed(job);
    break;
  case JobKind::Permission:
    m_propertiesController.changed(job);
    break;
  case JobKind::Create:
    m_createController.changed(job);
    break;
  }
  if (job.kind() == JobKind::Trash) {
    const auto status = job.service<luciole::TrashService>().snapshot().status;
    if (status == luciole::TrashStatus::Finished
        || status == luciole::TrashStatus::Error
        || status == luciole::TrashStatus::Cancelled) {
      m_sidebar.refreshTrashAvailabilityAfterJob(job.id());
    }
  } else if (job.kind() == JobKind::EmptyTrash) {
    const auto status = job.service<luciole::EmptyTrashService>().snapshot().status;
    if (status == luciole::EmptyTrashStatus::Finished
        || status == luciole::EmptyTrashStatus::Error
        || status == luciole::EmptyTrashStatus::Cancelled) {
      m_sidebar.refreshTrashAvailabilityAfterJob(job.id());
    }
  } else if (job.kind() == JobKind::Restore && !job.service<luciole::RestoreService>().busy()) {
    m_sidebar.refreshTrashAvailabilityAfterJob(job.id());
  }
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::thumbnailsChanged() {
  if (m_thumbnailClearing != m_thumbnails.clearing() || m_thumbnailError != m_thumbnails.error()) {
    m_thumbnailClearing = m_thumbnails.clearing();
    m_thumbnailError = m_thumbnails.error();
    m_settingsController.markDirty();
  }
  m_preview.thumbnailsChanged();
  m_surface->requestLayout();
}

void BrowserPage::leaveDrive(const luciole::Location& root) {
  const auto inside = [&](const luciole::Location& location) {
    return location == root || location.uri().starts_with(root.uri() + (root.uri().ends_with('/') ? "" : "/"));
  };
  const auto affected = [&](const luciole::BrowserController& controller) {
    const auto& location = controller.model().snapshot().location;
    return location
        && (inside(*location)
            || (controller.searchRecursive()
                && !controller.searchQuery().empty()
                && root.uri().starts_with(location->uri() + (location->uri().ends_with('/') ? "" : "/"))));
  };
  if (affected(m_browser)
      || std::ranges::any_of(m_miller.columns(), [&](const auto& column) { return affected(*column->browser); })) {
    m_preview.clear();
    m_retiringPreview = root;
    m_previewController.hidePane();
    m_miller.clear();
    m_miller.collectRetired();
    auto destination = luciole::Location::fromPath(g_get_home_dir());
    m_browser.open(inside(destination) ? luciole::Location::fromPath("/") : destination);
  }
  m_browser.releaseLocation(root);
  for (const auto& column : m_miller.columns()) {
    column->browser->releaseLocation(root);
  }
  if (m_sidebar.hasPlaces()) {
    m_sidebar.places().releaseLocation(root);
  }
  m_thumbnails.releaseLocation(root);
}

bool BrowserPage::driveReadersBusy(const luciole::Location& root) const {
  return m_thumbnails.readingLocation(root) || (m_retiringPreview == root && m_preview.busy());
}

void BrowserPage::transfersFinished(const std::vector<luciole::TransferResult>& results) {
  const auto refresh = [&](luciole::BrowserController& controller) {
    const auto& current = controller.model().snapshot().location;
    if (current
        && !current->isNative()
        && luciole::transferAffectsLocation(results, *current, !controller.searchQuery().empty())) {
      if (m_visible) {
        controller.refresh();
      } else {
        m_pendingProviderRefresh = true;
      }
    }
  };
  if (m_miller.enabled()) {
    for (const auto& column : m_miller.columns()) {
      refresh(*column->browser);
    }
  } else {
    refresh(m_browser);
  }
}

luciole::BrowserSettings BrowserPage::options() const {
  auto value = m_savedSettings;
  value.viewMode = m_viewMode;
  value.gridIconSize = static_cast<int>(gridIconSize());
  value.listIconSize = m_listIconSize;
  value.millerIconSize = m_millerIconSize;
  value.columns = m_detailsHeader.columnVisibility();
  value.columnOrder = m_detailsHeader.columnOrder();
  value.columnWidths = m_detailsHeader.columnWidths();
  // New tabs start from the defaults, not from the current folder's own settings.
  value.sort = m_folderView.enabled() ? browser().defaultSort() : browser().model().sort();
  if (m_folderView.enabled() && !m_miller.enabled()) {
    value.viewMode = browser().defaultViewMode();
  }
  value.showHidden = browser().model().filter().showHidden;
  value.showBackups = browser().model().filter().showBackups;
  return value;
}

std::optional<luciole::Location> BrowserPage::location() const { return browser().model().snapshot().location; }

std::string BrowserPage::title() const {
  if (!location()) {
    return "Luciole";
  }
  return location() == luciole::Location::network() ? "Network" : location()->displayName().value_or("Luciole");
}

void BrowserPage::preferencesChanged(const std::string& error) {
  m_settingsController.preferencesChanged(error);
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::placesChanged() {
  m_sidebar.markDirty();
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::driveUsageChanged() {
  // Background tabs build their scene on first mount, and new drive rows read the current usage then.
  if (!m_root) {
    return;
  }
  m_sidebar.updateDriveUsage();
  m_surface->requestUpdate();
}

void BrowserPage::clipboardChanged() {
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::clearExternalDrop() { m_dragDrop.clearExternalDrop(); }

const std::vector<luciole::Bookmark>* BrowserPage::draggedFolders() const { return m_dragDrop.draggedFolders(); }

luciole::FileDropTarget BrowserPage::externalDrop(float x, float y, const std::vector<luciole::Bookmark>* pins) {
  m_dragDrop.clearExternalDrop();
  // A file picker returns files to its caller; it never receives transfers.
  if (!m_visible || choosing() || overlay() || popup() || actionContext().operationsBusy) {
    return {};
  }
  m_pointerX = x;
  m_pointerY = y;
  return m_dragDrop.externalDrop(x, y, pins);
}

luciole::FileDropTarget BrowserPage::externalDestination(const luciole::Location& destination, LayoutRect rect) {
  return m_dragDrop.externalDestination(destination, rect);
}

void BrowserPage::iconsReady() {
  m_surface->requestLayout();
  if (m_popupSurface && !m_menuController.menu()) {
    m_popupSurface->requestLayout();
  }
}

void BrowserPage::reusePlacesFrom(BrowserPage& previous) { m_sidebar.reuseFrom(previous.m_sidebar); }

void BrowserPage::workspaceError(const std::string& message) { actionError(message); }

bool BrowserPage::owns(const Node* node) const {
  for (; node; node = node->parent()) {
    if (node == m_root || node == m_toolbar || node == m_sidebar.bundle()) {
      return true;
    }
  }
  return false;
}

void BrowserPage::focusChanged(InputArea* focused) {
  if (m_list != nullptr && focused != m_list->focusArea()) {
    m_fileKeys.clearTypeahead();
  }
  m_detailsHeader.focusChanged(focused);
}

bool BrowserPage::completeLocationOnTab(std::uint32_t modifiers) { return m_locationBar.completeOnTab(modifiers); }
bool BrowserPage::sidebarKey(const KeyboardEvent& event) { return m_sidebar.key(event); }
bool BrowserPage::sidebarOverlays(float x, float y) const { return !overlay() && m_sidebar.drawerContains(x, y); }
void BrowserPage::prepareSidebar(float width) {
  m_sidebar.prepareWidth(width);
  // The window arranges the sidebar before this page's layoutFrame(): rebuild a changed Places list now, so the
  // new rows are arranged and drawn in this frame instead of waiting for the next one.
  m_sidebar.invalidate();
}
void BrowserPage::arrangeSidebar(Renderer& renderer, float top, float height) {
  m_sidebar.arrange(renderer, top, height);
}

void BrowserPage::stampFocusKeys(Node& node) {
  if (auto* area = dynamic_cast<InputArea*>(&node); area && area->tabFocusKey().empty()) {
    area->setTabFocusKey(std::format("tab:{}:{}", m_workspaceId, ++m_nextFocusKey));
  }
  for (const auto& child : node.children()) {
    stampFocusKeys(*child);
  }
}

void BrowserPage::duplicateFrom(BrowserPage& source) { browser().duplicateFrom(source.browser()); }

void BrowserPage::open(const luciole::Location& location) { browser().open(location); }

void BrowserPage::setWorkspaceRequest(
    std::function<void(WorkspaceCommand, std::optional<luciole::Location>)> callback
) {
  m_workspaceRequest = std::move(callback);
}

void BrowserPage::reviewOperation(luciole::JobId id, bool retry) {
  m_pendingOperation = std::pair(id, retry);
  requestSync();
}

void BrowserPage::windowFocusChanged(bool entered) {
  if (!m_popupSurface) {
    return;
  }
  if (!entered) {
    m_popupDispatcher.pointerLeave();
    // Grabbing menus and dialog dropdowns take keyboard focus from the parent when they map (KWin, Hyprland), so
    // this leave is often their own; the compositor dismisses them when focus really moves to another client.
    return;
  }
  m_popupDispatcher.setTextInputContext(m_popupSurface->wlSurface(), &m_textInput, true, m_surface->wlSurface());
}

void BrowserPage::configurationChanged() {
  m_operations.parentConfigured();
  m_dragDrop.clearExternalDrop();
  m_locationBar.cancelBlankClick();
  if (m_popupSurface && m_dialogTier) {
    m_dialogResizePending = true;
    m_popupSurface->requestLayout();
  }
  if (m_sidebar.resizing() || m_detailsHeader.gestureActive() || (m_millerView && m_millerView->resizing())) {
    dispatcher().cancelPointerCapture();
  }
}

void BrowserPage::setHasOther(std::function<bool()> callback) { m_hasOther = std::move(callback); }

void BrowserPage::setPopupAvailable(std::function<bool()> callback) { m_popupAvailable = std::move(callback); }

void BrowserPage::setForeignDrop(
    std::function<std::optional<std::pair<luciole::Location, LayoutRect>>(float, float)> callback
) {
  m_dragDrop.setForeignDrop(std::move(callback));
}

bool BrowserPage::popupOwns(wl_surface* surface) const {
  return (m_popupSurface && m_popupSurface->wlSurface() == surface)
      || m_submenus.owns(surface)
      || m_dialogDropdown.owns(surface);
}

bool BrowserPage::popupCapturesAllKeys() const {
  const InputArea* focused = m_popupDispatcher.focusedArea();
  return focused != nullptr && focused->capturesAllKeys();
}

bool BrowserPage::popup() const { return m_popupSurface != nullptr; }

// Modal dialogs and quick preview dim the window; menus, popovers and other outside-click dialogs leave it undimmed.
bool BrowserPage::dimsWindow() const {
  return popup()
      && m_dialogTier.has_value()
      && (m_popupDismiss == PopupDismiss::Explicit || m_activePopup.kind() == ActivePopup::Kind::QuickPreview);
}

bool BrowserPage::overlay() const { return m_archiveController.open(); }

bool BrowserPage::previewEnabled() const {
  return choosing() ? m_savedSettings.chooserShowPreview : m_savedSettings.showPreview;
}

bool BrowserPage::previewVisible() const { return previewEnabled() && !overlay(); }

void BrowserPage::togglePreview() {
  const bool shown = !previewEnabled();
  (choosing() ? m_savedSettings.chooserShowPreview : m_savedSettings.showPreview) = shown;
  m_preferences.submit(
      {{choosing() ? luciole::BrowserPreference::ChooserShowPreview : luciole::BrowserPreference::ShowPreview, shown}}
  );
  m_surface->requestLayout();
}

int BrowserPage::previewWidth() const { return m_savedSettings.previewWidth; }

float BrowserPage::previewAreaWidth() const {
  return m_previewController.bounds() ? m_previewController.bounds()->width : 0;
}

void BrowserPage::savePreviewWidth(int width) {
  m_savedSettings.previewWidth = std::clamp(width, 208, 800);
  m_preferences.submit({{luciole::BrowserPreference::PreviewWidth, m_savedSettings.previewWidth}});
  m_surface->requestLayout();
}

void BrowserPage::setPreviewBounds(std::optional<LayoutRect> bounds) { m_previewController.setBounds(bounds); }

void BrowserPage::layoutNavigation(Renderer& renderer) {
  if (!m_toolbar->visible()) {
    return;
  }
  m_toolbar->arrange(renderer, {m_toolbar->x(), m_toolbar->y(), m_toolbar->width(), m_toolbar->height()});
  if (m_locationBar.layoutBreadcrumbs(renderer)) {
    m_dragDrop.requestDropRefresh();
  }
  m_locationBar.layoutCompletions(renderer);
  m_dragDrop.refreshDrop();
}

bool BrowserPage::dragging() const { return m_dragDrop.dragging(); }

bool BrowserPage::contains(float x, float y) const { return m_body && m_body->containsScenePoint(x, y); }

luciole::WindowSurface* BrowserPage::boundSurface() const { return m_surface; }

void BrowserPage::rebind(luciole::WindowSurface& surface, InputDispatcher& dispatcher) {
  deactivate();
  m_surface = &surface;
  m_dispatcher = &dispatcher;
}

void BrowserPage::prepare(LayoutRect bounds, bool active, bool roundCorner) {
  m_pageBounds = bounds;
  if (!m_visible) {
    m_visible = true;
    m_appliedViewMode.reset();
    m_pendingHistoryViewport = m_inactiveViewport;
    if (std::exchange(m_pendingProviderRefresh, false)) {
      if (m_miller.enabled()) {
        for (const auto& column : m_miller.columns()) {
          if (!column->location.isNative()) {
            column->browser->refresh();
          }
        }
      } else {
        m_browser.refresh();
      }
    }
  }
  const bool split = m_hasOther && m_hasOther();
  m_paneHeader->setVisible(split);
  m_paneHeader->setParticipatesInLayout(split);
  m_paneTitle->setText(title());
  m_paneTitle->setColor(colorSpecFromRole(active ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
  prepareFrame(true, true);
  stampFocusKeys(*m_root);
  stampFocusKeys(*m_toolbar);
  stampFocusKeys(*m_sidebar.bundle());
  // Panes are part of the flush content plane; the split view marks the active pane with the window's top bar.
  m_body->clearBorder();
  if (roundCorner) {
    m_body->setStyleRadii(Radii(Style::radiusXl, 0, 0, 0));
  } else {
    m_body->setRadii(Radii(0, 0, 0, 0));
  }
  if (active && !m_active) {
    m_active = true;
    // A press or Tab that activated the pane already focused inside it; otherwise bring back its last focus.
    if (!owns(dispatcher().focusedArea()) && m_stashedFocus) {
      dispatcher().restoreTabFocus(*m_stashedFocus);
    }
    if (!owns(dispatcher().focusedArea())) {
      dispatcher().setFocus(m_list->focusArea());
    }
  } else if (!active && m_active) {
    deactivate();
  }
  m_previewController.sync();
  m_previewController.layoutPane(active && previewVisible());
}

void BrowserPage::deactivate() {
  m_sidebar.dismissDrawer();
  m_dragDrop.clearExternalDrop();
  m_previewController.hidePane();
  if (m_root && owns(dispatcher().focusedArea())) {
    m_stashedFocus = dispatcher().captureTabFocus();
    dispatcher().setFocus(nullptr);
  } else if (m_active) {
    // The focus already left for another pane or the window chrome; there is no focus of this page to restore.
    m_stashedFocus.reset();
  }
  m_active = false;
  m_transferController.keepConflict();
  closePopup();
  m_fileKeys.clearTypeahead();
  m_dragDrop.cancelDrag();
}

void BrowserPage::hide() {
  deactivate();
  if (m_visible && m_list) {
    m_inactiveViewport = luciole::BrowseViewport{
        m_viewMode,
        static_cast<int>(gridIconSize()),
        m_list->scrollView().scrollOffset(),
        m_detailsHeader.offset(),
        m_list->firstVisibleOffset(),
        m_list->firstVisibleIndex() && *m_list->firstVisibleIndex() < browser().model().rows().size()
            ? std::optional(browser().model().rows()[*m_list->firstVisibleIndex()])
            : std::nullopt
    };
    // Releasing the cells lets the shared ThumbnailService evict this page's thumbnails when its budget needs room.
    m_list->setAdapter(nullptr);
    m_millerView->suspend();
    m_thumbnailContext = 0;
    m_thumbnailRevision = 0;
  }
  m_visible = false;
}

void BrowserPage::mount(Node& host) {
  if (m_detachedRoot) {
    host.addChild(std::move(m_detachedRoot));
  }
  m_dragDrop.mount(host);
}

void BrowserPage::unmount() {
  m_dragDrop.unmount();
  if (m_root && m_root->parent()) {
    m_detachedRoot = m_root->parent()->removeChild(m_root);
  }
}

Node* BrowserPage::root() const { return m_root; }

std::unique_ptr<Node> BrowserPage::takeToolbar() { return m_toolbar->parent()->removeChild(m_toolbar); }

void BrowserPage::returnToolbar(std::unique_ptr<Node> node) {
  if (node) {
    node->setVisible(false);
    m_root->addChild(std::move(node));
  }
}

void BrowserPage::takeSidebar(Node& host) { m_sidebar.take(host); }

void BrowserPage::returnSidebar() { m_sidebar.giveBack(); }

Node* BrowserPage::sidebar() const { return m_sidebar.bundle(); }

void BrowserPage::ensureScene() {
  if (!m_root) {
    UiPhaseScope phase(UiPhase::Layout);
    buildScene();
  }
}

float BrowserPage::sidebarWidth() const { return m_sidebar.width(); }

float BrowserPage::sidebarControlWidth() const { return m_sidebar.controlWidth(); }

void BrowserPage::pumpJobs() {
  if (m_root) {
    m_transferController.syncConflict();
    m_trashController.onFrame();
  }
}

void BrowserPage::workspaceMenu(
    std::vector<ContextMenuControlEntry> entries, std::function<void(int)> callback, float x, float y
) {
  m_menuController.showContextMenu(std::move(entries), std::move(callback), {x, y, 1, 1});
}

void BrowserPage::transferTo(const luciole::Location& destination, bool copy) {
  if (const auto request = std::exchange(m_pendingWorkspaceRequest, std::nullopt)) {
    m_transferController.transferTo(*request, destination, copy);
  }
}

std::optional<std::pair<luciole::Location, LayoutRect>> BrowserPage::foreignTarget(float x, float y) const {
  return m_dragDrop.foreignTarget(x, y);
}

void BrowserPage::outputChanged(wl_output* output) {
  if (m_connection.outputForSurface(m_surface->wlSurface()) == output) {
    m_surface->onSurfaceOutputEnter(m_surface->wlSurface(), output);
  }
  if (m_popupSurface != nullptr && m_connection.outputForSurface(m_popupSurface->wlSurface()) == output) {
    m_popupSurface->onSurfaceOutputEnter(m_popupSurface->wlSurface(), output);
  }
  m_submenus.outputChanged(output);
}

void BrowserPage::pointer(const PointerEvent& event) {
  if (m_dialogDropdown.pointer(event)) {
    return;
  }
  if (m_popupSurface != nullptr && event.type == PointerEvent::Type::Button && event.pressed) {
    const bool inPopup = m_submenus.owns(event.surface)
        || (event.surface == m_popupSurface->wlSurface()
            && event.sx >= 0
            && event.sy >= 0
            && event.sx < m_popupSurface->width()
            && event.sy < m_popupSurface->height());
    if (!inPopup) {
      dispatcher().cancelPointerCapture();
      m_popupDispatcher.cancelPointerCapture();
      if (m_popupDismiss == PopupDismiss::OutsideClick) {
        dismissPopup();
      }
      return;
    }
  }
  auto* input = event.surface == m_surface->wlSurface()                           ? &dispatcher()
      : m_popupSurface != nullptr && event.surface == m_popupSurface->wlSurface() ? &m_popupDispatcher
      : m_submenus.owns(event.surface)                                            ? &m_submenus.dispatcher()
                                                                                  : nullptr;
  if (input == nullptr) {
    return;
  }
  const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  if (input == &m_popupDispatcher) {
    // Dialog coordinates: the dialog sits inside its shadow frame on the popup surface.
    const auto* content = m_activePopup.root();
    m_batchRename.pointer(
        x - content->x(), y - content->y(),
        event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter
    );
    m_renameController.placeIconPointer(
        x - content->x(), y - content->y(),
        event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter
    );
  }
  const bool page = input == &dispatcher();
  if (page && sidebarOverlays(x, y) && !m_dragDrop.dragging()) {
    if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_MIDDLE && m_workspaceRequest) {
      (void)openInNewTab(x, y);
      if (m_sidebar.places().containsScenePoint(x, y)) {
        return;
      }
    }
    m_dragDrop.trackButton(event);
    pointerOverlay(*input, event, x, y);
    if (m_dragDrop.dragging()) {
      m_dragDrop.updateDrag();
    }
    return;
  }
  if (page
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && event.button == BTN_MIDDLE
      && m_workspaceRequest
      && openInNewTab(x, y)) {
    return;
  }
  if (overlay() && page) {
    pointerOverlay(*input, event, x, y);
    return;
  }
  m_pointerX = x;
  m_pointerY = y;
  if (page && millerPointer(event, x, y)) {
    return;
  }
  if (page) {
    m_dragDrop.trackButton(event);
    m_locationBar.pointer(event, x, y);
  }
  if (event.type == PointerEvent::Type::Button && event.pressed && page) {
    m_fileKeys.clearTypeahead();
  }
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    m_popupSerial = event.serial;
    if (page) {
      m_fileKeys.setKeyboardFocus(false);
      m_browserDirty = true;
      m_surface->requestUpdate();
    }
  }
  if (page && (m_detailsHeader.release(event, x, y) || m_detailsHeader.contextPress(event, x, y))) {
    return;
  }
  if (page
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && event.button == BTN_RIGHT
      && m_statusBar.noticeShown()
      && m_filePane->containsScenePoint(x, y)
      && !m_detailsHeader.headerContains(x, y)) {
    m_menuController.openFileMenu(std::nullopt, x, y);
    return;
  }
  if (page
      && m_dragDrop.dragging()
      && event.type == PointerEvent::Type::Button
      && event.button == BTN_LEFT
      && !event.pressed) {
    m_dragDrop.finishDrag();
    return;
  }
  if (page && m_locationBar.press(event, x, y)) {
    return;
  }
  if (page
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && event.button == BTN_LEFT
      && !m_surface->serverDecorated()
      && !m_surface->maximized()
      && !m_surface->fullscreen()) {
    const auto edges = resizeEdges(x, y);
    if (edges != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
      m_surface->beginResize(event.serial, edges);
      return;
    }
  }
  dispatchPointer(*input, event, x, y);
  if (input == &m_submenus.dispatcher() && event.type != PointerEvent::Type::Leave) {
    m_submenus.pointer();
  }
  if (input == &m_popupDispatcher
      && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
    m_submenus.menuHover();
  }
}

std::optional<luciole::FileTooltipTarget> BrowserPage::fileTooltipAt(float x, float y) const {
  if (!m_visible || overlay() || m_sidebar.drawerContains(x, y)) {
    return std::nullopt;
  }
  const luciole::BrowserController* controller = &browser();
  const VirtualGridView* view = m_list;
  std::uint64_t columnId = 0;
  if (m_miller.enabled()) {
    const auto column = m_millerView->columnAt(x, y);
    if (!column) {
      return std::nullopt;
    }
    controller = m_miller.columns()[*column]->browser.get();
    columnId = m_miller.columns()[*column]->id;
    view = m_millerView->view(*column);
  }
  if (!controller->model().selectionAvailable()) {
    return std::nullopt;
  }
  const auto index = view ? view->itemAtScene(x, y) : std::nullopt;
  const auto bounds = index ? view->itemRect(*index) : std::nullopt;
  if (!bounds || *index >= controller->model().rows().size()) {
    return std::nullopt;
  }
  if (m_viewMode == luciole::ViewMode::Grid) {
    float centerX = 0, centerY = 0;
    view->absoluteAnchorForIndex(*index, centerX, centerY);
    if (!m_gridAdapter.itemHitTest(
            *index, x - centerX + bounds->width * 0.5F, y - centerY + gridHeight() * 0.5F, bounds->width, gridHeight()
        )) {
      return std::nullopt;
    }
  }
  const auto* entry = controller->model().entry(controller->model().rows()[*index]);
  if (!entry) {
    return std::nullopt;
  }
  return luciole::FileTooltipTarget{
      .context = std::format("{}:{}:{}", m_workspaceId, columnId, controller->contextSerial()),
      .entry = *entry,
      .typeLabel = std::string(controller->model().typeLabel(entry->id)),
  };
}

bool BrowserPage::openInNewTab(float x, float y) {
  if (sidebarOverlays(x, y)) {
    if (m_sidebar.visible()) {
      if (const auto folder = m_sidebar.places().folderAt(x, y)) {
        m_workspaceRequest(WorkspaceCommand::OpenTab, folder->location);
      }
    }
    return true;
  }
  const auto command = (m_connection.keyboardModifiers() & KeyMod::Shift) != 0
      ? (m_savedSettings.switchToNewTab ? WorkspaceCommand::NewBackgroundTab : WorkspaceCommand::NewTab)
      : WorkspaceCommand::OpenTab;
  if (m_list && m_list->containsScenePoint(x, y)) {
    if (auto index = m_list->itemAtScene(x, y); index && *index < browser().model().rows().size()) {
      const auto* entry = browser().model().entry(browser().model().rows()[*index]);
      if (entry && entry->isFolderLink()) {
        m_workspaceRequest(command, entry->navigationLocation());
      }
      return true;
    }
  }
  for (const auto& [location, button] : m_locationBar.breadcrumbs()) {
    if (button->containsScenePoint(x, y)) {
      m_workspaceRequest(command, location);
      return true;
    }
  }
  if (m_sidebar.hasPlaces()) {
    if (auto folder = m_sidebar.places().folderAt(x, y)) {
      m_workspaceRequest(command, folder->location);
      return true;
    }
  }
  return false;
}

void BrowserPage::pointerOverlay(InputDispatcher& input, const PointerEvent& event, float x, float y) {
  m_pointerX = x;
  m_pointerY = y;
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    m_popupSerial = event.serial;
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    input.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    input.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    input.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    input.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
  m_surface->requestUpdate();
}

bool BrowserPage::millerPointer(const PointerEvent& event, float x, float y) {
  if (m_miller.enabled() && event.type == PointerEvent::Type::Button && event.pressed) {
    if (const auto column = m_millerView->columnAt(x, y)) {
      focusMiller(*column, false);
      m_miller.interact(*column);
    }
  }
  if (m_miller.enabled()
      && event.type == PointerEvent::Type::Axis
      && m_filePane->containsScenePoint(x, y)
      && (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL || m_connection.keyboardModifiers() == KeyMod::Shift)) {
    InputArea::PointerData data;
    data.axisSource = event.axisSource;
    data.axisValue = event.axisValue;
    data.axisLines = event.axisLines;
    const float delta = data.scrollDelta(Style::scrollWheelStep);
    if (std::isfinite(delta)) {
      m_millerView->scrollBy(delta);
    }
    m_surface->requestLayout();
    return true;
  }
  return false;
}

void BrowserPage::dispatchPointer(InputDispatcher& input, const PointerEvent& event, float x, float y) {
  const bool page = &input == &dispatcher();
  switch (event.type) {
  case PointerEvent::Type::Enter:
    input.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    input.pointerLeave();
    if (page) {
      m_dragDrop.cancelLocalDrag();
    }
    break;
  case PointerEvent::Type::Motion:
    input.pointerMotion(x, y, event.serial);
    if (page && m_dragDrop.dragging()) {
      m_dragDrop.updateDrag();
    }
    break;
  case PointerEvent::Type::Button:
    input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    if (&input == &m_popupDispatcher) {
      m_submenus.menuScrolled();
    }
    if (page
        && m_viewMode == luciole::ViewMode::List
        && m_filePane->containsScenePoint(x, y)
        && (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL
            || (event.axis == WL_POINTER_AXIS_VERTICAL_SCROLL && m_connection.keyboardModifiers() == KeyMod::Shift))) {
      m_detailsHeader.scrollWheel(event);
      break;
    }
    if (page && zoomFromScroll(event)) {
      break;
    }
    input.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
}

void BrowserPage::keyboard(const KeyboardEvent& event) {
  if (m_sidebar.key(event)) {
    return;
  }
  if (event.pressed) {
    m_popupSerial = m_connection.lastInputSerial();
    m_locationBar.keyPressed();
    if (escapeGesture(event)) {
      return;
    }
  }
  if (m_fileKeys.escapeKey(event)) {
    return;
  }
  if (m_detailsHeader.gestureActive()) {
    return;
  }
  if (m_dragDrop.dragging()) {
    m_dragDrop.updateDrag();
    return;
  }
  if (m_popupSurface != nullptr) {
    if (m_trashController.key(event)) {
      return;
    }
    if (m_propertiesController.resultsKey(event)) {
      return;
    }
    if (m_previewController.quickPreviewKey(event)) {
      return;
    }
    if (m_dialogDropdown.key(event)) {
      return;
    }
    if (m_folderIconController.escapeKey(event)) {
      return;
    }
    if (m_settingsController.key(event)) {
      return;
    }
    if (m_shortcutsController.key(event)) {
      return;
    }
    m_menuController.key(event);
    return;
  }
  if (m_archiveController.key(event)) {
    return;
  }
  if (choosing()
      && event.pressed
      && !event.preedit
      && luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers)) {
    if (!m_locationBar.escapeKey(event)) {
      m_chooser.cancel();
    }
    return;
  }
  // Enter on a file list without a focused or selected item answers with the folder itself, as a chooser's
  // default button would; with an item, the list activates it.
  if (choosing()
      && event.pressed
      && !event.preedit
      && event.modifiers == 0
      && KeySymbol::isEnter(event.sym)
      && m_list != nullptr
      && dispatcher().focusedArea() == m_list->focusArea()
      && !browser().model().selection().focused
      && browser().model().selectedIds().empty()) {
    m_chooser.open({});
    return;
  }
  if (event.pressed && !event.preedit && shortcutKey(event)) {
    return;
  }
  if (m_fileKeys.typeKey(event)) {
    return;
  }
  dispatcher().keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}

bool BrowserPage::escapeGesture(const KeyboardEvent& event) {
  const bool cancel = luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers);
  if ((m_detailsHeader.gestureActive() || (m_millerView && m_millerView->resizing())) && cancel) {
    dispatcher().cancelPointerCapture();
    return true;
  }
  if (m_sidebar.resizing() && cancel) {
    dispatcher().cancelPointerCapture();
    return true;
  }
  if (m_dragDrop.dragging() && cancel) {
    m_dragDrop.cancelDrag();
    return true;
  }
  return false;
}

bool BrowserPage::dispatchShortcut(const KeyboardEvent& event, std::initializer_list<Action> actions) {
  const auto action = luciole::shortcutAction(actions, event.sym, event.modifiers);
  if (action) {
    dispatchAction(*action);
  }
  return action.has_value();
}

bool BrowserPage::shortcutKey(const KeyboardEvent& event) {
  const bool editing = m_locationBar.textFocused();
  if (!editing && !event.repeat && dispatchShortcut(event, {Action::Terminal})) {
    return true;
  }
  if (!editing
      && !event.repeat
      && dispatchShortcut(
          event,
          {Action::CopyFiles, Action::CutFiles, Action::PasteFiles, Action::ShowPreview, Action::Undo, Action::Redo}
      )) {
    return true;
  }
  if (!editing
      && !event.repeat
      && luciole::shortcutMatches(Action::QuickPreview, event.sym, event.modifiers)
      && m_list != nullptr
      && dispatcher().focusedArea() == m_list->focusArea()) {
    dispatchAction(Action::QuickPreview);
    return true;
  }
  if (m_fileKeys.preeditKey(event)) {
    return true;
  }
  if (m_locationBar.menuKey(event) || m_locationBar.completionKey(event) || m_locationBar.filterKey(event)) {
    return true;
  }
  if (windowKey(event) || m_locationBar.escapeKey(event)) {
    return true;
  }
  // Cancel elsewhere returns to the file list.
  if (!editing
      && luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers)
      && m_list != nullptr
      && dispatcher().focusedArea() != m_list->focusArea()) {
    focusFileList();
    m_fileKeys.focusEntered();
    m_browserDirty = true;
    m_surface->requestUpdate();
    return true;
  }
  if (navigationKey(event, editing)) {
    return true;
  }
  if (!editing && (m_detailsHeader.menuKey(event) || m_fileKeys.commandKey(event))) {
    return true;
  }
  // View chords cannot be plain text keys, so they switch the view even while typing in a field.
  if (dispatchShortcut(event, {Action::Grid, Action::List, Action::Miller})) {
    return true;
  }
  if (!editing && dispatchShortcut(event, {Action::ShowHidden})) {
    return true;
  }
  if (m_list != nullptr
      && dispatcher().focusedArea() == m_list->focusArea()
      && (millerKey(event) || m_fileKeys.listKey(event))) {
    m_fileKeys.setKeyboardFocus(true);
    m_browserDirty = true;
    m_surface->requestUpdate();
    return true;
  }
  return false;
}

bool BrowserPage::windowKey(const KeyboardEvent& event) {
  const bool focusNext = luciole::keybindMatches(luciole::KeybindCommand::FocusNext, event.sym, event.modifiers);
  const bool focusPrevious =
      luciole::keybindMatches(luciole::KeybindCommand::FocusPrevious, event.sym, event.modifiers);
  if (focusNext || focusPrevious) {
    (void)dispatcher().cycleTabFocus(focusPrevious);
    if (m_list != nullptr && dispatcher().focusedArea() == m_list->focusArea()) {
      m_fileKeys.focusEntered();
      m_browserDirty = true;
    }
    m_surface->requestRedraw();
    return true;
  }
  if (luciole::keybindMatches(luciole::KeybindCommand::Fullscreen, event.sym, event.modifiers) && !event.repeat) {
    m_surface->toggleFullscreen();
    return true;
  }
  if (dispatchShortcut(event, {Action::Settings, Action::KeyboardShortcuts})) {
    return true;
  }
  if (luciole::shortcutMatches(Action::EditLocation, event.sym, event.modifiers) && m_locationBar.hasLocationInput()) {
    dispatchAction(Action::EditLocation);
    return true;
  }
  if (luciole::shortcutMatches(Action::Search, event.sym, event.modifiers) && m_locationBar.hasFilterInput()) {
    dispatchAction(Action::Search);
    return true;
  }
  return false;
}

bool BrowserPage::navigationKey(const KeyboardEvent& event, bool editing) {
  if (!editing && dispatchShortcut(event, {Action::Back, Action::Forward, Action::Up, Action::Home, Action::Refresh})) {
    return true;
  }
  if (!editing && m_detailsHeader.widthKey(event)) {
    return true;
  }
  if (!editing && zoomFromKey(event)) {
    return true;
  }
  if (!editing
      && !event.repeat
      && dispatchShortcut(event, {Action::ShowSidebar, Action::CreateFolder, Action::CreateFile})) {
    return true;
  }
  return false;
}

bool BrowserPage::millerKey(const KeyboardEvent& event) {
  const bool right = luciole::keybindMatches(luciole::KeybindCommand::MoveRight, event.sym, event.modifiers);
  const bool left = luciole::keybindMatches(luciole::KeybindCommand::MoveLeft, event.sym, event.modifiers);
  if (m_miller.enabled() && (left || right)) {
    const bool next = right != Style::rtl();
    if (next) {
      m_fileKeys.selectFirstOnLoad();
      if (m_miller.enter()) {
        focusMiller(m_miller.focused());
        m_millerFocusPending = true;
      } else {
        m_fileKeys.cancelPendingCursor();
      }
    } else if (actionState(Action::Up, actionRequest(false)).enabled) {
      dispatchAction(Action::Up);
    }
    return true;
  }
  return false;
}

luciole::ActionContext BrowserPage::actionContext() const {
  return {
      .model = browser().model(),
      .context = browser().contextSerial(),
      .query = browser().searchQuery(),
      .canBack = browser().canBack(),
      .canForward = browser().canForward(),
      .canUp = browser().canUp(),
      .operationsBusy =
          m_jobs.busy() || m_transferController.pastePending() || m_transferController.extractionPending(),
      .pinsEditable = m_pins.editable(),
      .pinsBusy = m_pins.busy(),
      .canEmptyTrash = !m_jobs.busy(),
      .pinned = [this](const auto& location) { return m_pins.contains(location); },
      .viewMode = m_viewMode,
      .showSidebar = m_sidebar.visible(),
      .showPreview = previewEnabled(),
      .clipboardAvailable = m_clipboard.available(),
      .clipboardHasFiles = m_clipboard.hasFiles(),
      .clipboardHasImage = m_clipboard.hasImage(),
      .terminalAvailable = m_terminal.available(),
      .canUndo = m_hub.canUndo(),
      .canRedo = m_hub.canRedo(),
      .chooser = choosing(),
      .singleSelection = browser().model().singleSelection(),
      .folderViews = m_folderView.enabled(),
      .folderViewCustomized = m_folderView.customized()
  };
}

luciole::ActionRequest BrowserPage::actionRequest(bool withTargets) const {
  if (withTargets) {
    return luciole::ActionRegistry::capture(actionContext());
  }
  return {browser().contextSerial(), browser().model().snapshot().location, browser().searchQuery(), {}};
}

luciole::ActionRequest BrowserPage::requestFor(const std::vector<luciole::DirectoryEntry>& entries) const {
  auto request = actionRequest(false);
  for (const auto& entry : entries) {
    request.targets.push_back({entry.id, entry.location()});
  }
  return request;
}

luciole::ActionState BrowserPage::actionState(Action action, const luciole::ActionRequest& request) const {
  return luciole::ActionRegistry::state(action, actionContext(), request);
}

void BrowserPage::dispatchAction(Action action) {
  dispatchAction(
      action, actionRequest(luciole::ActionRegistry::definition(action).scope == luciole::ActionScope::Targets)
  );
}

void BrowserPage::dispatchAction(Action action, const luciole::ActionRequest& request) {
  if (luciole::ActionRegistry::definition(action).scope == luciole::ActionScope::Targets
      && requestCurrent(request, luciole::ActionScope::Targets)) {
    std::vector<luciole::EntryId> ids;
    ids.reserve(request.targets.size());
    for (const auto& target : request.targets) {
      ids.push_back(target.id);
    }
    const std::weak_ptr<int> lifetime = m_lifetime;
    if (browser().whenMetadataReady(std::move(ids), [this, lifetime, action, request] {
          if (!lifetime.expired()) {
            dispatchAction(action, request);
          }
        })) {
      m_statusBar.setMessage("Reading item metadata…");
      return;
    }
  }
  if (action == Action::Open) {
    action = luciole::ActionRegistry::openAction(actionContext(), request);
    Logger("file-launch").debug("Open action dispatch ({} targets)", request.targets.size());
  }
  const auto state = actionState(action, request);
  if (!state.enabled) {
    actionError(std::string(state.reason));
    return;
  }
  m_statusBar.clearMessage();
  m_fileKeys.clearTypeahead();
  const bool selectionAction = luciole::ActionRegistry::definition(action).scope == luciole::ActionScope::Selection;
  if (selectionAction) {
    if (m_miller.enabled()) {
      m_miller.interact(m_miller.focused());
    }
    dispatcher().cancelPointerCapture();
    m_list->cancelPointerGesture();
  }
  std::vector<luciole::DirectoryEntry> targets;
  for (const auto& target : request.targets) {
    if (const auto* entry = browser().model().entry(target.id)) {
      targets.push_back(*entry);
    }
  }
  runAction(action, request, targets);
  if (!m_popupSurface && selectionAction) {
    dispatcher().setFocus(m_list->focusArea());
  }
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::runAction(
    Action action, const luciole::ActionRequest& request, const std::vector<luciole::DirectoryEntry>& targets
) {
  struct Call {
    Action action;
    const luciole::ActionRequest& request;
    const std::vector<luciole::DirectoryEntry>& targets;
  };
  struct Handler {
    Action action;
    void (*run)(BrowserPage& page, const Call& call);
  };
  static constexpr auto fileClipboard = [](BrowserPage& page, const Call& call) {
    page.copyFiles(call.action, call.targets);
  };
  static constexpr auto pathClipboard = [](BrowserPage& page, const Call& call) {
    page.copyPaths(call.action, call.request, call.targets);
  };
  static constexpr auto sort = [](BrowserPage& page, const Call& call) { page.sortAction(call.action); };
  static constexpr auto createItem = [](BrowserPage& page, const Call& call) {
    page.m_createController.openCreateItem(
        *call.request.folder,
        call.action == Action::CreateFolder ? luciole::CreateKind::Folder : luciole::CreateKind::EmptyFile
    );
  };
  // One handler per action in enum order; the static_assert rejects a missing or misplaced row.
  static constexpr auto kHandlers = std::to_array<Handler>({
      {Action::Back, [](BrowserPage& page, const Call&) { (void)page.browser().back(); }},
      {Action::Forward, [](BrowserPage& page, const Call&) { (void)page.browser().forward(); }},
      {Action::Up, [](BrowserPage& page, const Call&) { page.leaveFolder(); }},
      {Action::Home, [](BrowserPage& page, const Call&) { page.browser().home(); }},
      {Action::Root, [](BrowserPage& page, const Call&) { page.browser().open(luciole::Location::fromPath("/")); }},
      {Action::Refresh, [](BrowserPage& page, const Call&) { page.browser().refresh(); }},
      {Action::Stop, [](BrowserPage& page, const Call&) { page.browser().stop(); }},
      {Action::EditLocation, [](BrowserPage& page, const Call&) { page.m_locationBar.beginLocationEdit(); }},
      {Action::Search, [](BrowserPage& page, const Call&) { page.m_locationBar.openFilter(); }},
      {Action::Grid, [](BrowserPage& page, const Call&) { page.setViewMode(luciole::ViewMode::Grid); }},
      {Action::List, [](BrowserPage& page, const Call&) { page.setViewMode(luciole::ViewMode::List); }},
      {Action::Miller, [](BrowserPage& page, const Call&) { page.setViewMode(luciole::ViewMode::Miller); }},
      {Action::ShowHidden, [](BrowserPage& page, const Call&) { page.toggleHidden(); }},
      {Action::ShowSidebar, [](BrowserPage& page, const Call&) { page.m_sidebar.toggle(); }},
      {Action::ShowPreview, [](BrowserPage& page, const Call&) { page.togglePreview(); }},
      {Action::QuickPreview, [](BrowserPage& page, const Call&) { page.m_previewController.showQuickPreview(); }},
      {Action::OpenWith,
       [](BrowserPage& page, const Call& call) { page.m_applicationChooser.show(call.targets); }},
      {Action::SortName, sort},
      {Action::SortType, sort},
      {Action::SortSize, sort},
      {Action::SortModified, sort},
      {Action::SortAccessed, sort},
      {Action::SortCreated, sort},
      {Action::SortOwner, sort},
      {Action::SortGroup, sort},
      {Action::SortPermissions, sort},
      {Action::SortLocation, sort},

      {Action::Ascending, sort},
      {Action::Descending, sort},
      {Action::FoldersFirst, sort},
      {Action::HiddenLast, sort},
      {Action::ResetFolderView,
       [](BrowserPage& page, const Call& call) { page.m_folderView.reset(*call.request.folder); }},
      {Action::Open, [](BrowserPage& page, const Call& call) { page.openTargets(call.targets); }},
      {Action::ContainingFolder,
       [](BrowserPage& page, const Call& call) { page.showContaining(call.targets.front().location()); }},
      {Action::OriginalLocation,
       [](BrowserPage& page, const Call& call) { page.showContaining(*call.targets.front().trashInfo().original); }},
      {Action::ShowLinkTarget,
       [](BrowserPage& page, const Call& call) { page.showLinkTarget(call.targets.front().location()); }},
      {Action::Pin, [](BrowserPage& page, const Call& call) { page.pinTargets(call.targets); }},
      {Action::Unpin, [](BrowserPage& page, const Call& call) { page.unpinTargets(call.targets); }},
      {Action::CutFiles, fileClipboard},
      {Action::CopyFiles, fileClipboard},
      {Action::PasteFiles, [](BrowserPage& page, const Call& call) { page.m_transferController.paste(call.request); }},
      {Action::Rename,
       [](BrowserPage& page, const Call& call) {
         if (call.targets.size() == 1) {
           page.m_renameController.openFileRename(call.targets.front());
         } else {
           page.m_batchRename.open(call.targets);
         }
       }},
      {Action::CreateLink,
       [](BrowserPage& page, const Call& call) { page.m_createController.openLinkDialog(call.targets.front()); }},
      {Action::Trash, [](BrowserPage& page, const Call& call) { page.m_trashController.trashItems(call.targets); }},
      {Action::Restore, [](BrowserPage& page, const Call& call) { page.m_trashController.restore(call.targets); }},
      {Action::DeletePermanently,
       [](BrowserPage& page, const Call& call) { page.m_trashController.deleteItems(call.targets); }},
      {Action::Properties,
       [](BrowserPage& page, const Call& call) { page.m_propertiesController.show(call.targets); }},
      {Action::Compress,
       [](BrowserPage& page, const Call& call) { page.m_archiveController.compress(call.targets); }},
      {Action::ExtractHere,
       [](BrowserPage& page, const Call& call) {
         page.m_archiveController.extractHere(call.targets.front().location());
       }},
      {Action::ExtractTo,
       [](BrowserPage& page, const Call& call) { page.m_archiveController.extractTo(call.targets.front().location()); }},
      {Action::BrowseArchive,
       [](BrowserPage& page, const Call& call) { page.m_archiveController.browse(call.targets.front().location()); }},
      {Action::CreateFolder, createItem},
      {Action::CreateFile, createItem},
      {Action::NewDocument,
       [](BrowserPage& page, const Call&) {
         page.m_submenus.open(luciole::ActionRegistry::menuId(Action::NewDocument), false);
       }},
      {Action::PinCurrent,
       [](BrowserPage& page, const Call& call) {
         const auto& folder = *call.request.folder;
         page.m_sidebar.pinFolders(
             {{luciole::locationBreadcrumbs(folder).back().label, folder, {}}}, page.m_pins.bookmarks().size()
         );
       }},
      {Action::UnpinCurrent, [](BrowserPage& page, const Call& call) { page.m_pins.remove(*call.request.folder); }},
      {Action::ReloadPins, [](BrowserPage& page, const Call&) { page.m_pins.reload(); }},
      {Action::EmptyTrash, [](BrowserPage& page, const Call&) { page.m_trashController.openEmptyTrash(); }},
      {Action::SelectAll, [](BrowserPage& page, const Call&) { page.browser().model().selectAll(); }},
      {Action::InvertSelection, [](BrowserPage& page, const Call&) { (void)page.browser().model().invertSelection(); }},
      {Action::SelectPattern, [](BrowserPage& page, const Call& call) { page.openPatternSelection(call.request); }},
      {Action::ClearSelection,
       [](BrowserPage& page, const Call&) {
         page.m_list->cancelPointerGesture();
         page.browser().model().clearSelection();
       }},
      {Action::Settings, [](BrowserPage& page, const Call&) { page.m_settingsController.toggle(); }},
      {Action::About, [](BrowserPage& page, const Call&) { page.m_aboutController.open(); }},
      {Action::MenuBack, [](BrowserPage& page, const Call&) { page.m_menuController.openMenu(); }},
      {Action::SortMenu, [](BrowserPage& page, const Call&) { page.m_menuController.openSortMenu(); }},
      {Action::SelectionMenu, [](BrowserPage& page, const Call&) { page.m_menuController.openSelectionMenu(); }},
      {Action::CopyPath, pathClipboard},
      {Action::CopyUri, pathClipboard},
      {Action::Terminal, [](BrowserPage& page, const Call& call) { page.openTerminal(*call.request.folder); }},
      {Action::CopyTargetPath, pathClipboard},
      {Action::CopyTargetUri, pathClipboard},
      {Action::RunProgram, [](BrowserPage& page, const Call& call) { page.m_programs.run(call.request, false); }},
      {Action::RunInTerminal, [](BrowserPage& page, const Call& call) { page.m_programs.run(call.request, true); }},
      {Action::Scripts, [](BrowserPage& page, const Call&) {
        page.m_submenus.open(luciole::ActionRegistry::menuId(Action::Scripts), false);
      }},
      {Action::Undo, [](BrowserPage& page, const Call&) { (void)page.m_hub.replayHistory(page.m_workspaceId, false); }},
      {Action::Redo, [](BrowserPage& page, const Call&) { (void)page.m_hub.replayHistory(page.m_workspaceId, true); }},
      {Action::Operations, [](BrowserPage& page, const Call&) { page.m_operations.open(); }},
      {Action::TargetTerminal,
       [](BrowserPage& page, const Call& call) { page.openTerminal(call.targets.front().location()); }},
      {Action::KeyboardShortcuts, [](BrowserPage& page, const Call&) { page.m_shortcutsController.open(); }},
  });
  static_assert(luciole::actionTableInOrder(kHandlers));
  kHandlers[std::to_underlying(action)].run(*this, {.action = action, .request = request, .targets = targets});
}

void BrowserPage::copyFiles(Action action, const std::vector<luciole::DirectoryEntry>& targets) {
  luciole::FilePayload payload{.locations = {}, .cut = action == Action::CutFiles};
  for (const auto& target : targets) {
    payload.locations.push_back(target.location());
  }
  if (!m_clipboard.setFiles(std::move(payload))) {
    actionError("The file selection could not be copied to the clipboard.");
  }
}

void BrowserPage::copyPaths(
    Action action, const luciole::ActionRequest& request, const std::vector<luciole::DirectoryEntry>& targets
) {
  std::string text;
  const bool uri = action == Action::CopyUri || action == Action::CopyTargetUri;
  const auto append = [&](const luciole::Location& location) {
    if (!text.empty()) {
      text += '\n';
    }
    text += uri ? location.uri() : luciole::locationEditText(location);
  };
  if (action == Action::CopyPath || action == Action::CopyUri) {
    append(*request.folder);
  } else {
    for (const auto& target : targets) {
      append(target.location());
    }
  }
  if (text.size() > 1024 * 1024) {
    actionError("The selected paths exceed the clipboard text limit.");
  } else {
    m_clipboard.setClipboardText(std::move(text));
  }
}

void BrowserPage::openTerminal(const luciole::Location& target) {
  const auto context = browser().contextSerial();
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_terminal.open(target, [this, lifetime, context](std::string error) {
    if (!lifetime.expired() && context == browser().contextSerial() && !error.empty()) {
      actionError(std::move(error));
    }
  });
}

void BrowserPage::sortAction(Action action) {
  auto options = browser().model().sort();
  const auto field = action == Action::SortName ? luciole::SortField::Name
      : action == Action::SortType              ? luciole::SortField::Type
      : action == Action::SortSize              ? luciole::SortField::Size
      : action == Action::SortAccessed          ? luciole::SortField::Accessed
      : action == Action::SortCreated           ? luciole::SortField::Created
      : action == Action::SortOwner             ? luciole::SortField::Owner
      : action == Action::SortGroup             ? luciole::SortField::Group
      : action == Action::SortPermissions       ? luciole::SortField::Permissions
      : action == Action::SortLocation          ? luciole::SortField::Location

                                       : luciole::SortField::Modified;
  if (action == Action::Ascending || action == Action::Descending) {
    options.descending = action == Action::Descending;
  } else if (action == Action::FoldersFirst) {
    options.directoriesFirst = !options.directoriesFirst;
  } else if (action == Action::HiddenLast) {
    options.hiddenLast = !options.hiddenLast;
  } else if (options.field != field) {
    options.field = field;
    options.descending = false;
  }
  changeSort(options);
}

void BrowserPage::openTargets(const std::vector<luciole::DirectoryEntry>& targets) {
  if (choosing()) {
    if (targets.size() == 1
        && (targets.front().type == luciole::FileType::Directory
            || targets.front().type == luciole::FileType::Mountable)) {
      browser().activate(targets.front().id);
    } else {
      m_chooser.open(targets);
    }
    return;
  }
  if (m_miller.enabled()
      && targets.size() == 1
      && targets.front().type == luciole::FileType::Directory
      && !targets.front().symlink
      && browser().searchQuery().empty()) {
    (void)browser().model().select(targets.front().id);
    if (m_miller.enter()) {
      m_millerFocusPending = true;
    }
  } else if (
      targets.size() == 1
      && targets.front().location().isNative()
      && !targets.front().location().isTrash()
      && (targets.front().type == luciole::FileType::Regular || targets.front().symlink)
  ) {
    m_programs.open(requestFor(targets), [this, target = targets.front()] {
      if (target.type == luciole::FileType::Regular && !target.symlink) {
        m_launcher.open(target.location());
      } else {
        browser().activate(target.id);
      }
    });
  } else {
    std::vector<luciole::Location> files;
    for (const auto& target : targets) {
      if (target.type == luciole::FileType::Regular) {
        files.push_back(target.location());
      } else {
        browser().activate(target.id);
      }
    }
    if (!files.empty()) {
      m_launcher.open(files);
    }
  }
}

void BrowserPage::pinTargets(const std::vector<luciole::DirectoryEntry>& targets) {
  std::vector<luciole::Bookmark> pins;
  for (const auto& target : targets) {
    if (auto location = target.location(); !m_pins.contains(location)) {
      pins.push_back({target.displayName(), std::move(location), {}});
    }
  }
  m_sidebar.pinFolders(std::move(pins), m_pins.bookmarks().size());
}

void BrowserPage::unpinTargets(const std::vector<luciole::DirectoryEntry>& targets) {
  std::vector<luciole::Location> locations;
  locations.reserve(targets.size());
  for (const auto& target : targets) {
    locations.push_back(target.location());
  }
  m_pins.removeMany(locations);
}

void BrowserPage::openPatternSelection(const luciole::ActionRequest& request) {
  auto editor = std::make_unique<NameDialog>(
      dialogWidth(DialogWidth::Small), "Select by Pattern", "cursor-text", m_selectionPattern,
      "Use * for any text and ? for one character. Matches visible names, ignoring case.",
      luciole::selectionPatternProblem,
      [this, request](std::string pattern) {
        const auto state = actionState(Action::SelectPattern, request);
        if (!state.enabled) {
          m_patternEditor->showError(std::string(state.reason));
          return;
        }
        if (const auto error = browser().model().selectPattern(pattern)) {
          m_patternEditor->showError(*error);
          return;
        }
        m_selectionPattern = std::move(pattern);
        dismissPopup();
        dispatcher().setFocus(m_list->focusArea());
      },
      [this] { dismissPopup(); }, "Select", "Selecting…"
  );
  auto* control = editor.get();
  control->input()->setPlaceholder("Pattern");
  showDialog(std::move(editor), DialogWidth::Small);
  if (m_popupSurface) {
    m_activePopup.bind(ActivePopup::Kind::PatternSelection, [this] { m_patternEditor = nullptr; });
    m_patternEditor = control;
    m_popupDispatcher.setFocus(control->input()->inputArea());
    control->input()->selectAll();
  }
}

void BrowserPage::actionError(std::string message) {
  m_statusBar.setMessage(std::move(message));
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::setActionMessage(std::string message) { m_statusBar.setMessage(std::move(message)); }

void BrowserPage::revealOnReload(std::vector<luciole::Location> targets) { m_revealTargets = std::move(targets); }

void BrowserPage::leaveFolder() {
  if (m_miller.enabled() && m_miller.focused() > 0) {
    focusMiller(m_miller.focused() - 1);
    dispatcher().setFocus(m_list->focusArea());
    return;
  }
  if (auto current = browser().model().snapshot().location; browser().up()) {
    revealOnReload({std::move(*current)});
    focusFileList();
  }
}

void BrowserPage::restorePlaceFocus(luciole::Location location) { m_sidebar.focusPlace(std::move(location)); }

PageJobs& BrowserPage::jobs() { return m_jobs; }

void BrowserPage::requestSync() {
  m_browserDirty = true;
  m_surface->requestUpdate();
}

float BrowserPage::pointerX() const { return m_pointerX; }

float BrowserPage::pointerY() const { return m_pointerY; }

Node& BrowserPage::upButton() { return *m_up; }

VirtualGridView* BrowserPage::fileList() { return m_list; }

Flex& BrowserPage::filePane() { return *m_filePane; }

MillerView& BrowserPage::millerView() { return *m_millerView; }

luciole::ViewMode BrowserPage::viewMode() const { return m_viewMode; }

void BrowserPage::resetFileView() {
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  m_viewAnchor.reset();
  m_list->scrollView().requestScrollToOffset(0);
}

bool BrowserPage::targetsCurrent(const std::vector<luciole::DirectoryEntry>& targets) {
  const bool current = std::ranges::all_of(targets, [this](const auto& target) {
    const auto* entry = browser().model().entry(target.id);
    return entry != nullptr
        && entry->location() == target.location()
        && std::ranges::contains(browser().model().rows(), target.id);
  });
  if (!current) {
    actionError("The selection changed. Open the menu again.");
  }
  return current;
}

bool BrowserPage::requestCurrent(const luciole::ActionRequest& request, luciole::ActionScope scope) const {
  return luciole::ActionRegistry::current(actionContext(), request, scope);
}

bool BrowserPage::operationsBusy() const { return actionContext().operationsBusy; }

std::vector<std::string> BrowserPage::runningJobTitles() const {
  std::vector<std::string> titles;
  for (const auto& activity : m_hub.activities()) {
    if (!activity.finished) {
      titles.push_back(activity.title);
    }
  }
  return titles;
}

bool BrowserPage::visible() const { return m_visible; }

void BrowserPage::operationProgressTick() {
  m_batchRename.progressTick();
  m_propertiesController.progressTick();
  m_surface->requestUpdate();
}

void BrowserPage::showContaining(const luciole::Location& location) {
  const auto parent = location.parent();
  if (!parent) {
    return;
  }
  browser().open(*parent);
  revealItems({location}, false);
}

void BrowserPage::showLinkTarget(const luciole::Location& link) {
  const auto context = browser().contextSerial();
  const std::weak_ptr<int> lifetime = m_lifetime;
  luciole::runJob<luciole::Location>(
      [link] { return luciole::linkTarget(link); },
      [this, lifetime, context](luciole::JobResult<luciole::Location> result) {
        // Navigating away meanwhile cancels the jump.
        if (lifetime.expired() || context != browser().contextSerial()) {
          return;
        }
        if (result) {
          showContaining(*result);
        } else {
          actionError(result.error().message);
        }
      }
  );
}

void BrowserPage::applySettings(const luciole::BrowserSettings& settings, bool force) {
  m_terminal.setCommand(settings.terminalCommand);
  m_launcher.setTerminalCommand(settings.terminalCommand);
  const auto before = m_savedSettings;
  if (!force && before == settings) {
    return;
  }
  captureViewAnchor();
  if (m_list) {
    m_list->cancelPointerGesture();
  }
  dispatcher().cancelPointerCapture();
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  if (m_menuController.menu()) {
    closePopup();
  }
  if (m_settingsController.open() && (force || before.viewMode != settings.viewMode)) {
    browser().setDefaultViewMode(settings.viewMode);
    // A folder with its own Grid/List choice keeps it; only folders without one follow the new default.
    m_viewMode = browser().viewport().mode;
  }
  if (m_settingsController.open() && (force || before.gridIconSize != settings.gridIconSize)) {
    m_gridZoom = static_cast<std::size_t>(
        std::ranges::find(luciole::kGridIconSizes, settings.gridIconSize) - luciole::kGridIconSizes.begin()
    );
    m_zoomScroll = 0;
    browser().setDefaultIconSize(settings.gridIconSize);
  }
  if (m_settingsController.open() && (force || before.listIconSize != settings.listIconSize)) {
    m_listIconSize = settings.listIconSize;
    m_zoomScroll = 0;
  }
  if (m_settingsController.open() && (force || before.millerIconSize != settings.millerIconSize)) {
    m_millerIconSize = settings.millerIconSize;
    m_zoomScroll = 0;
  }
  if (m_settingsController.open() && (force || before.sort != settings.sort)) {
    applySort(settings.sort);
  }
  if (m_settingsController.open()
      && (force || before.showHidden != settings.showHidden)
      && browser().model().filter().showHidden != settings.showHidden) {
    browser().toggleHidden();
  }
  if (force || before.showBackups != settings.showBackups) {
    m_browser.setShowBackups(settings.showBackups);
    for (const auto& column : m_miller.columns()) {
      column->browser->setShowBackups(settings.showBackups);
    }
  }
  if (force || before.millerColumnWidth != settings.millerColumnWidth) {
    m_miller.defaultWidth = static_cast<float>(settings.millerColumnWidth);
    if (m_settingsController.open() && m_miller.enabled()) {
      m_miller.columns()[m_miller.focused()]->width = m_miller.defaultWidth;
      m_miller.columns()[m_miller.focused()]->fixedWidth = false;
    }
  }
  if (force || before.activationMode != settings.activationMode || before.typingMode != settings.typingMode) {
    m_fileKeys.clearTypeahead();
    m_activationMode = settings.activationMode;
    m_fileKeys.setTypingMode(settings.typingMode);
    m_browser.setSearchRecursive(settings.typingMode != luciole::TypingMode::Filter);
    for (const auto& column : m_miller.columns()) {
      column->browser->setSearchRecursive(settings.typingMode != luciole::TypingMode::Filter);
    }
    for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
      adapter->setActivationMode(m_activationMode);
    }
  }
  if (m_body && (force || before.backgroundOpacity != settings.backgroundOpacity)) {
    m_body->setFill(colorSpecFromRole(ColorRole::Surface, static_cast<float>(settings.backgroundOpacity)));
  }
  m_sidebar.applySettings(before, settings, force);
  if (m_settingsController.open()) {
    m_detailsHeader.applySettings(before, settings, force);
  }
  if (force || before.thumbnails != settings.thumbnails) {
    m_thumbnails.setOptions(settings.thumbnails);
  }
  if (before.millerShowParents != settings.millerShowParents && m_miller.enabled()) {
    m_restartMiller = true;
  }
  m_savedSettings = settings;
  m_browserDirty = true;
  m_surface->requestLayout();
}

void BrowserPage::cancelDrag() { m_dragDrop.cancelDrag(); }

void BrowserPage::captureViewAnchor() {
  if (m_list == nullptr) {
    return;
  }
  const auto first = m_list->firstVisibleIndex();
  const auto& rows = browser().model().rows();
  if (first && *first < rows.size()) {
    m_viewAnchor = rows[*first];
  }
}

void BrowserPage::sortChanged() {
  // An explicit reorder starts at the new beginning, rather than following the old first visible item.
  m_viewAnchor.reset();
  m_pendingHistoryViewport.reset();
  m_fileKeys.resetReveal();
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  m_sortScrollPending = true;
}

void BrowserPage::applySort(luciole::SortOptions options) {
  const auto before = browser().model().sort();
  m_browser.setDefaultSort(options);
  for (const auto& column : m_miller.columns()) {
    column->browser->setDefaultSort(options);
  }
  if (browser().model().sort() != before) {
    sortChanged();
  }
}

void BrowserPage::changeSort(luciole::SortOptions options) {
  if (options == browser().model().sort()) {
    return;
  }
  if (!m_folderView.saveSort(options)) {
    applySort(options);
    if (choosing()) {
      m_preferences.submit({
          {luciole::BrowserPreference::ChooserSortBy, options.field},
          {luciole::BrowserPreference::ChooserSortDescending, options.descending},
          {luciole::BrowserPreference::ChooserDirectoriesFirst, options.directoriesFirst},
          {luciole::BrowserPreference::ChooserHiddenLast, options.hiddenLast},
      });
    } else {
      m_preferences.saveSort(options);
    }
  }
  dispatcher().setFocus(m_list->focusArea());
}

void BrowserPage::showViewMode(luciole::ViewMode mode) {
  if (mode == m_viewMode) {
    return;
  }
  m_viewAnchor.reset();
  if (m_list != nullptr) {
    captureViewAnchor();
  }
  m_viewMode = mode;
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::setViewMode(luciole::ViewMode mode) {
  if (mode == m_viewMode) {
    return;
  }
  // Taking focus from a text field would end its edit and drop the path being typed.
  if (m_list != nullptr && !m_locationBar.textFocused()) {
    dispatcher().setFocus(m_list->focusArea());
  }
  if (m_folderView.saveViewMode(mode)) {
    return;
  }
  showViewMode(mode);
  if (choosing()) {
    m_preferences.submit({{luciole::BrowserPreference::ChooserViewMode, mode}});
  } else {
    m_preferences.save(mode);
  }
  browser().setDefaultViewMode(m_viewMode);
  // Leaving Columns shows the current folder's own Grid/List choice, if it has one.
  showViewMode(browser().viewport().mode);
}

void BrowserPage::applyViewMode() {
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  if (m_miller.enabled()) {
    m_regularList->setVisible(false);
    m_regularList->setParticipatesInLayout(false);
    m_regularList->setAdapter(nullptr);
    m_millerView->setVisible(true);
    m_millerView->setParticipatesInLayout(true);
    m_appliedViewMode = m_viewMode;
    return;
  }
  m_list = m_regularList;
  m_millerView->setVisible(false);
  m_millerView->setParticipatesInLayout(false);
  const bool grid = m_viewMode == luciole::ViewMode::Grid;
  m_list->setAdapter(grid ? &m_gridAdapter : &m_listAdapter);
  m_list->setColumns(grid ? 0 : 1);
  // Details rows reserve the lane so the column header stays aligned; grid cells only yield it while scrolling.
  m_list->scrollView().setReserveScrollbarGutter(!grid);
  m_list->setMinCellWidth(luciole::gridMetrics(gridIconSize(), m_savedSettings.presentation, false).minWidth);
  m_list->setCellHeight(gridHeight());
  m_list->setColumnGap(grid ? luciole::gridMetrics(gridIconSize(), m_savedSettings.presentation, false).gap : 0.0F);
  m_list->setRowGap(grid ? luciole::gridMetrics(gridIconSize(), m_savedSettings.presentation, false).gap : 2.0F);
  m_appliedViewMode = m_viewMode;
}

float BrowserPage::gridHeight() const {
  const auto& location = browser().model().snapshot().location;
  const bool context = !browser().searchQuery().empty() || (location && location->isTrash());
  return m_viewMode == luciole::ViewMode::Grid
      ? luciole::gridMetrics(gridIconSize(), m_savedSettings.presentation, context).height
      : luciole::fileRowHeight(m_savedSettings.presentation, context, rowIconSize());
}

float BrowserPage::gridIconSize() const { return static_cast<float>(luciole::kGridIconSizes[m_gridZoom]); }
float BrowserPage::rowIconSize() const {
  return m_viewMode == luciole::ViewMode::Miller
      ? luciole::millerRowIconSize(m_savedSettings.presentation, m_millerIconSize)
      : luciole::fileRowIconSize(m_listIconSize);
}

void BrowserPage::changeGridZoom(int requested) {
  const auto zoom =
      static_cast<std::size_t>(std::clamp(requested, 0, static_cast<int>(luciole::kGridIconSizes.size()) - 1));
  if (zoom == m_gridZoom) {
    return;
  }
  captureViewAnchor();
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  m_gridZoom = zoom;
  if (choosing()) {
    m_preferences.submit({{luciole::BrowserPreference::ChooserGridIconSize, luciole::kGridIconSizes[m_gridZoom]}});
  } else {
    m_preferences.saveGridIconSize(luciole::kGridIconSizes[m_gridZoom]);
  }
  browser().setDefaultIconSize(static_cast<int>(gridIconSize()));
  m_browserDirty = true;
  m_surface->requestUpdate();
}

void BrowserPage::changeRowIconSize(int size) {
  if (size == (m_viewMode == luciole::ViewMode::Miller ? m_millerIconSize : m_listIconSize)) {
    return;
  }
  captureViewAnchor();
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  if (m_viewMode == luciole::ViewMode::Miller) {
    m_millerIconSize = size;
    m_preferences.saveMillerIconSize(size);
  } else {
    m_listIconSize = size;
    if (choosing()) {
      m_preferences.submit({{luciole::BrowserPreference::ChooserListIconSize, size}});
    } else {
      m_preferences.saveListIconSize(size);
    }
  }
  m_browserDirty = true;
  m_surface->requestUpdate();
}

int BrowserPage::zoomStep() const {
  if (m_viewMode == luciole::ViewMode::Grid) {
    return static_cast<int>(m_gridZoom);
  }
  const auto size = static_cast<int>(rowIconSize());
  return static_cast<int>(std::ranges::find(luciole::kRowIconSizes, size) - luciole::kRowIconSizes.begin());
}

void BrowserPage::changeZoomStep(int requested) {
  if (m_viewMode == luciole::ViewMode::Grid) {
    changeGridZoom(requested);
    return;
  }
  const auto step = std::clamp(requested, 0, static_cast<int>(luciole::kRowIconSizes.size()) - 1);
  // Staying on the default size keeps the view on automatic sizing.
  if (step != zoomStep()) {
    changeRowIconSize(luciole::kRowIconSizes[static_cast<std::size_t>(step)]);
  }
}

void BrowserPage::resetZoom() {
  if (m_viewMode == luciole::ViewMode::Grid) {
    changeGridZoom(
        static_cast<int>(
            std::ranges::find(luciole::kGridIconSizes, luciole::kDefaultGridIconSize) - luciole::kGridIconSizes.begin()
        )
    );
    return;
  }
  changeRowIconSize(0);
}

bool BrowserPage::zoomFromKey(const KeyboardEvent& event) {
  if (m_list == nullptr) {
    return false;
  }
  int zoom = zoomStep();
  if (luciole::keybindMatches(luciole::KeybindCommand::ZoomIn, event.sym, event.modifiers)) {
    ++zoom;
  } else if (luciole::keybindMatches(luciole::KeybindCommand::ZoomOut, event.sym, event.modifiers)) {
    --zoom;
  } else if (luciole::keybindMatches(luciole::KeybindCommand::ZoomReset, event.sym, event.modifiers)) {
    m_zoomScroll = 0;
    resetZoom();
    return true;
  } else {
    return false;
  }
  m_zoomScroll = 0;
  changeZoomStep(zoom);
  return true;
}

bool BrowserPage::zoomFromScroll(const PointerEvent& event) {
  // Columns zoom from anywhere over the columns, not only the focused one.
  Node* area = m_viewMode == luciole::ViewMode::Miller ? static_cast<Node*>(m_millerView) : m_list;
  if (area == nullptr
      || !area->visible()
      || m_connection.keyboardModifiers() != KeyMod::Ctrl
      || event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    m_zoomScroll = 0;
    return false;
  }
  float localX = 0, localY = 0;
  if (!Node::mapFromScene(area, static_cast<float>(event.sx), static_cast<float>(event.sy), localX, localY)
      || localX < 0
      || localY < 0
      || localX >= area->width()
      || localY >= area->height()) {
    return false;
  }
  const double steps = event.axisValue120 != 0 ? event.axisValue120 / 120.0
      : event.axisDiscrete != 0                ? static_cast<double>(event.axisDiscrete)
                                               : event.axisValue / 15.0;
  if (!std::isfinite(steps)) {
    return true;
  }
  const auto limit = static_cast<double>(luciole::kGridIconSizes.size());
  m_zoomScroll = std::clamp(m_zoomScroll - steps, -limit, limit);
  const auto delta = static_cast<int>(m_zoomScroll);
  m_zoomScroll -= delta;
  changeZoomStep(zoomStep() + delta);
  return true;
}

void BrowserPage::syncBrowser() {
  m_propertiesController.updateApply();
  m_fileKeys.syncContext();
  if (m_visitSerial != browser().visitSerial()) {
    m_visitSerial = browser().visitSerial();
    const auto& viewport = browser().viewport();
    m_viewMode = m_miller.enabled() ? luciole::ViewMode::Miller : viewport.mode;
    m_gridZoom = static_cast<std::size_t>(
        std::ranges::find(luciole::kGridIconSizes, viewport.iconSize) - luciole::kGridIconSizes.begin()
    );
    m_detailsHeader.setOffset(0);
    m_fileKeys.resetReveal();
  }
  m_fileKeys.applyPendingCursor();
  m_browser.setExpandableFolders(
      m_savedSettings.expandableFolders && m_viewMode == luciole::ViewMode::List && !m_miller.enabled()
  );
  const auto& model = browser().model();
  const auto& snapshot = model.snapshot();
  if (m_locationBar.syncLocation(snapshot.location)) {
    m_siblingCompletion.cancel();
    resetFileView();
  }
  if (auto restored = browser().takeRestoredViewport()) {
    m_pendingHistoryViewport = restored;
    m_viewAnchor.reset();
  }
  m_locationBar.sync();
  m_list->setCellHeight(gridHeight());
  m_list->setMinCellWidth(luciole::gridMetrics(gridIconSize(), m_savedSettings.presentation, false).minWidth);
  m_gridAdapter.setGridIconSize(gridIconSize());
  m_listAdapter.setRowIconSize(m_listIconSize);
  for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
    adapter->setPresentation(m_savedSettings.presentation, snapshot.metadata.filesystem.readOnly);
  }
  const bool gridMode = m_viewMode == luciole::ViewMode::Grid;
  const auto metrics = luciole::gridMetrics(gridIconSize(), m_savedSettings.presentation, false);
  m_list->setColumnGap(gridMode ? metrics.gap : 0.0F);
  m_list->setRowGap(gridMode ? metrics.gap : m_viewMode == luciole::ViewMode::Miller ? 0.0F : 2.0F);
  const auto& sort = model.sort();
  m_detailsHeader.sync(sort);
  const auto field = luciole::sortFieldLabel(sort.field);
  const char* direction = (sort.field == luciole::SortField::Modified
                           || sort.field == luciole::SortField::Accessed
                           || sort.field == luciole::SortField::Created)
      ? (sort.descending ? "newest first" : "oldest first")
      : sort.field == luciole::SortField::Size ? (sort.descending ? "largest first" : "smallest first")
                                               : (sort.descending ? "Z to A" : "A to Z");
  m_menuButton->setTooltip(
      std::format(
          "View options: {}, {}{}{}{}", field, direction, sort.directoriesFirst ? " · Folders first" : "",
          sort.hiddenLast ? " · Hidden last" : "", m_folderView.sortCustomized() ? " · This folder" : ""
      )
  );
  const bool grid = m_viewMode == luciole::ViewMode::Grid;
  m_viewSwitch->setSelectedIndex(grid ? 0 : m_viewMode == luciole::ViewMode::List ? 1 : 2);
  const auto request = actionRequest(false);
  for (const auto& [button, action] : std::array{
           std::pair{m_back, Action::Back}, std::pair{m_forward, Action::Forward}, std::pair{m_up, Action::Up},
           std::pair{m_newFolderButton, Action::CreateFolder}
       }) {
    const auto state = actionState(action, request);
    button->setEnabled(state.enabled);
    const auto& definition = luciole::ActionRegistry::definition(action);
    const auto shortcut = luciole::actionShortcutLabel(action);
    std::string tooltip(definition.label);
    if (!shortcut.empty()) {
      tooltip += std::format(" ({})", shortcut);
    }
    if (!state.enabled) {
      tooltip += "\n" + std::string(state.reason);
    }
    button->setTooltip(std::move(tooltip));
  }
  m_closePaneButton->setTooltip(luciole::shortcutTooltip("Close pane", luciole::KeybindCommand::ClosePane));
  m_viewSwitch->setOptionTooltip(0, luciole::shortcutTooltip("Grid view", Action::Grid));
  m_viewSwitch->setOptionTooltip(1, luciole::shortcutTooltip("List view", Action::List));
  m_viewSwitch->setOptionTooltip(2, luciole::shortcutTooltip("Column view", Action::Miller));
  const bool showEmptyTrash = !choosing() && snapshot.location && snapshot.location->isTrash();
  m_emptyTrashButton->setVisible(showEmptyTrash);
  m_emptyTrashButton->setParticipatesInLayout(showEmptyTrash);
  const auto emptyTrashState = actionState(Action::EmptyTrash, request);
  const std::string emptyTrashLabel(luciole::ActionRegistry::definition(Action::EmptyTrash).label);
  m_emptyTrashButton->setEnabled(emptyTrashState.enabled);
  m_emptyTrashButton->setTooltip(
      emptyTrashState.enabled ? emptyTrashLabel : emptyTrashLabel + "\n" + std::string(emptyTrashState.reason)
  );
  m_sidebar.sync(snapshot.location);
  m_statusBar.sync(m_miller.enabled());
  const bool empty = model.rows().empty();
  if (!m_miller.enabled()) {
    m_list->setVisible(!empty);
    m_list->setParticipatesInLayout(!empty);
  }
  std::optional<std::size_t> focus;
  if (model.selection().focused) {
    const auto found = std::ranges::find(model.rows(), *model.selection().focused);
    if (found != model.rows().end()) {
      focus = static_cast<std::size_t>(std::distance(model.rows().begin(), found));
    }
  }
  m_list->setSelectedIndex(focus, false);
  m_list->notifyDataChanged();
  if (const auto reveal = m_fileKeys.takeReveal(); reveal && model.selection().focused == reveal) {
    const auto found = std::ranges::find(model.rows(), *reveal);
    if (found != model.rows().end()) {
      m_list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
    }
  }
  m_batchRename.revealRenamed(*m_list);
  revealPendingTargets();
}

void BrowserPage::revealPendingTargets() {
  const auto& model = browser().model();
  const auto& snapshot = model.snapshot();
  if (!m_revealTargets.empty() && snapshot.status != luciole::DirectoryStatus::Loading) {
    const auto targets = std::exchange(m_revealTargets, {});
    const bool properties = std::exchange(m_revealProperties, false);
    std::vector<luciole::EntryId> ids;
    for (const auto id : model.rows()) {
      if (const auto* entry = model.entry(id); entry && std::ranges::contains(targets, entry->location())) {
        ids.push_back(id);
      }
    }
    if (!ids.empty() && browser().model().replaceSelection(ids)) {
      const auto found = std::ranges::find(model.rows(), ids.front());
      m_list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
      if (properties) {
        const std::weak_ptr<int> lifetime = m_lifetime;
        DeferredCall::callLater([this, lifetime] {
          if (!lifetime.expired()) {
            dispatchAction(Action::Properties);
          }
        });
      }
    }
  }
}

std::uint32_t BrowserPage::resizeEdges(float x, float y) const {
  const bool left = x<6, right = x> static_cast<float>(m_surface->width()) - 6;
  const bool top = y<6, bottom = y> static_cast<float>(m_surface->height()) - 6;
  if (top) {
    return left ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT
        : right ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT
                : XDG_TOPLEVEL_RESIZE_EDGE_TOP;
  }
  if (bottom) {
    return left ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT
        : right ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
                : XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
  }
  return left ? XDG_TOPLEVEL_RESIZE_EDGE_LEFT : right ? XDG_TOPLEVEL_RESIZE_EDGE_RIGHT : XDG_TOPLEVEL_RESIZE_EDGE_NONE;
}

void BrowserPage::prepareFrame(bool, bool layout) {
  if (!m_renderer.makeCurrent(m_surface->renderTarget())) {
    return;
  }
  auto& renderer = m_surface->renderTarget().renderer();
  UiPhaseScope update(UiPhase::Update);
  if (m_root == nullptr) {
    layout = true;
    UiPhaseScope phase(UiPhase::Layout);
    buildScene();
  }
  if (std::exchange(m_restartMiller, false) && m_miller.enabled()) {
    m_browser.duplicateFrom(m_miller.active());
    m_miller.clear();
    m_list = m_regularList;
  }
  if (m_viewMode == luciole::ViewMode::Miller && !m_miller.enabled()) {
    m_miller.start(m_browser, m_savedSettings.millerShowParents);
    if (m_miller.enabled()) {
      m_browser.stop();
    }
  } else if (m_viewMode != luciole::ViewMode::Miller && m_miller.enabled()) {
    m_browser.duplicateFrom(m_miller.active());
    m_browser.setDefaultViewMode(m_viewMode);
    m_miller.clear();
    m_list = m_regularList;
    m_millerFocusPending = true;
  }
  if (m_visible) {
    m_millerView->loadVisible();
  }
  m_miller.synchronize();
  if (m_browserDirty || m_appliedViewMode != m_viewMode) {
    const bool restoreFocus = m_miller.enabled()
        && m_lastBrowser != &browser()
        && m_list
        && dispatcher().focusedArea() == m_list->focusArea();
    UiPhaseScope phase(UiPhase::Layout);
    m_millerView->activationMode = m_activationMode;
    m_millerView->presentation = m_savedSettings.presentation;
    m_millerView->iconSize = m_millerIconSize;
    m_millerView->synchronize();
    if (auto* grid = m_millerView->focusedView()) {
      m_list = grid;
    }
    if (m_lastBrowser != &browser()) {
      m_lastBrowser = &browser();
      m_visitSerial = 0;
      m_fileKeys.resetReveal();
      m_pendingHistoryViewport.reset();
      m_thumbnailContext = 0;
      m_fileKeys.clearTypeahead();
    }
    m_millerFocusPending = m_millerFocusPending || restoreFocus;
  }
  if (std::exchange(m_millerFocusPending, false)) {
    m_millerView->revealFocused();
    focusFileList();
  }
  if (m_miller.enabled() && m_browserDirty) {
    for (const auto& column : m_miller.columns()) {
      m_thumbnails.synchronizeSources(column->browser->model().snapshot());
    }
  }
  const auto& thumbnailSnapshot = browser().model().snapshot();
  if (m_thumbnailContext != browser().contextSerial() || thumbnailSnapshot.generation != m_thumbnailRevision) {
    m_thumbnailRevision = thumbnailSnapshot.generation;
    m_thumbnailContext = browser().contextSerial();
    m_thumbnails.synchronizeSources(thumbnailSnapshot);
  }
  if (m_visible && active()) {
    if (const auto pending = std::exchange(m_pendingOperation, {}); pending && m_jobs.find(pending->first)) {
      if (pending->second) {
        m_transferController.retry(pending->first);
      }
    }
  }
  m_operations.onFrame();
  m_settingsController.onFrame();
  m_sidebar.onFrame();
  m_mountController.onFrame();
  m_archiveController.onFrame();
  m_transferController.onFrame();
  m_transferController.syncConflict();
  m_trashController.onFrame();
  m_createController.onFrame();
  m_propertiesController.onFrame();
  m_batchRename.onFrame();
  m_renameController.onFrame();
  if (m_browserDirty) {
    m_browserDirty = false;
    m_renameController.syncPlaceEditors();
    syncBrowser();
    layout = true;
  }
  if (m_locationBar.onFrame()) {
    layout = true;
  }
  if (layout) {
    layoutFrame(renderer);
  }
}

void BrowserPage::layoutFrame(Renderer& renderer) {
  UiPhaseScope phase(UiPhase::Layout);
  m_statusBar.layout(m_pageBounds.width);
  // Places changed while this frame was prepared, after the window arranged the sidebar: draw them in another frame.
  const bool placesLate = m_sidebar.invalidate();
  m_dragDrop.applyScroll();
  m_sidebar.layout();
  m_locationBar.prepareLayout();
  if (m_appliedViewMode != m_viewMode) {
    applyViewMode();
  }
  if (std::exchange(m_sortScrollPending, false)) {
    // Apply after preference notifications and view switches can capture another anchor.
    m_viewAnchor.reset();
    m_pendingHistoryViewport.reset();
    m_list->setSelectedIndex(m_list->selectedIndex(), false);
    m_list->scrollView().requestScrollToOffset(0);
  }
  if (m_viewAnchor) {
    const auto& rows = browser().model().rows();
    const auto found = std::ranges::find(rows, *m_viewAnchor);
    if (found != rows.end()) {
      m_list->scrollToIndex(static_cast<std::size_t>(std::distance(rows.begin(), found)), true);
    }
    m_viewAnchor.reset();
  }
  if (m_pendingHistoryViewport) {
    const auto viewport = *m_pendingHistoryViewport;
    m_pendingHistoryViewport.reset();
    m_detailsHeader.setOffset(viewport.horizontal);
    const auto& rows = browser().model().rows();
    const auto found = viewport.first ? std::ranges::find(rows, *viewport.first) : rows.end();
    if (found != rows.end()) {
      m_list->scrollToIndex(static_cast<std::size_t>(found - rows.begin()), true, viewport.withinRow);
    } else {
      m_list->scrollView().requestScrollToOffset(viewport.vertical);
    }
  }
  const bool clientHeader = false;
  const bool columnOrderChanged = m_detailsHeader.applyOrder();
  m_titlebar->setVisible(clientHeader);
  m_titlebar->setParticipatesInLayout(clientHeader);
  const bool details = m_viewMode == luciole::ViewMode::List && !browser().model().rows().empty();
  // List headers own their vertical insets so their labels and resize
  // markers are centered between the pane edge and the header rule.
  m_paneContentFrame->setPadding(
      details && !m_paneHeader->visible() ? 0.0F : Style::radiusMd, luciole::kSideListPadding, 0.0F,
      luciole::kSideListPadding
  );
  m_filePane->setPadding(details ? 0.0F : Style::spaceSm, Style::spaceSm, Style::spaceSm, Style::spaceSm);
  m_root->arrange(
      renderer, LayoutRect{0, 0, static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height())}
  );
  // A translucent overlay (background_opacity below 1) would otherwise show the panes through it.
  m_body->setVisible(!overlay());
  m_body->arrange(renderer, m_pageBounds);
  m_archiveController.layout(renderer, m_pageBounds);
  if (m_detailsHeader.layoutColumns(m_miller.enabled())) {
    m_root->arrange(renderer, {0, 0, static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height())});
    m_body->arrange(renderer, m_pageBounds);
    m_detailsHeader.layoutHeader(renderer);
  }
  m_statusBar.arrange(renderer, m_pageBounds.width, m_pageBounds.height);
  const bool columnHoverDirty = m_detailsHeader.takeHoverDirty();
  if (columnOrderChanged || columnHoverDirty) {
    dispatcher().syncPointerHover();
  }
  m_detailsHeader.layoutDrag(renderer);
  m_dragDrop.refreshDrop();
  m_sidebar.layoutResizeLine(renderer);
  m_dragDrop.layout(renderer);
  m_locationBar.layoutCompletions(renderer);
  m_thumbnails.endFrame();
  m_sidebar.restoreFocus(m_popupSurface != nullptr || m_archiveController.open() || !m_active);
  m_fileKeys.notifyStateChanged();
  if (placesLate) {
    m_surface->requestUpdate();
  }
}

void BrowserPage::buildScene() {
  uiAssertNotRendering("Window::buildScene");
  m_titlebar = nullptr;
  // Every page root spans the whole window (pages share one surface and one dispatcher), so the root itself must
  // not be a hit target: otherwise the active page's root would sit over the other pane and swallow its hover,
  // presses and tooltips. Only its children are hit-tested.
  class PageRoot final : public Flex {
  protected:
    bool containsLocalPoint(float, float, bool) const override { return false; }
  };
  auto root = std::make_unique<PageRoot>();
  root->setDirection(FlexDirection::Vertical);
  root->setAlign(FlexAlign::Stretch);
  root->setFillWidth(true);
  root->setFillHeight(true);
  root->addChild(buildTitlebar());
  root->addChild(buildToolbar());
  root->addChild(
      ui::column(
          {.out = &m_body,
           .align = FlexAlign::Stretch,
           .fill = colorSpecFromRole(ColorRole::Surface, static_cast<float>(m_savedSettings.backgroundOpacity)),
           .clipChildren = true,
           .participatesInLayout = false},
          ui::column(
              {.out = &m_paneContentFrame,
               .align = FlexAlign::Stretch,
               .flexGrow = 1.0F,
               .configure =
                   [](Flex& frame) {
                     frame.setPadding(Style::radiusMd, luciole::kSideListPadding, 0.0F, luciole::kSideListPadding);
                   }},
              ui::row(
                  {.out = &m_paneHeader,
                   .align = FlexAlign::Center,
                   .gap = Style::spaceXs,
                   .paddingH = Style::spaceSm,
                   .height = 32.0F,
                   .visible = false,
                   .participatesInLayout = false},
                  ui::label(
                      {.out = &m_paneTitle,
                       .fontSize = Style::fontSizeCaption,
                       .maxLines = 1,
                       .flexGrow = 1,
                       .configure = [](Label& label) { label.setUseMarkup(false); }}
                  ),
                  ui::button(
                      {.out = &m_closePaneButton,
                       .glyph = "x",
                       .glyphSize = Style::baseGlyphSize,
                       .controlHeight = 24.0F,
                       .variant = ButtonVariant::Ghost,
                       .tooltip = luciole::shortcutTooltip("Close pane", luciole::KeybindCommand::ClosePane),
                       .followsCloseButtons = true,
                       .padding = Style::spaceXs,
                       .width = 24.0F,
                       .onClick =
                           [this] {
                             if (m_workspaceRequest) {
                               m_workspaceRequest(WorkspaceCommand::ClosePane, std::nullopt);
                             }
                           }}
                  )
              ),
              ui::row(
                  {.align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F}, m_sidebar.buildPlaces(),
                  m_sidebar.buildResizeHandle(), buildFilePane()
              )
          ),
          m_statusBar.build()
      )
  );
  m_detachedRoot = std::move(root);
  m_root = m_detachedRoot.get();
  m_sidebar.detach();
  m_detailsHeader.attach(*m_root);
  m_dragDrop.buildOverlay(m_animations);
  m_locationBar.buildCompletion(*m_toolbar);
  m_root->setAnimationManager(&m_animations);
}

std::unique_ptr<Flex> BrowserPage::buildTitlebar() {
  auto title = ui::inputArea(
      {
          .acceptedButtons = InputArea::buttonMask(BTN_LEFT) | InputArea::buttonMask(BTN_RIGHT),
          .flexGrow = 1.0F,
          .onLeave = [this] { m_titlebarMove.cancel(); },
          .onMotion =
              [this](const InputArea::PointerData& data) {
                if (const auto serial = m_titlebarMove.motion(data.localX, data.localY)) {
                  m_surface->beginMove(*serial);
                }
              },
          // InputArea reports releases through onPress too.
          .onPress =
              [this](const InputArea::PointerData& data) {
                if (!data.pressed) {
                  m_titlebarMove.cancel();
                  return;
                }
                if (data.button == BTN_LEFT) {
                  m_titlebarMove.press(data.serial, data.localX, data.localY);
                } else {
                  m_surface->showWindowMenu(data.serial, static_cast<int>(data.sceneX), static_cast<int>(data.sceneY));
                }
              },
      },
      ui::label({.text = "Luciole", .fontSize = Style::fontSizeTitle, .fontWeight = FontWeight::SemiBold})
  );
  return ui::row(
      {.out = &m_titlebar,
       .align = FlexAlign::Center,
       .gap = Style::spaceSm,
       .padding = Style::spaceSm,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
       .height = 48.0F},
      std::move(title),
      ui::button(
          {.glyph = "minus",
           .glyphSize = Style::baseGlyphSize,
           .variant = ButtonVariant::Ghost,
           .onClick = [this] { m_surface->minimize(); }}
      ),
      ui::button(
          {.glyph = "square",
           .glyphSize = Style::baseGlyphSize,
           .variant = ButtonVariant::Ghost,
           .onClick = [this] { m_surface->toggleMaximized(); }}
      ),
      ui::button({.glyph = "x", .glyphSize = Style::baseGlyphSize, .variant = ButtonVariant::Ghost, .onClick = [this] {
                    m_surface->close();
                  }})
  );
}

std::unique_ptr<Flex> BrowserPage::buildToolbar() {
  return ui::row(
      {.out = &m_toolbar, .align = FlexAlign::Center, .gap = Style::spaceSm, .padding = Style::spaceSm},
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs}, m_sidebar.buildToggle(),
          ui::button(
              {.out = &m_back,
               .glyph = "arrow-left",
               .glyphSize = Style::baseGlyphSize,
               .variant = ButtonVariant::Ghost,
               .tooltip = luciole::shortcutTooltip("Back", Action::Back),
               .onClick = [this] { dispatchAction(Action::Back); }}
          ),
          ui::button(
              {.out = &m_forward,
               .glyph = "arrow-right",
               .glyphSize = Style::baseGlyphSize,
               .variant = ButtonVariant::Ghost,
               .tooltip = luciole::shortcutTooltip("Forward", Action::Forward),
               .onClick = [this] { dispatchAction(Action::Forward); }}
          ),
          ui::button(
              {.out = &m_up,
               .glyph = "arrow-up",
               .glyphSize = Style::baseGlyphSize,
               .variant = ButtonVariant::Ghost,
               .tooltip = luciole::shortcutTooltip("Parent folder", Action::Up),
               .onClick = [this] { dispatchAction(Action::Up); }}
          )
      ),
      m_locationBar.buildLocationArea(), m_locationBar.buildSearchArea(),
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs},
          ui::button(
              {.out = &m_newFolderButton,
               .glyph = "folder-plus",
               .glyphSize = Style::baseGlyphSize,
               .variant = ButtonVariant::Ghost,
               .tooltip = luciole::shortcutTooltip("New Folder…", Action::CreateFolder),
               .visible = choosing(),
               .participatesInLayout = choosing(),
               .onClick = [this] { dispatchAction(Action::CreateFolder); }}
          ),
          ui::button(
              {.out = &m_emptyTrashButton,
               .glyph = "trash-x",
               .glyphSize = Style::baseGlyphSize,
               .variant = ButtonVariant::Ghost,
               .visible = false,
               .participatesInLayout = false,
               .onClick = [this] { dispatchAction(Action::EmptyTrash); }}
          ),
          ui::button(
              {.out = &m_sortButton,
               .glyph = "arrows-sort",
               .glyphSize = Style::baseGlyphSize,
               .variant = ButtonVariant::Ghost,
               .tooltip = "Sort files",
               .onClick = [this] { m_menuController.openSortMenu(sceneRect(*m_sortButton)); }}
          ),
          ui::segmented(
              {.out = &m_viewSwitch,
               .options =
                   [this] {
                     std::vector<ui::SegmentedOption> views{
                         {.label = "",
                          .glyph = "layout-grid",
                          .tooltip = luciole::shortcutTooltip("Grid view", Action::Grid)},
                         {.label = "", .glyph = "list", .tooltip = luciole::shortcutTooltip("List view", Action::List)}
                     };
                     // Column view is unavailable while choosing files.
                     if (!choosing()) {
                       views.push_back(
                           {.label = "",
                            .glyph = "columns-3",
                            .tooltip = luciole::shortcutTooltip("Column view", Action::Miller)}
                       );
                     }
                     return views;
                   }(),
               .selectedIndex = m_viewMode == luciole::ViewMode::Grid ? 0U
                   : m_viewMode == luciole::ViewMode::List            ? 1U
                                                                      : 2U,
               // The segments are glyph-only, so the font size is the glyph size.
               .fontSize = Style::baseGlyphSize,
               .compact = true,
               .onChange =
                   [this](std::size_t index) {
                     dispatchAction(index == 0 ? Action::Grid : index == 1 ? Action::List : Action::Miller);
                   },
               // Same pill colour as the address and search fields; the default track is the chrome colour.
               .configure = [](Segmented& view) { view.setSurfaceRole(ColorRole::SurfaceContainerHigh); }}
          ),
          m_operations.buildButton(),
          ui::button(
              {.out = &m_menuButton,
               .glyph = "dots-vertical",
               .glyphSize = Style::baseGlyphSize,
               .variant = ButtonVariant::Ghost,
               .visible = !choosing(),
               .onClick = [this] { m_menuController.openMenu(); }}
          )
      )
  );
}

std::unique_ptr<Flex> BrowserPage::buildFilePane() {
  auto content = ui::column(
      {.out = &m_filePane,
       .align = FlexAlign::Stretch,
       .gap = Style::spaceXs,
       .padding = Style::spaceSm,
       .flexGrow = 1.0F},
      m_detailsHeader.buildHeader(), m_statusBar.buildNotice(),
      ui::virtualGridView(
          {.out = &m_list,
           .columns = 1,
           .cellHeight = 36.0F,
           .squareCells = false,
           .columnGap = 0.0F,
           .rowGap = 2.0F,
           .adapter = &m_listAdapter,
           .flexGrow = 1.0F,
           .configure = [this](VirtualGridView& list) {
             list.scrollView().setReserveScrollbarGutter(true);
             list.focusArea()->setRetainsFocusOnPointerRelease(true);
             list.focusArea()->setTextInputClient(&m_fileKeys);
             list.focusArea()->setOnFocusGain([this] {
               m_browserDirty = true;
               m_surface->requestUpdate();
             });
             list.focusArea()->setOnFocusLoss([this] {
               m_browserDirty = true;
               m_surface->requestUpdate();
             });
           }}
      )
  );
  m_regularList = m_list;
  auto miller = std::make_unique<MillerView>(m_miller, m_icons, m_thumbnails);
  m_millerView = miller.get();
  miller->setVisible(false);
  miller->setParticipatesInLayout(false);
  miller->modifiers = [this] { return m_connection.keyboardModifiers(); };
  miller->keyboardFocus = [this] { return m_fileKeys.keyboardFocus() && m_list && m_list->focusArea()->focused(); };
  miller->cut = [this](const luciole::Location& location) { return m_clipboard.isCut(location); };
  miller->focus = [this](std::size_t index) { focusMiller(index); };
  miller->activate = [this](luciole::EntryId id) {
    auto request = actionRequest(false);
    if (const auto* entry = browser().model().entry(id)) {
      request.targets.push_back({id, entry->location()});
    }
    dispatchAction(m_connection.keyboardModifiers() == KeyMod::Alt ? Action::Properties : Action::Open, request);
  };
  miller->context = [this](std::optional<luciole::EntryId> entry, float x, float y) {
    m_menuController.openFileMenu(entry, x, y);
  };
  miller->drag = [this](const std::vector<luciole::EntryId>& ids) { m_dragDrop.startFileDrag(ids); };
  miller->cancelDrag = [this] { m_dragDrop.cancelLocalDrag(); };
  miller->resized = [this](int width) {
    m_miller.defaultWidth = static_cast<float>(width);
    m_preferences.submit({{luciole::BrowserPreference::MillerColumnWidth, width}});
  };
  miller->textInputClient = &m_fileKeys;
  miller->requestUpdate = [this] { m_surface->requestUpdate(); };
  content->addChild(std::move(miller));
  content->addChild(m_detailsHeader.buildScrollbar());
  return content;
}

void BrowserPage::dismissPopup() {
  const auto generation = m_activePopup.generation();
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, generation, lifetime] {
    if (!lifetime.expired() && generation == m_activePopup.generation()) {
      closePopup();
    }
  });
}

void BrowserPage::closePopup() {
  m_submenus.menuClosed();
  m_launcher.cancelApplications();
  if (!m_previewController.bounds() || !previewVisible()) {
    m_preview.clear();
  }
  m_dialogDropdown.reset();
  m_activePopup.close();
  m_popupDispatcher.setSceneRoot(nullptr);
  m_popupDispatcher.setTextInputContext(nullptr, nullptr);
  if (m_popupSurface != nullptr) {
    m_popupSurface->setSceneRoot(nullptr);
  }
  m_dialogTier.reset();
  m_dialogTop.reset();
  m_dialogShadow = nullptr;
  m_dialogPosition.reset();
  m_dialogResizePending = false;
  m_popupDispatcher.setFocusChangeCallback({});
  m_activePopup.release();
  m_menuController.menuClosed();
  m_popupSurface.reset();
  m_popupDismiss = PopupDismiss::OutsideClick;
  dispatcher().setTextInputContext(m_surface->wlSurface(), &m_textInput);
  m_surface->requestLayout();
}

PopupSurface* BrowserPage::popupSurface() { return m_popupSurface.get(); }

InputDispatcher& BrowserPage::popupDispatcher() { return m_popupDispatcher; }

ActivePopup& BrowserPage::activePopup() { return m_activePopup; }

DialogDropdown& BrowserPage::dialogDropdown() { return m_dialogDropdown; }

ContextMenuControl* BrowserPage::contextMenu() { return m_menuController.menu(); }

std::uint32_t BrowserPage::popupSerial() { return m_popupSerial != 0 ? m_popupSerial : m_connection.lastInputSerial(); }

float BrowserPage::popupMaxHeight() const {
  const auto* output = m_connection.findOutputByWl(m_connection.outputForSurface(m_surface->wlSurface()));
  const auto height = output != nullptr && output->hasUsableGeometry() ? output->effectiveLogicalHeight()
                                                                       : static_cast<std::int32_t>(m_surface->height());
  return std::max(120.0F, static_cast<float>(height) - 32.0F);
}

luciole::WaylandTextClipboard& BrowserPage::clipboard() { return m_clipboard; }

luciole::WindowSurface& BrowserPage::surface() { return *m_surface; }

InputDispatcher& BrowserPage::dispatcher() { return *m_dispatcher; }

bool BrowserPage::popupAvailable() { return !m_popupAvailable || m_popupAvailable(); }

void BrowserPage::prepareOverlay() {
  m_sidebar.dismissDrawer();
  closePopup();
  cancelDrag();
  m_locationBar.clearCompletions();
  if (m_locationBar.editing()) {
    m_locationBar.endLocationEdit(false);
  }
}

Node& BrowserPage::sceneRoot() { return *m_root; }

void BrowserPage::focusFileList() {
  // The window has one focus: an inactive pane must not take it from the active one.
  if (m_list && m_active) {
    dispatcher().setFocus(m_list->focusArea());
  }
}

bool BrowserPage::active() const { return m_active; }

bool BrowserPage::thumbnailsPaused() const { return m_thumbnails.clearing(); }

const luciole::BrowserSettings& BrowserPage::settings() const { return m_savedSettings; }

bool BrowserPage::hasOtherPane() const { return m_hasOther && m_hasOther(); }

bool BrowserPage::choosing() const { return static_cast<bool>(m_chooser.open); }

void BrowserPage::setChooser(ChooserHooks hooks) {
  m_chooser = std::move(hooks);
  // A chooser returns files; it never launches them.
  m_browser.setFileActivatedCallback([](const luciole::Location&) {});
  m_miller.setFileActivatedCallback([](const luciole::Location&) {});
  for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
    adapter->setDragCallback([](const std::vector<luciole::EntryId>&) {}, [] {});
  }
  focusFileList();
}

void BrowserPage::requestWorkspace(
    WorkspaceCommand command, std::optional<luciole::Location> location, std::optional<luciole::ActionRequest> pending
) {
  if (!m_workspaceRequest) {
    return;
  }
  if (pending) {
    m_pendingWorkspaceRequest = std::move(pending);
  }
  m_workspaceRequest(command, std::move(location));
}

LayoutRect BrowserPage::menuAnchor() const { return sceneRect(*m_menuButton); }

void BrowserPage::showPopup(
    std::unique_ptr<Flex> root, ContextMenuControl* menu, LayoutRect anchor, float width, float height, bool alignLeft
) {
  closePopup();
  auto* content = root.get();
  openPopup(
      std::move(root), content, menu,
      {.anchorX = static_cast<int>(anchor.x),
       .anchorY = static_cast<int>(anchor.y),
       .anchorWidth = std::max(1, static_cast<int>(anchor.width)),
       .anchorHeight = std::max(1, static_cast<int>(anchor.height)),
       .width = static_cast<std::uint32_t>(width),
       .height = static_cast<std::uint32_t>(height),
       .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
       .gravity = alignLeft ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
       .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
           | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
           | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y},
      PopupDismiss::OutsideClick
  );
}

void BrowserPage::showContextMenu(
    std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor
) {
  m_menuController.showContextMenu(std::move(entries), std::move(action), anchor);
}

float BrowserPage::dialogWidth(DialogWidth width) const {
  const float tier = width == DialogWidth::Small ? kDialogWidthSmall
      : width == DialogWidth::Medium             ? kDialogWidthMedium
                                                 : kDialogWidthLarge;
  return std::floor(std::max(1.0F, std::min(tier, static_cast<float>(m_surface->width()) - kDialogMargin * 2.0F)));
}

popup_chrome::Geometry BrowserPage::dialogGeometry(Flex& content) {
  auto& renderer = (m_popupSurface ? m_popupSurface->renderTarget() : m_surface->renderTarget()).renderer();
  const float width = dialogWidth(*m_dialogTier);
  // A width change moves an open dropdown's anchor.
  if (width != content.width()) {
    m_dialogDropdown.closeSelectDropdown();
  }
  m_settingsController.layoutDialog(width);
  m_propertiesController.layoutDialog(width);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = std::ceil(
      std::max(
          1.0F,
          std::min(
              content.measure(renderer, constraints).height,
              static_cast<float>(m_surface->height()) - kDialogMargin * 2.0F
          )
      )
  );
  return popup_chrome::computeGeometry(width, height, kDialogShadow, Style::popupShadowsEnabled());
}

std::pair<int, int> BrowserPage::dialogPosition(const popup_chrome::Geometry& chrome) {
  // Centered when it opens and when the window height changes; otherwise the top edge stays put so switching
  // tabs or pages only moves the bottom edge. A taller page slides it up just enough to stay inside the margin.
  const auto windowHeight = static_cast<float>(m_surface->height());
  if (windowHeight != m_dialogWindowHeight) {
    m_dialogWindowHeight = windowHeight;
    m_dialogTop.reset();
  }
  const float lowest = std::max(kDialogMargin, windowHeight - kDialogMargin - chrome.contentHeight);
  const float top = std::clamp(m_dialogTop.value_or((windowHeight - chrome.contentHeight) / 2), kDialogMargin, lowest);
  m_dialogTop = top;
  return {
      static_cast<int>((static_cast<float>(m_surface->width()) - chrome.contentWidth) / 2) - chrome.bleed.left,
      static_cast<int>(top) - chrome.bleed.up
  };
}

void BrowserPage::showDialog(std::unique_ptr<Flex> root, DialogWidth width, PopupDismiss dismiss) {
  // The frame holds the shadow in the bleed around the dialog; only the dialog itself takes input.
  auto* content = root.get();
  content->setFlexGrow(1.0F);
  auto frame = ui::column({.align = FlexAlign::Stretch}, std::move(root));
  closePopup();
  m_dialogTier = width;
  // Measured before the popup exists so the first configure already has the final size.
  UiPhaseScope phase(UiPhase::Layout);
  const auto chrome = dialogGeometry(*content);
  frame->setPadding(
      static_cast<float>(chrome.bleed.up), static_cast<float>(chrome.bleed.right),
      static_cast<float>(chrome.bleed.down), static_cast<float>(chrome.bleed.left)
  );
  if (auto* shadow = popup_chrome::addShadow(*frame, chrome, kDialogShadow, Style::scaledRadiusLg())) {
    shadow->setParticipatesInLayout(false);
    m_dialogShadow = shadow;
  }
  const auto position = dialogPosition(chrome);
  openPopup(
      std::move(frame), content, nullptr,
      {.anchorX = position.first,
       .anchorY = position.second,
       .anchorWidth = 1,
       .anchorHeight = 1,
       .width = chrome.surfaceWidth,
       .height = chrome.surfaceHeight,
       .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
       .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
       .constraintAdjustment =
           XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y},
      dismiss
  );
  if (!m_popupSurface) {
    return;
  }
  m_dialogPosition = position;
  popup_chrome::setContentInputRegion(*m_popupSurface, chrome);
  m_popupDispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
    m_operations.revealFocus(focused);
    m_aboutController.revealFocus(focused);
    m_applicationChooser.revealFocus(focused);
    if (m_popupSurface) {
      m_popupSurface->requestLayout();
    }
  });
  luciole::focusFirstDialogControl(m_popupDispatcher);
  m_popupSurface->requestLayout();
}

void BrowserPage::layoutDialogFrame() {
  const auto chrome = dialogGeometry(*m_activePopup.root());
  if (chrome.surfaceWidth != m_popupSurface->width() || chrome.surfaceHeight != m_popupSurface->height()) {
    (void)m_popupSurface->resize(chrome.surfaceWidth, chrome.surfaceHeight, false);
    popup_chrome::setContentInputRegion(*m_popupSurface, chrome);
    m_dialogResizePending = true;
  }
  if (m_dialogShadow != nullptr) {
    m_dialogShadow->setFrameSize(chrome.contentWidth, chrome.contentHeight);
  }
  const auto position = dialogPosition(chrome);
  if (m_dialogPosition != position) {
    m_dialogPosition = position;
    (void)m_popupSurface->repositionAnchor(
        {.anchorX = position.first,
         .anchorY = position.second,
         .anchorWidth = 1,
         .anchorHeight = 1,
         .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
         .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
         .constraintAdjustment =
             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y},
        false
    );
  }
}

void BrowserPage::openPopup(
    std::unique_ptr<Flex> scene, Flex* content, ContextMenuControl* menu, PopupSurfaceConfig config,
    PopupDismiss dismiss
) {
  m_fileKeys.clearTypeahead();
  m_locationBar.clearCompletions();
  dispatcher().setTextInputContext(nullptr, nullptr);
  m_menuController.setMenu(menu);
  m_popupDismiss = dismiss;
  m_activePopup.open(std::move(scene), content);
  m_popupSurface = std::make_unique<PopupSurface>(m_connection);
  m_popupSurface->setRenderContext(&m_renderer);
  m_popupSurface->setAnimationManager(&m_animations);
  m_popupSurface->setDismissedCallback([this] { dismissPopup(); });
  m_activePopup.scene()->setAnimationManager(&m_animations);
  m_popupSurface->setSceneRoot(m_activePopup.scene());
  m_popupDispatcher.setSceneRoot(m_activePopup.scene());
  m_popupDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.extension<luciole::DesktopIntegration>().setCursorShape(serial, shape);
  });
  m_popupSurface->setPrepareFrameCallback([this](bool, bool) {
    if (m_popupSurface == nullptr) {
      return;
    }
    UiPhaseScope phase(UiPhase::Layout);
    m_previewController.layoutQuickPreview();
    // Resize before binding the surface: binding fetches Mesa's back buffer, and a wl_egl_window resize after that
    // only applies to the next frame, so this frame's old-size buffer would be shown stretched to the new size.
    if (m_dialogTier) {
      layoutDialogFrame();
    }
    if (!m_renderer.makeCurrent(m_popupSurface->renderTarget())) {
      return;
    }
    m_operations.layoutDialog();
    m_aboutController.layoutDialog();
    m_activePopup.scene()->arrange(
        m_popupSurface->renderTarget().renderer(),
        {0, 0, static_cast<float>(m_popupSurface->width()), static_cast<float>(m_popupSurface->height())}
    );
    m_batchRename.syncTooltip(m_popupSurface->renderTarget().renderer());
    m_renameController.syncPlaceIconTooltip(m_popupSurface->renderTarget().renderer());
  });
  m_popupSurface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (std::exchange(m_dialogResizePending, false)
        && m_dialogPosition
        && (m_popupSurface->configuredX() != m_dialogPosition->first
            || m_popupSurface->configuredY() != m_dialogPosition->second)) {
      // A parent resize can constrain the first position against its old geometry.
      // Retry the requested anchor once after receiving that configure.
      m_dialogPosition.reset();
    }
    m_popupSurface->requestLayout();
  });
  config.serial = m_popupSerial != 0 ? m_popupSerial : m_connection.lastInputSerial();
  config.grab = dismiss == PopupDismiss::OutsideClick;
  if (!m_popupSurface->initializeAsChild(
          m_surface->xdgSurface(), m_connection.outputForSurface(m_surface->wlSurface()), config
      )) {
    closePopup();
  } else {
    m_popupDispatcher.setTextInputContext(m_popupSurface->wlSurface(), &m_textInput, true, m_surface->wlSurface());
  }
}

void BrowserPage::toggleHidden() {
  browser().toggleHidden();
  const bool show = browser().model().filter().showHidden;
  for (const auto& column : m_miller.columns()) {
    if (column->browser->model().filter().showHidden != show) {
      column->browser->toggleHidden();
    }
  }
  if (choosing()) {
    m_preferences.submit({{luciole::BrowserPreference::ChooserShowHidden, show}});
  } else {
    m_preferences.saveShowHidden(show);
  }
}
