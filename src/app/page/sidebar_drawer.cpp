#include "app/page/drag_drop_controller.h"
#include "app/page/page_context.h"
#include "app/page/sidebar_controller.h"
#include "browser/browser_controller.h"
#include "config/keybinds.h"
#include "config/view_preference_writer.h"
#include "core/input/key_modifiers.h"
#include "platform/desktop_integration.h"
#include "render/animation/animation_manager.h"
#include "ui/browser/places_sidebar.h"
#include "ui/builders.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

SidebarController::~SidebarController() { m_animations.cancelForOwner(this); }

std::unique_ptr<Button> SidebarController::buildToggle() {
  return ui::button(
      {.out = &m_toggle,
       .glyph = "layout-sidebar",
       .glyphSize = Style::baseGlyphSize,
       .variant = ButtonVariant::Ghost,
       .onClick = [this] {
         toggle();
         // A pointer click leaves no keyboard focus behind; give it to the file list so shortcuts like F2 work.
         if (!m_state.compact()) {
           m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
         }
       }}
  );
}

void SidebarController::toggle() {
  if (!m_state.compact()) {
    setVisible(!visible());
    return;
  }
  (void)m_state.toggle();
  m_focusDrawer = visible();
  if (!visible() && m_toggle) {
    m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  }
  revealDrawer();
}

void SidebarController::dismissDrawer() {
  if (!drawerOpen()) {
    return;
  }
  m_state.dismiss();
  m_focusDrawer = false;
  m_restorePlaceFocus.reset();
  m_toggle->setSelected(false);
  m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  revealDrawer();
}

void SidebarController::revealDrawer() {
  m_animations.cancelForOwner(this);
  const float target = visible() ? 1.0F : 0.0F;
  const float shade = drawerOpen() ? 0.5F : 0.0F;
  const float from = m_reveal, fromShade = m_scrimOpacity;
  m_animations.animate(
      0.0F, 1.0F, 180.0F, Easing::EaseOutCubic,
      [this, from, target, fromShade, shade](float progress) {
        m_reveal = std::lerp(from, target, progress);
        m_scrimOpacity = std::lerp(fromShade, shade, progress);
        if (m_sidebarPanel) {
          m_sidebarPanel->setTranslate(-(1.0F - m_reveal) * m_sidebarPanel->width(), 0);
          m_scrimFill->setOpacity(m_scrimOpacity);
        }
        if (m_state.compact()) {
          m_page.surface().requestRedraw();
        } else {
          m_page.surface().requestLayout();
        }
      },
      [this] { m_page.surface().requestLayout(); }, this
  );
  m_page.surface().requestLayout();
}

void SidebarController::prepareWidth(float width) {
  if (!m_state.updateWidth(width)) {
    return;
  }
  m_page.dispatcher().cancelPointerCapture();
  m_drags.clearExternalDrop();
  m_animations.cancelForOwner(this);
  m_focusDrawer = false;
  m_page.captureViewAnchor();
  if (m_places && !visible()) {
    for (const Node* node = m_page.dispatcher().focusedArea(); node; node = node->parent()) {
      if (node == m_sidebarBundle) {
        m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
        break;
      }
    }
  }
  revealDrawer();
}

bool SidebarController::drawerContains(float x, float y) const {
  return m_state.compact()
      && (visible() || m_reveal > 0)
      && m_sidebarBundle
      && m_sidebarBundle->visible()
      && m_sidebarBundle->containsScenePoint(x, y);
}

void SidebarController::arrange(Renderer& renderer, float top, float height) {
  layout();
  const bool compact = m_state.compact();
  const bool show = visible() || m_reveal > 0;
  m_sidebarBundle->setVisible(show);
  m_sidebarBundle->setZIndex(compact ? 60 : 30);
  const float controls = compact ? static_cast<float>(std::min(m_sidebarWidth, maximumWidth())) : controlWidth();
  // Position the bundle without Node::arrange: its default lays the panel out at natural height,
  // which clamps the places scroll offset to zero before the real arrange below.
  m_sidebarBundle->setPosition(0, top);
  m_sidebarBundle->setFrameSize(
      compact ? static_cast<float>(m_page.surface().width()) : std::max(0.0F, controls), height
  );
  m_drawerScrim->setVisible(compact && show);
  m_drawerScrim->arrange(renderer, {0, 0, m_sidebarBundle->width(), height});
  m_scrimFill->arrange(renderer, {0, 0, m_sidebarBundle->width(), height});
  m_scrimFill->setOpacity(m_scrimOpacity);
  if (compact) {
    m_sidebarPanel->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
  } else {
    m_sidebarPanel->clearFill();
  }
  m_sidebarPanel->arrange(renderer, {0, 0, std::max(0.0F, controls), height});
  m_sidebarPanel->setTranslate(-(1.0F - m_reveal) * controls, 0);
}

void SidebarController::focusDrawer() {
  if (!m_focusDrawer || !drawerOpen() || !m_page.active()) {
    return;
  }
  InputArea* area = nullptr;
  if (const auto& location = m_page.browser().model().snapshot().location; location) {
    area = m_places->focusArea(*location);
  }
  if (!area) {
    area = m_page.dispatcher().firstTabFocusUnder(m_places);
  }
  if (area) {
    m_focusDrawer = false;
    m_page.dispatcher().setFocus(area);
  }
}

bool SidebarController::key(const KeyboardEvent& event) {
  if (!drawerOpen() || m_page.popupSurface() || m_drags.dragging()) {
    return false;
  }
  if (event.pressed && !event.preedit) {
    using K = luciole::KeybindCommand;
    if (luciole::keybindMatches(K::Cancel, event.sym, event.modifiers)
        || luciole::keybindMatches(K::ToggleSidebar, event.sym, event.modifiers)) {
      dismissDrawer();
      return true;
    }
    const bool focusPrevious = luciole::keybindMatches(K::FocusPrevious, event.sym, event.modifiers);
    if (focusPrevious || luciole::keybindMatches(K::FocusNext, event.sym, event.modifiers)) {
      (void)m_page.dispatcher().cycleTabFocusInSubtree(m_places, focusPrevious);
      m_page.surface().requestRedraw();
      return true;
    }
    if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt)) != 0) {
      dismissDrawer();
      return false;
    }
  }
  m_page.dispatcher().keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  return true;
}
