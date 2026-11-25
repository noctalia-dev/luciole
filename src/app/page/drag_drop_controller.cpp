#include "app/page/drag_drop_controller.h"

#include "app/operation_hub.h"
#include "app/page/location_bar_controller.h"
#include "app/page/page_context.h"
#include "app/page/sidebar_controller.h"
#include "app/page/transfer_controller.h"
#include "browser/browser_controller.h"
#include "browser/miller_columns.h"
#include "browser/places.h"
#include "config/settings.h"
#include "cursor-shape-v1-client-protocol.h"
#include "operations/trash_item.h"
#include "platform/desktop_integration.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/file_icon.h"
#include "ui/browser/places_sidebar.h"
#include "ui/builders.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/selection_contour.h"
#include "ui/palette.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace {
  luciole::DragAction resolveDropAction(
      std::uint32_t modifiers, const luciole::Location& destination, bool forcedMove, bool remoteSource,
      luciole::DropAction preference
  ) {
    if (forcedMove || destination == luciole::Location::trash()) {
      return luciole::DragAction::Move;
    }
    if ((modifiers & KeyMod::Alt) != 0) {
      return luciole::DragAction::Ask;
    }
    if ((modifiers & KeyMod::Ctrl) != 0) {
      return luciole::DragAction::Copy;
    }
    if ((modifiers & KeyMod::Shift) != 0) {
      return luciole::DragAction::Move;
    }
    if (!destination.isNative() || remoteSource) {
      return luciole::DragAction::Copy;
    }
    return preference == luciole::DropAction::Menu ? luciole::DragAction::Ask
        : preference == luciole::DropAction::Copy  ? luciole::DragAction::Copy
                                                   : luciole::DragAction::Move;
  }
} // namespace

DragDropController::DragDropController(
    PageContext& page, WaylandConnection& connection, luciole::PinService& pins, TransferController& transferController,
    luciole::ThumbnailService& thumbnails, luciole::FileIconCache& icons, luciole::MillerColumns& miller,
    SidebarController& sidebar, LocationBarController& locationBar
)
    : m_page(page), m_connection(connection), m_pins(pins), m_transferController(transferController),
      m_thumbnails(thumbnails), m_icons(icons), m_miller(miller), m_sidebar(sidebar), m_locationBar(locationBar) {}

void DragDropController::setForeignDrop(
    std::function<std::optional<std::pair<luciole::Location, LayoutRect>>(float, float)> callback
) {
  m_foreignDrop = std::move(callback);
}

void DragDropController::trackButton(const PointerEvent& event) {
  if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT) {
    m_fileDragSerial = event.pressed && !event.touch ? event.serial : 0;
  }
}

void DragDropController::cancelFileDrag() {
  if (m_drag && !m_drag->sourcePin && !m_systemDrag) {
    cancelDrag();
  }
}

void DragDropController::cancelLocalDrag() {
  if (!m_systemDrag) {
    cancelDrag();
  }
}

void DragDropController::stop() {
  m_dragScroll.stop();
  m_externalScroll.stop();
}

void DragDropController::buildOverlay(AnimationManager& animations) {
  m_detachedDragOverlay = std::make_unique<Node>();
  m_dragOverlay = m_detachedDragOverlay.get();
  m_dragOverlay->setZIndex(80);
  m_dragOverlay->setParticipatesInLayout(false);
  m_dragOverlay->setHitTestVisible(false);
  m_dragOverlay->setAnimationManager(&animations);
  auto dragIcon = std::make_unique<luciole::FileIcon>(m_icons, 40);
  m_dragIcon = dragIcon.get();
  m_dragIcon->setVisible(false);
  m_dragIcon->setParticipatesInLayout(false);
  m_dragIcon->setOpacity(0.85F);
  m_dragIcon->setZIndex(100);
  m_dragOverlay->addChild(std::move(dragIcon));
  m_dragOverlay->addChild(
      ui::column(
          {.out = &m_dragCount,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = colorSpecFromRole(ColorRole::Primary),
           .styleRadius = Style::radiusSm,
           .visible = false,
           .participatesInLayout = false},
          ui::label(
              {.out = &m_dragCountLabel,
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnPrimary)}
          )
      )
  );
  m_dragCount->setHitTestVisible(false);
  m_dragCount->setZIndex(101);
  m_dragOverlay->addChild(m_sidebar.buildDropLine());
  m_dragOverlay->addChild(
      ui::box(
          {.out = &m_folderDropHighlight,
           .fill = clearColorSpec(),
           .border = colorSpecFromRole(ColorRole::Primary),
           .borderWidth = Style::focusRingWidth,
           .styleRadius = SelectionContour::kBaseCornerRadius,
           .visible = false,
           .participatesInLayout = false}
      )
  );
  m_folderDropHighlight->setZIndex(99);
  m_folderDropHighlight->setHitTestVisible(false);
}

void DragDropController::mount(Node& host) {
  if (m_detachedDragOverlay) {
    host.addChild(std::move(m_detachedDragOverlay));
  }
}

void DragDropController::unmount() {
  if (m_dragOverlay && m_dragOverlay->parent()) {
    m_detachedDragOverlay = m_dragOverlay->parent()->removeChild(m_dragOverlay);
  }
}

void DragDropController::applyScroll() {
  if (m_externalScrollDelta != 0) {
    const auto delta = std::exchange(m_externalScrollDelta, 0.0F);
    if (m_externalScrollInPlaces) {
      m_sidebar.places().dragScroll(delta);
    } else {
      auto& scroll = dragView()->scrollView();
      scroll.requestScrollToOffset(scroll.scrollOffset() + delta);
    }
    m_refreshExternalDrop = true;
  }
  if (m_drag && m_dragScrollDelta != 0) {
    const auto delta = std::exchange(m_dragScrollDelta, 0.0F);
    if (m_dragScrollInPlaces) {
      m_sidebar.places().dragScroll(delta);
    } else if (m_miller.enabled()) {
      auto& scroll = dragView()->scrollView();
      scroll.requestScrollToOffset(scroll.scrollOffset() + delta);
    } else {
      m_page.fileList()->scrollDuringDrag(delta);
    }
  }
}

void DragDropController::refreshDrop() {
  if (std::exchange(m_refreshExternalDrop, false)) {
    m_page.clipboard().refreshDrag();
  }
}

void DragDropController::clearExternalDrop() {
  m_externalDraggingHere = false;
  m_refreshExternalDrop = false;
  m_externalScroll.stop();
  m_externalScrollDelta = 0;
  if (m_externalDropTarget || m_externalPinSlot) {
    m_externalDropTarget.reset();
    m_externalPinSlot.reset();
    m_page.surface().requestLayout();
  }
}

const std::vector<luciole::Bookmark>* DragDropController::draggedFolders() const {
  return m_systemDrag
          && m_drag
          && m_drag->foldersOnly
          && std::ranges::none_of(m_drag->entries, [](const auto& entry) { return entry.location.isTrash(); })
      ? &m_drag->bookmarks
      : nullptr;
}

luciole::FileDropTarget DragDropController::externalDrop(float x, float y, const std::vector<luciole::Bookmark>* pins) {
  m_externalDraggingHere = true;
  updateExternalScroll();
  // External offers are decoded only after a drop. Transfer worker validation
  // still checks self/descendant/alias targets before any filesystem change.
  if (pins && m_sidebar.visible() && m_pins.editable() && !m_sidebar.places().dropFolderAt(x, y)) {
    if (const auto slot = m_sidebar.places().dropSlot(x, y)) {
      m_externalPinSlot = slot;
      updateExternalScroll();
      m_page.surface().requestLayout();
      const std::weak_ptr<int> lifetime = m_lifetime;
      return {
          .receive =
              [this, lifetime, bookmarks = *pins, slot = *slot](
                  const luciole::FilePayload&, const luciole::DropOptions&, const luciole::DropCompletion& finish
              ) {
                if (lifetime.expired() || !m_page.visible() || !m_pins.editable()) {
                  finish(false);
                  return;
                }
                finish(m_pins.add(bookmarks, slot.before, slot.belowSeparator));
              },
          .preferred = luciole::DragAction::Copy,
          .copyOnly = true
      };
    }
  }
  m_externalHover = true;
  const auto target = dropTarget();
  m_externalHover = false;
  if (!target) {
    return {};
  }
  return externalDestination(target->location, target->rect);
}

luciole::FileDropTarget DragDropController::externalDestination(const luciole::Location& destination, LayoutRect rect) {
  const auto* payload = m_page.clipboard().dragFiles();
  const bool fromTrash =
      payload && std::ranges::any_of(payload->locations, [](const auto& source) { return source.isTrash(); });
  if (!m_page.visible()
      || m_page.overlay()
      || m_page.popupSurface() != nullptr
      || m_page.operationsBusy()
      || (fromTrash
          && (!destination.isNative() || (m_connection.keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Alt)) != 0))
      || (destination == luciole::Location::trash()
          && (m_connection.keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Alt)) != 0)
      || (!luciole::transferLocationSupported(destination) && destination != luciole::Location::trash())) {
    return {};
  }
  m_externalDropTarget = DropTarget{destination, rect};
  updateExternalScroll();
  m_page.surface().requestLayout();
  const auto context = m_page.browser().contextSerial();
  const std::weak_ptr<int> lifetime = m_lifetime;
  return {
      .receive =
          [this, lifetime, context,
           destination](luciole::FilePayload payload, luciole::DropOptions options, luciole::DropCompletion finish) {
            if (lifetime.expired()
                || context != m_page.browser().contextSerial()
                || !m_page.visible()
                || m_page.overlay()
                || m_page.operationsBusy()) {
              finish(false);
              return;
            }
            m_transferController.receiveDrop(std::move(payload), destination, std::move(options), std::move(finish));
          },
      .extract = destination.isNative()
          ? std::function<void(luciole::ExtractionOffer, luciole::DropCompletion)>(
                [this, lifetime, context, destination](luciole::ExtractionOffer offer, luciole::DropCompletion finish) {
                  if (lifetime.expired()
                      || context != m_page.browser().contextSerial()
                      || !m_page.visible()
                      || m_page.overlay()
                      || m_page.operationsBusy()) {
                    finish(false);
                    return;
                  }
                  m_transferController.extractDrop(std::move(offer), destination, std::move(finish));
                }
            )
          : nullptr,
      .failed =
          [this, lifetime, context](std::string error) {
            if (!lifetime.expired() && context == m_page.browser().contextSerial()) {
              m_page.actionError(std::move(error));
            }
          },
      .preferred = resolveDropAction(
          m_connection.keyboardModifiers(), destination, fromTrash, false, m_page.settings().dropAction
      ),
      .moveOnly = fromTrash || destination == luciole::Location::trash()
  };
}

std::optional<std::pair<luciole::Location, LayoutRect>> DragDropController::foreignTarget(float x, float y) const {
  if (!m_page.visible() || !m_page.filePane().containsScenePoint(x, y) || m_page.overlay()) {
    return std::nullopt;
  }
  // A file row drops into the shown folder, like empty space.
  if (const auto index = m_page.fileList()->visible() ? m_page.fileList()->itemAtScene(x, y) : std::nullopt) {
    const auto& rows = m_page.browser().model().rows();
    const auto* entry = *index < rows.size() ? m_page.browser().model().entry(rows[*index]) : nullptr;
    if (entry && !entry->symlink && entry->type == luciole::FileType::Directory) {
      auto location = entry->location();
      if (!luciole::transferLocationSupported(location)) {
        return std::nullopt;
      }
      if (auto rect = m_page.fileList()->itemRect(*index)) {
        return std::pair(std::move(location), *rect);
      }
      return std::nullopt;
    }
  }
  const auto& location = m_page.browser().model().snapshot().location;
  if (location && luciole::transferLocationSupported(*location) && m_page.browser().searchQuery().empty()) {
    return std::pair(*location, sceneRect(m_page.filePane()));
  }
  return std::nullopt;
}

bool DragDropController::dragCurrent() const {
  if (!m_drag) {
    return false;
  }
  if (m_drag->sourcePin) {
    return m_pins.editable() && m_pins.contains(*m_drag->sourcePin);
  }
  return !m_page.jobs().busy() && std::ranges::all_of(m_drag->entries, [this](const auto& target) {
    const auto* entry = m_page.browser().model().entry(target.id);
    return entry != nullptr
        && entry->location() == target.location
        && std::ranges::contains(m_page.browser().model().rows(), target.id);
  });
}

bool DragDropController::validDropFolder(const luciole::Location& destination) const {
  const auto* payload = m_externalHover ? m_page.clipboard().dragFiles() : nullptr;
  const bool fromTrash = payload
      ? std::ranges::any_of(payload->locations, [](const auto& source) { return source.isTrash(); })
      : m_drag && std::ranges::any_of(m_drag->entries, [](const auto& source) { return source.location.isTrash(); });
  if (fromTrash) {
    if (!destination.isNative()
        || destination.isTrash()
        || (m_connection.keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Alt)) != 0) {
      return false;
    }
    return payload ? std::ranges::all_of(payload->locations, luciole::isTopLevelTrashItem)
                   : m_drag && !m_drag->sourcePin && std::ranges::all_of(m_drag->entries, [](const auto& source) {
                       return luciole::isTopLevelTrashItem(source.location);
                     });
  }
  if (destination == luciole::Location::trash()) {
    if ((m_connection.keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Alt)) != 0) {
      return false;
    }
    if (m_externalHover) {
      const auto* payload = m_page.clipboard().dragFiles();
      return !payload || std::ranges::all_of(payload->locations, [](const auto& source) { return source.isNative(); });
    }
    return m_drag
        && !m_drag->sourcePin
        && std::ranges::all_of(m_drag->entries, [](const auto& source) { return source.location.isNative(); });
  }
  if (m_externalHover) {
    if (!luciole::transferLocationSupported(destination)) {
      return false;
    }
    const auto* payload = m_page.clipboard().dragFiles();
    if (!payload) {
      return true;
    }
    bool differentParent = false;
    for (const auto& source : payload->locations) {
      if (luciole::transferLocationsEqual(source, destination)
          || luciole::transferLocationInside(destination, source)) {
        return false;
      }
      differentParent |= source.parent() != destination;
    }
    return differentParent || (m_connection.keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Alt)) != 0;
  }
  if (!m_drag || m_drag->sourcePin || !luciole::transferLocationSupported(destination)) {
    return false;
  }
  bool anyDifferentParent = false;
  for (const auto& source : m_drag->entries) {
    if (!luciole::transferLocationSupported(source.location)
        || luciole::transferLocationsEqual(source.location, destination)) {
      return false;
    }
    const auto* entry = m_page.browser().model().entry(source.id);
    if (entry == nullptr) {
      return false;
    }
    if (entry->type == luciole::FileType::Directory && luciole::transferLocationInside(destination, source.location)) {
      return false;
    }
    anyDifferentParent = anyDifferentParent || source.location.parent() != destination;
  }
  return anyDifferentParent || (m_connection.keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Alt)) != 0;
}

std::optional<DragDropController::DropTarget> DragDropController::dropTarget() const {
  if (!m_externalHover && (!m_drag || m_drag->sourcePin)) {
    return std::nullopt;
  }
  if (m_sidebar.drawerContains(m_page.pointerX(), m_page.pointerY())
      && (!m_sidebar.visible() || !m_sidebar.places().containsScenePoint(m_page.pointerX(), m_page.pointerY()))) {
    return std::nullopt;
  }
  if (const auto place =
          m_sidebar.visible() ? m_sidebar.places().dropFolderAt(m_page.pointerX(), m_page.pointerY()) : std::nullopt) {
    if (validDropFolder(place->location)) {
      if (const auto rect = m_sidebar.places().folderRect(place->location)) {
        return DropTarget{place->location, *rect};
      }
    }
    return std::nullopt;
  }
  if (m_locationBar.breadcrumbsContain(m_page.pointerX(), m_page.pointerY())) {
    for (const auto& [location, button] : m_locationBar.breadcrumbs()) {
      if (button->containsScenePoint(m_page.pointerX(), m_page.pointerY()) && validDropFolder(location)) {
        return DropTarget{location, sceneRect(*button)};
      }
    }
  }
  if (m_page.upButton().containsScenePoint(m_page.pointerX(), m_page.pointerY())
      && m_page.browser().model().snapshot().location) {
    if (const auto location = m_page.browser().model().snapshot().location->parent()) {
      if (validDropFolder(*location)) {
        return DropTarget{*location, sceneRect(m_page.upButton())};
      }
    }
  }
  if (!m_externalHover && m_foreignDrop) {
    if (auto target = m_foreignDrop(m_page.pointerX(), m_page.pointerY()); target && validDropFolder(target->first)) {
      return DropTarget{target->first, target->second};
    }
  }
  if (!m_page.filePane().containsScenePoint(m_page.pointerX(), m_page.pointerY())) {
    return std::nullopt;
  }
  if (m_miller.enabled()) {
    if (const auto index = m_page.millerView().columnAt(m_page.pointerX(), m_page.pointerY())) {
      const auto* grid = m_page.millerView().view(*index);
      const auto& model = m_miller.columns()[*index]->browser->model();
      // A file row drops into the column's folder, like empty space; an invalid folder row denies.
      if (const auto row = grid->itemAtScene(m_page.pointerX(), m_page.pointerY()); row && *row < model.rows().size()) {
        const auto* entry = model.entry(model.rows()[*row]);
        if (entry && entry->type == luciole::FileType::Directory && !entry->symlink) {
          if (auto location = entry->location(); validDropFolder(location)) {
            return DropTarget{std::move(location), *grid->itemRect(*row), entry->id};
          }
          return std::nullopt;
        }
      }
      const auto& folder = model.snapshot().location;
      if (folder && validDropFolder(*folder)) {
        return DropTarget{*folder, sceneRect(*grid)};
      }
    }
    return std::nullopt;
  }
  if (m_page.fileList()->visible()) {
    // A file row drops into the shown folder, like empty space; an invalid folder row denies.
    if (const auto index = m_page.fileList()->itemAtScene(m_page.pointerX(), m_page.pointerY())) {
      const auto& rows = m_page.browser().model().rows();
      const auto* entry = *index < rows.size() ? m_page.browser().model().entry(rows[*index]) : nullptr;
      if (entry != nullptr && entry->type == luciole::FileType::Directory && !entry->symlink) {
        if (auto location = entry->location(); validDropFolder(location)) {
          if (const auto rect = m_page.fileList()->itemRect(*index)) {
            return DropTarget{std::move(location), *rect, entry->id};
          }
        }
        return std::nullopt;
      }
    }
  }
  const auto& location = m_page.browser().model().snapshot().location;
  if (location && validDropFolder(*location)) {
    return DropTarget{*location, sceneRect(m_page.filePane())};
  }
  return std::nullopt;
}

std::optional<luciole::PinSlot> DragDropController::pinDropSlot() const {
  if (!m_drag || !m_pins.editable() || !m_sidebar.visible()) {
    return std::nullopt;
  }
  if (!m_drag->sourcePin
      && std::ranges::any_of(m_drag->entries, [](const auto& entry) { return entry.location.isTrash(); })) {
    return std::nullopt;
  }
  if (!m_drag->sourcePin
      && (!m_drag->foldersOnly || m_sidebar.places().dropFolderAt(m_page.pointerX(), m_page.pointerY()))) {
    return std::nullopt;
  }
  return m_sidebar.places().dropSlot(m_page.pointerX(), m_page.pointerY());
}

void DragDropController::startFileDrag(const std::vector<luciole::EntryId>& ids) {
  if (ids.empty() || m_page.jobs().busy() || m_page.choosing()) {
    return;
  }
  Drag drag;
  drag.foldersOnly = true;
  for (const auto id : ids) {
    const auto* entry = m_page.browser().model().entry(id);
    if (entry == nullptr
        || entry->metadataPending
        || entry->metadataFailed
        || (entry->type != luciole::FileType::Directory
            && entry->type != luciole::FileType::Regular
            && !entry->symlink)) {
      return;
    }
    auto location = entry->location();
    if (location.isTrash()
        && (!luciole::isTopLevelTrashItem(location)
            || !entry->identity()
            || entry->access.remove == false
            || (m_connection.keyboardModifiers() & (KeyMod::Ctrl | KeyMod::Alt)) != 0)) {
      return;
    }
    drag.foldersOnly = drag.foldersOnly && entry->type == luciole::FileType::Directory && !entry->symlink;
    drag.entries.push_back({id, location});
    drag.bookmarks.push_back({entry->displayName(), std::move(location), {}});
  }
  const auto* entry = m_page.browser().model().entry(ids.front());
  if (entry) {
    m_dragArtwork = m_thumbnails.prepared(*entry);
    if (!m_dragArtwork || !m_dragArtwork->pixels) {
      m_dragArtwork = m_icons.request(
          luciole::iconSource(entry->icon),
          static_cast<int>(std::ceil(
              (m_page.viewMode() == luciole::ViewMode::Grid ? m_page.gridIconSize() : m_page.rowIconSize())
              * m_page.surface().effectiveBufferScale()
          ))
      );
    }
  }
  m_drag = std::move(drag);
  luciole::FilePayload payload;
  for (const auto& target : m_drag->entries) {
    payload.locations.push_back(target.location);
  }
  const std::weak_ptr<int> lifetime = m_lifetime;
  const auto cursorSerial = m_connection.extension<luciole::DesktopIntegration>().pointerEnterSerial();
  m_systemDrag = m_page.clipboard().startDrag(
      std::move(payload), m_page.surface().wlSurface(), m_fileDragSerial,
      m_dragArtwork && m_dragArtwork->pixels ? &*m_dragArtwork->pixels : nullptr,
      [this, lifetime] {
        if (!lifetime.expired()) {
          m_systemDrag = false;
          cancelDrag();
        }
      },
      m_drag->entries.front().location.isTrash()                    ? std::optional(luciole::DragAction::Move)
          : (m_connection.keyboardModifiers() & KeyMod::Alt) != 0   ? std::optional(luciole::DragAction::Ask)
          : (m_connection.keyboardModifiers() & KeyMod::Ctrl) != 0  ? std::optional(luciole::DragAction::Copy)
          : (m_connection.keyboardModifiers() & KeyMod::Shift) != 0 ? std::optional(luciole::DragAction::Move)
                                                                    : std::nullopt,
      [this, lifetime, cursorSerial](std::optional<luciole::DragAction> action) {
        if (!lifetime.expired()) {
          // The shared connection binds cursor-shape v1; dnd_ask requires v2.
          const auto shape = action == luciole::DragAction::Copy ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY
              : action == luciole::DragAction::Move              ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE
              : action == luciole::DragAction::Ask               ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU
                                                                 : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
          m_connection.extension<luciole::DesktopIntegration>().setCursorShape(cursorSerial, shape);
        }
      }
  );
  if (m_systemDrag) {
    m_page.dispatcher().cancelPointerCapture();
    m_connection.extension<luciole::DesktopIntegration>().setCursorShape(
        cursorSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP
    );
    m_page.surface().requestLayout();
  } else {
    updateDrag();
  }
}

void DragDropController::startPlaceDrag(const luciole::Bookmark& bookmark) {
  if (!m_pins.editable() || m_page.choosing()) {
    return;
  }
  m_drag = Drag{.entries = {}, .bookmarks = {bookmark}, .sourcePin = bookmark.location, .foldersOnly = true};
  updateDrag();
}

void DragDropController::cancelDrag() {
  if (m_systemDrag) {
    m_systemDrag = false;
    m_page.clipboard().cancelDrag();
  }
  if (!m_drag) {
    return;
  }
  m_drag.reset();
  m_dragArtwork.reset();
  m_lastDropTarget.reset();
  m_dragScroll.stop();
  m_dragScrollDelta = 0;
  m_sidebar.places().cancelDragCandidate();
  m_page.dispatcher().cancelPointerCapture();
  if (m_dragIcon != nullptr) {
    m_dragIcon->setVisible(false);
    m_dragCount->setVisible(false);
    m_sidebar.hideDropLine();
    m_folderDropHighlight->setVisible(false);
  }
  m_connection.extension<luciole::DesktopIntegration>().setCursorShape(
      m_connection.lastInputSerial(), WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
  );
  m_page.surface().requestLayout();
}

VirtualGridView* DragDropController::dragView() const {
  if (m_miller.enabled()) {
    if (const auto index = m_page.millerView().columnAt(m_page.pointerX(), m_page.pointerY())) {
      return m_page.millerView().view(*index);
    }
  }
  return m_page.fileList();
}

float DragDropController::dragScrollSpeed(bool& places) const {
  places = m_sidebar.visible() && m_sidebar.places().containsScenePoint(m_page.pointerX(), m_page.pointerY());
  if (places) {
    return m_sidebar.places().dragScrollSpeed(m_page.pointerX(), m_page.pointerY());
  }
  if ((!m_drag && !m_externalDraggingHere) || (m_drag && m_drag->sourcePin) || !m_page.fileList()->visible()) {
    return 0;
  }
  auto& scroll = dragView()->scrollView();
  if (!scroll.containsScenePoint(m_page.pointerX(), m_page.pointerY())) {
    return 0;
  }
  float x = 0, y = 0;
  Node::absolutePosition(&scroll, x, y);
  if (m_page.pointerY() < y + 32 && scroll.scrollOffset() > 0) {
    return -6;
  }
  if (m_page.pointerY() > y + scroll.height() - 32 && scroll.scrollOffset() < scroll.maxScrollOffset()) {
    return 6;
  }
  return 0;
}

void DragDropController::updateExternalScroll() {
  bool places = false;
  if (!m_externalDraggingHere || dragScrollSpeed(places) == 0 || m_externalScroll.active()) {
    return;
  }
  m_externalScroll.startRepeating(std::chrono::milliseconds(16), [this] {
    if (!m_externalDraggingHere) {
      m_externalScroll.stop();
      return;
    }
    bool places = false;
    const float speed = dragScrollSpeed(places);
    if (speed == 0) {
      m_externalScroll.stop();
      return;
    }
    m_externalScrollInPlaces = places;
    m_externalScrollDelta = std::clamp(m_externalScrollDelta + speed, -18.0F, 18.0F);
    m_page.surface().requestLayout();
  });
}

void DragDropController::updateDrag() {
  if (m_systemDrag) {
    return;
  }
  if (!dragCurrent()) {
    cancelDrag();
    return;
  }
  m_lastDropTarget = dropTarget();
  bool places = false;
  const auto speed = dragScrollSpeed(places);
  if (places != m_dragScrollInPlaces) {
    m_dragScrollDelta = 0;
    m_dragScrollInPlaces = places;
  }
  if (speed == 0) {
    m_dragScroll.stop();
    m_dragScrollDelta = 0;
  } else if (!m_dragScroll.active()) {
    m_dragScroll.startRepeating(std::chrono::milliseconds(16), [this] {
      if (!dragCurrent()) {
        cancelDrag();
        return;
      }
      bool places = false;
      const auto speed = dragScrollSpeed(places);
      if (places != m_dragScrollInPlaces) {
        m_dragScrollDelta = 0;
        m_dragScrollInPlaces = places;
      }
      if (speed == 0) {
        m_dragScroll.stop();
        return;
      }
      m_dragScrollDelta = std::clamp(m_dragScrollDelta + speed, -18.0F, 18.0F);
      m_page.surface().requestLayout();
    });
  }
  m_page.surface().requestLayout();
}

void DragDropController::layout(Renderer& renderer) {
  m_dragOverlay->setFrameSize(
      static_cast<float>(m_page.surface().width()), static_cast<float>(m_page.surface().height())
  );
  if (m_drag && !m_systemDrag && !dragCurrent()) {
    cancelDrag();
  }
  const bool active = m_drag.has_value() && !m_systemDrag;
  m_dragIcon->setVisible(active);
  m_dragCount->setVisible(active && m_drag->bookmarks.size() > 1);
  m_sidebar.hideDropLine();
  m_folderDropHighlight->setVisible(false);
  if (!active) {
    if (m_externalPinSlot) {
      m_sidebar.showDropLine(renderer, *m_externalPinSlot);
    }
    if (m_externalDropTarget) {
      m_folderDropHighlight->setVisible(true);
      m_folderDropHighlight->arrange(renderer, m_externalDropTarget->rect);
    }
    return;
  }
  luciole::FileIconSource icon{.names = {"folder"}, .filePath = {}, .desktopFile = {}};
  if (m_drag->sourcePin == luciole::Location::trash()) {
    icon.names = {"user-trash"};
  } else if (m_drag->sourcePin == luciole::Location::network()) {
    icon.names = {"network-workgroup"};
  }
  if (!m_drag->entries.empty()) {
    if (const auto* entry = m_page.browser().model().entry(m_drag->entries.front().id)) {
      icon = luciole::iconSource(entry->icon);
    }
  }
  m_dragIcon->bind(icon, colorSpecFromRole(ColorRole::OnSurface));
  const float x = std::clamp(m_page.pointerX() + 12, 0.0F, static_cast<float>(m_page.surface().width()) - 48.0F);
  const float y = std::clamp(m_page.pointerY() + 12, 0.0F, static_cast<float>(m_page.surface().height()) - 48.0F);
  m_dragIcon->arrange(renderer, {x, y, 40, 40});
  m_dragCountLabel->setText(std::to_string(m_drag->bookmarks.size()));
  m_dragCount->arrange(renderer, {x + 24, y + 28, 30, 20});
  m_lastDropTarget = dropTarget();
  const auto slot = pinDropSlot();
  if (m_lastDropTarget) {
    m_folderDropHighlight->setVisible(true);
    m_folderDropHighlight->arrange(renderer, m_lastDropTarget->rect);
  } else if (slot) {
    m_sidebar.showDropLine(renderer, *slot);
  }
  const bool remoteSource =
      std::ranges::any_of(m_drag->entries, [](const auto& source) { return !source.location.isNative(); });
  const bool fromTrash = !m_drag->entries.empty() && m_drag->entries.front().location.isTrash();
  std::optional<luciole::DragAction> action;
  if (m_lastDropTarget) {
    action = resolveDropAction(
        m_connection.keyboardModifiers(), m_lastDropTarget->location, fromTrash, remoteSource,
        m_page.settings().dropAction
    );
  }
  const auto shape = slot
      ? (m_drag->sourcePin ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY)
      : action ? (*action == luciole::DragAction::Ask        ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU
                      : *action == luciole::DragAction::Copy ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY
                                                             : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE)
               : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
  m_connection.extension<luciole::DesktopIntegration>().setCursorShape(m_connection.lastInputSerial(), shape);
}

void DragDropController::finishDrag() {
  const bool current = dragCurrent();
  const auto target = dropTarget();
  const auto previous = m_lastDropTarget;
  const auto slot = pinDropSlot();
  const auto drag = *m_drag;
  cancelDrag();
  if (!current) {
    return;
  }
  if (target && previous && target->location == previous->location && target->entry == previous->entry) {
    std::vector<luciole::Location> sources;
    sources.reserve(drag.entries.size());
    for (const auto& entry : drag.entries) {
      sources.push_back(entry.location);
    }
    m_page.setActionMessage({});
    const bool remoteSource =
        std::ranges::any_of(drag.entries, [](const auto& source) { return !source.location.isNative(); });
    const bool fromTrash = !drag.entries.empty() && drag.entries.front().location.isTrash();
    const auto action = resolveDropAction(
        m_connection.keyboardModifiers(), target->location, fromTrash, remoteSource, m_page.settings().dropAction
    );
    luciole::DropOptions options{
        .action = action,
        .canCopy = true,
        .canMove = true,
        .x = m_page.pointerX(),
        .y = m_page.pointerY(),
        .select = [](luciole::TransferAction) { return true; }
    };
    m_transferController.receiveDrop({std::move(sources), false}, target->location, std::move(options), [](bool) {});
  } else if (slot) {
    m_sidebar.pinFolders(drag.bookmarks, slot->before, slot->belowSeparator);
  }
}
