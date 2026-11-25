#include "app/page/submenu_controller.h"

#include "app/page/page_context.h"
#include "config/keybinds.h"
#include "config/settings.h"
#include "core/deferred_call.h"
#include "core/ui_phase.h"
#include "platform/desktop_integration.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <utility>

SubmenuController::SubmenuController(
    PageContext& page, WaylandConnection& connection, RenderContext& renderer, AnimationManager& animations
)
    : m_page(page), m_connection(connection), m_renderer(renderer), m_animations(animations) {}

void SubmenuController::attach(std::size_t row, int id, std::function<void()> fill, std::function<void()> closed) {
  auto* menu = m_page.contextMenu();
  if (menu == nullptr) {
    return;
  }
  m_rows.push_back({.index = row, .id = id, .fill = std::move(fill), .closed = std::move(closed)});
  menu->setOnSubmenuOpen([this](const ContextMenuControlEntry& entry, float) { open(entry.id, false); });
}

const SubmenuController::Row* SubmenuController::findRow(int id) const {
  const auto found = std::ranges::find(m_rows, id, &Row::id);
  return found != m_rows.end() ? &*found : nullptr;
}

void SubmenuController::open(int id, bool keyboard) {
  m_closePending = false;
  if (m_open == id) {
    return;
  }
  const auto* row = findRow(id);
  if (row == nullptr || !m_page.popupSurface() || !m_page.contextMenu()) {
    return;
  }
  close();
  m_open = id;
  m_keyboard = keyboard;
  ++m_generation;
  // Filling can close every menu (popup roundtrip), which drops the rows.
  const auto fill = row->fill;
  fill();
}

bool SubmenuController::openHighlighted() {
  auto* menu = m_page.contextMenu();
  if (menu == nullptr) {
    return false;
  }
  const auto found = std::ranges::find(m_rows, menu->highlightedIndex(), &Row::index);
  if (found == m_rows.end()) {
    return false;
  }
  open(found->id, true);
  return true;
}

void SubmenuController::show(
    std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, float width
) {
  if (!m_open) {
    return;
  }
  const bool compact = m_page.settings().compactMenus;
  const float contentHeight = ContextMenuControl::preferredHeight(entries, entries.size(), 1.0F, compact);
  const float height = std::min(contentHeight, m_page.popupMaxHeight());
  if (!m_surface && !create(width, height)) {
    return;
  }
  const bool overflowing = contentHeight > height + 0.5F;
  applyContextMenuIconPreference(entries, m_page.settings().contextMenuIcons);
  m_menu->setMaxVisible(entries.size());
  m_menu->setEntries(std::move(entries));
  m_menu->setMenuWidth(width - (overflowing ? Style::scrollbarWidth + Style::scrollbarGap : 0.0F));
  m_menu->setHighlightedIndex(m_keyboard ? 0 : m_menu->entryCount());
  m_menu->setOnActivate([this, action = std::move(action)](const ContextMenuControlEntry& entry) {
    const int id = entry.id;
    const std::weak_ptr<int> lifetime = m_lifetime;
    // The action runs before the close: clipboard actions need the menu's keyboard focus for their serial.
    DeferredCall::callLater([lifetime, action, id] {
      if (!lifetime.expired()) {
        action(id);
      }
    });
    m_page.dismissPopup();
  });
  m_scroll->setReserveScrollbarGutter(overflowing);
  m_scroll->setSize(width, height);
  (void)m_surface->resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(std::ceil(height)), false);
  m_surface->requestLayout();
}

bool SubmenuController::create(float width, float height) {
  auto* menuSurface = m_page.popupSurface();
  auto* contextMenu = m_page.contextMenu();
  const auto* row = m_open ? findRow(*m_open) : nullptr;
  if (!menuSurface || !contextMenu || !row) {
    return false;
  }
  const float top = contextMenu->rowTop(row->index);
  const float bottom = contextMenu->rowBottom(row->index);
  float menuX = 0, menuY = 0;
  Node::absolutePosition(contextMenu, menuX, menuY);
  m_surface = std::make_unique<PopupSurface>(m_connection);
  m_surface->setRenderContext(&m_renderer);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setDismissedCallback([this] { dismiss(); });
  auto menu = std::make_unique<ContextMenuControl>();
  m_menu = menu.get();
  m_menu->setCompact(m_page.settings().compactMenus);
  auto scroll =
      ui::scrollView({.out = &m_scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .flexGrow = 1.0F});
  scroll->content()->addChild(std::move(menu));
  m_root = ui::column(
      {.fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh), .styleRadius = Style::radiusLg}, std::move(scroll)
  );
  m_root->setAnimationManager(&m_animations);
  m_surface->setSceneRoot(m_root.get());
  m_dispatcher.setSceneRoot(m_root.get());
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.extension<luciole::DesktopIntegration>().setCursorShape(serial, shape);
  });
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (auto* menu = m_page.contextMenu()) {
      menu->setSubmenuDirection(
          m_surface->configuredX() < 0 ? ContextSubmenuDirection::Left : ContextSubmenuDirection::Right
      );
      m_page.popupSurface()->requestLayout();
    }
    m_surface->requestLayout();
  });
  m_surface->setPrepareFrameCallback([this](bool, bool) {
    if (m_surface && m_renderer.makeCurrent(m_surface->renderTarget())) {
      UiPhaseScope phase(UiPhase::Layout);
      m_root->arrange(
          m_surface->renderTarget().renderer(),
          {0, 0, static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height())}
      );
    }
  });
  const PopupSurfaceConfig config{
      .anchorX = 0,
      .anchorY = static_cast<int>(std::floor(menuY + top)),
      .anchorWidth = static_cast<int>(menuSurface->width()),
      .anchorHeight = std::max(1, static_cast<int>(bottom - top)),
      .width = static_cast<std::uint32_t>(width),
      .height = static_cast<std::uint32_t>(std::ceil(height)),
      .anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
      .serial = m_page.popupSerial()
  };
  if (!m_surface->initializeAsChild(
          menuSurface->xdgSurface(), m_connection.outputForSurface(m_page.surface().wlSurface()), config
      )) {
    close();
    return false;
  }
  return true;
}

void SubmenuController::close() {
  if (m_open) {
    if (const auto* row = findRow(*m_open); row != nullptr && row->closed) {
      row->closed();
    }
  }
  if (auto* menu = m_page.contextMenu(); menu != nullptr && m_surface) {
    menu->setSubmenuDirection(ContextSubmenuDirection::Right);
  }
  ++m_generation;
  m_open.reset();
  m_closePending = false;
  m_dispatcher.setSceneRoot(nullptr);
  if (m_surface) {
    m_surface->setSceneRoot(nullptr);
  }
  m_root.reset();
  m_menu = nullptr;
  m_scroll = nullptr;
  m_surface.reset();
}

void SubmenuController::menuClosed() {
  close();
  m_rows.clear();
}

void SubmenuController::dismiss() {
  m_closePending = true;
  const auto generation = m_generation;
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime, generation] {
    if (!lifetime.expired() && generation == m_generation && m_closePending) {
      close();
    }
  });
}

bool SubmenuController::owns(const wl_surface* surface) const { return m_surface && m_surface->wlSurface() == surface; }

void SubmenuController::outputChanged(wl_output* output) {
  if (m_surface && m_connection.outputForSurface(m_surface->wlSurface()) == output) {
    m_surface->onSurfaceOutputEnter(m_surface->wlSurface(), output);
  }
}

bool SubmenuController::key(const KeyboardEvent& event) {
  if (!m_surface) {
    return false;
  }
  if (!event.pressed || event.preedit) {
    return true;
  }
  using K = luciole::KeybindCommand;
  const bool up = luciole::keybindMatches(K::MoveUp, event.sym, event.modifiers);
  const bool down = luciole::keybindMatches(K::MoveDown, event.sym, event.modifiers);
  if (luciole::keybindMatches(K::Cancel, event.sym, event.modifiers)
      || luciole::keybindMatches(K::MoveLeft, event.sym, event.modifiers)) {
    dismiss();
  } else if (luciole::keybindMatches(K::Validate, event.sym, event.modifiers)) {
    if (m_menu->highlightedIndex() >= m_menu->entryCount()) {
      (void)m_menu->moveHighlight(1);
    }
    (void)m_menu->activateHighlighted();
  } else if (up || down) {
    (void)m_menu->moveHighlight(up ? -1 : 1);
    const auto index = m_menu->highlightedIndex();
    const float top = m_menu->rowTop(index), bottom = m_menu->rowBottom(index);
    const float offset = m_scroll->scrollOffset();
    if (top < offset) {
      m_scroll->requestScrollToOffset(top);
    } else if (bottom > offset + m_scroll->height()) {
      m_scroll->requestScrollToOffset(bottom - m_scroll->height());
    }
    m_surface->requestLayout();
  }
  return true;
}

void SubmenuController::menuHover() {
  auto* menu = m_page.contextMenu();
  const auto found = menu ? std::ranges::find(m_rows, menu->highlightedIndex(), &Row::index) : m_rows.end();
  if (found != m_rows.end()) {
    m_keyboard = false;
    open(found->id, false);
  } else if (m_open) {
    dismiss();
  }
}

void SubmenuController::menuScrolled() {
  if (m_open) {
    dismiss();
  }
}
