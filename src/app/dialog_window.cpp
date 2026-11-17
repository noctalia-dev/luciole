#include "app/dialog_window.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "ui/controls/flex.h"
#include "ui/dialogs/dialog_chrome.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <utility>

namespace {
  // Small enough for half a laptop screen, large enough that every dialog still lays out.
  constexpr std::uint32_t kMinWidth = 360;
  constexpr std::uint32_t kMinHeight = 240;
} // namespace

DialogWindow::DialogWindow(DialogWindows& windows, luciole::WindowSurface& parent)
    : m_windows(windows), m_surface(windows.m_connection), m_parent(&parent) {
  m_windows.add(*this);
}

DialogWindow::~DialogWindow() {
  m_windows.remove(*this);
  m_dispatcher.setTextInputContext(nullptr, nullptr);
  m_dispatcher.setSceneRoot(nullptr);
  m_surface.setSceneRoot(nullptr);
}

bool DialogWindow::open(std::unique_ptr<Flex> root, const DialogWindowOptions& options) {
  m_root = std::move(root);
  m_group = options.group;
  // The window's own edges replace the in-window dialog's border and rounded corners.
  m_root->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), 0.0F);
  m_root->setRadius(0.0F);
  m_root->setAnimationManager(&m_windows.m_animations);
  m_surface.setRenderContext(&m_windows.m_renderer);
  m_surface.setAnimationManager(&m_windows.m_animations);
  m_surface.setSceneRoot(m_root.get());
  m_dispatcher.setSceneRoot(m_root.get());
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_windows.m_connection.extension<luciole::DesktopIntegration>().setCursorShape(serial, shape);
  });
  m_surface.setPrepareFrameCallback([this](bool, bool) {
    UiPhaseScope phase(UiPhase::Layout);
    if (!m_windows.m_renderer.makeCurrent(m_surface.renderTarget())) {
      return;
    }
    const auto width = static_cast<float>(m_surface.width());
    if (m_layout) {
      m_layout(width);
    }
    m_root->arrange(m_surface.renderTarget().renderer(), {0, 0, width, static_cast<float>(m_surface.height())});
  });
  m_surface.setUpdateCallback([] {});
  m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface.requestLayout(); });
  m_surface.setClosedCallback([this] { dismiss(); });
  auto& connection = m_windows.m_connection;
  if (!m_surface.initialize(
          connection.outputForSurface(m_parent->wlSurface()),
          {.width = std::max(options.width, kMinWidth),
           .height = std::max(options.height, kMinHeight),
           .minWidth = kMinWidth,
           .minHeight = kMinHeight,
           .title = options.title,
           .appId = "dev.noctalia.luciole"}
      )) {
    return false;
  }
  // Before the first buffer, so the compositor maps the window as the browser window's dialog.
  xdg_toplevel_set_parent(m_surface.xdgToplevel(), m_parent->xdgToplevel());
  m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_windows.m_input);
  m_windows.claimGroup(*this);
  luciole::focusFirstDialogControl(m_dispatcher);
  m_surface.requestLayout();
  return true;
}

void DialogWindow::setParent(luciole::WindowSurface& parent) {
  m_parent = &parent;
  if (m_surface.xdgToplevel() != nullptr) {
    xdg_toplevel_set_parent(m_surface.xdgToplevel(), parent.xdgToplevel());
    m_surface.requestRedraw();
  }
}

void DialogWindow::dismiss() {
  if (std::exchange(m_dismissing, true)) {
    return;
  }
  DeferredCall::callLater([this, lifetime = std::weak_ptr<int>(m_lifetime)] {
    if (lifetime.expired()) {
      return;
    }
    // A copy: the callback usually destroys this window.
    if (const auto closed = m_closed) {
      closed();
    }
  });
}

void DialogWindow::requestLayout() { m_surface.requestLayout(); }

bool DialogWindow::owns(const wl_surface* surface) const {
  return surface != nullptr && (m_surface.wlSurface() == surface || (m_ownsSurface && m_ownsSurface(surface)));
}

void DialogWindow::keyboard(const KeyboardEvent& event) {
  if (m_keyHandler && m_keyHandler(event)) {
    return;
  }
  if (event.pressed && KeySymbol::isEscape(event.sym)) {
    dismiss();
  } else if (event.pressed && KeySymbol::isTab(event.sym)) {
    (void)m_dispatcher.cycleTabFocus((event.modifiers & KeyMod::Shift) != 0);
  } else {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  }
  m_surface.requestUpdate();
}

void DialogWindow::pointer(const PointerEvent& event) {
  if (m_pointerHandler && m_pointerHandler(event)) {
    return;
  }
  if (event.surface == m_surface.wlSurface()) {
    dispatchPointerEvent(m_dispatcher, event);
  }
}

void DialogWindow::focusChanged(bool entered) {
  if (entered) {
    m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_windows.m_input);
  } else {
    m_dispatcher.pointerLeave();
  }
  m_surface.requestUpdate();
}

void DialogWindow::outputChanged(wl_output* output) {
  if (m_windows.m_connection.outputForSurface(m_surface.wlSurface()) == output) {
    m_surface.onSurfaceOutputEnter(m_surface.wlSurface(), output);
  }
}

DialogWindows::DialogWindows(
    WaylandConnection& connection, RenderContext& renderer, TextInputService& input, AnimationManager& animations
)
    : m_connection(connection), m_renderer(renderer), m_input(input), m_animations(animations) {}

bool DialogWindows::keyboard(const wl_surface* focused, const KeyboardEvent& event) {
  auto* window = find(focused);
  if (window == nullptr) {
    return false;
  }
  window->keyboard(event);
  return true;
}

bool DialogWindows::pointer(const PointerEvent& event) {
  auto* window = find(event.surface);
  if (window == nullptr) {
    return false;
  }
  window->pointer(event);
  return true;
}

bool DialogWindows::focus(const wl_surface* surface, bool entered) {
  auto* window = find(surface);
  if (window == nullptr) {
    return false;
  }
  window->focusChanged(entered);
  return true;
}

void DialogWindows::outputChanged(wl_output* output) {
  for (auto* window : m_windows) {
    window->outputChanged(output);
  }
}

void DialogWindows::add(DialogWindow& window) { m_windows.push_back(&window); }

void DialogWindows::remove(DialogWindow& window) { std::erase(m_windows, &window); }

void DialogWindows::claimGroup(DialogWindow& window) {
  if (window.group().empty()) {
    return;
  }
  for (auto* other : m_windows) {
    if (other != &window && other->group() == window.group()) {
      other->dismiss();
    }
  }
}

DialogWindow* DialogWindows::find(const wl_surface* surface) const {
  const auto found = std::ranges::find_if(m_windows, [surface](const auto* window) { return window->owns(surface); });
  return found == m_windows.end() ? nullptr : *found;
}

void dispatchPointerEvent(InputDispatcher& dispatcher, const PointerEvent& event) {
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
