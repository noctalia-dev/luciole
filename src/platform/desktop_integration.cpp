#include "platform/desktop_integration.h"

#include "cursor-shape-v1-client-protocol.h"
#include "platform/launch_activation.h"
#include "platform/modal_dialog.h"
#include "wayland/wayland_connection.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <wayland-cursor.h>

luciole::DesktopIntegration::DesktopIntegration(WaylandConnection& connection) : m_connection(connection) {}

luciole::DesktopIntegration::~DesktopIntegration() = default;

void luciole::DesktopIntegration::requestActivation(
    wl_surface* source, std::uint32_t serial, const std::string& appId, std::function<void(std::string)> completed
) {
  if (m_activation) {
    m_activation->request(source, m_connection.seat(), serial, appId, std::move(completed));
  } else {
    completed({});
  }
}

void luciole::DesktopIntegration::activate(const std::string& token, wl_surface* surface) {
  if (m_activation) {
    m_activation->activate(token, surface);
  }
}

void luciole::DesktopIntegration::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == "wl_data_device_manager" && !m_dataDeviceManager) {
    m_dataDeviceManager = static_cast<wl_data_device_manager*>(
        wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3U))
    );
    m_dataGlobal = name;
  } else if (interface == "zxdg_decoration_manager_v1" && !m_decorations) {
    m_decorations = static_cast<zxdg_decoration_manager_v1*>(
        wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1)
    );
    m_decorationGlobal = name;
  } else if (interface == "zxdg_importer_v2" && !m_importer) {
    m_importer = static_cast<zxdg_importer_v2*>(wl_registry_bind(registry, name, &zxdg_importer_v2_interface, 1));
    m_importerGlobal = name;
  } else if (interface == "xdg_wm_dialog_v1" && !m_dialogs) {
    m_dialogs = static_cast<xdg_wm_dialog_v1*>(wl_registry_bind(registry, name, &xdg_wm_dialog_v1_interface, 1));
    m_dialogGlobal = name;
  } else if (interface == "xdg_activation_v1" && !m_activation) {
    auto* manager = static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
    m_activation = std::make_unique<LaunchActivation>(manager);
    m_activationGlobal = name;
  } else if (interface == "wp_cursor_shape_manager_v1") {
    m_shapeGlobal = name;
  }
}
void luciole::DesktopIntegration::onGlobalRemove(std::uint32_t name) {
  if (name == m_activationGlobal) {
    m_activationGlobal = 0;
    m_activation.reset();
  }
  if (name == m_dataGlobal) {
    wl_data_device_manager_destroy(m_dataDeviceManager);
    m_dataDeviceManager = nullptr;
    m_dataGlobal = 0;
  }
  if (name == m_decorationGlobal) {
    zxdg_decoration_manager_v1_destroy(m_decorations);
    m_decorations = nullptr;
    m_decorationGlobal = 0;
  }
  if (name == m_importerGlobal) {
    zxdg_importer_v2_destroy(m_importer);
    m_importer = nullptr;
    m_importerGlobal = 0;
  }
  if (name == m_dialogGlobal) {
    xdg_wm_dialog_v1_destroy(m_dialogs);
    m_dialogs = nullptr;
    m_dialogGlobal = 0;
  }
  if (name == m_shapeGlobal) {
    m_shapeGlobal = 0;
  }
}
void luciole::DesktopIntegration::releaseGlobals() {
  m_activation.reset();
  m_activationGlobal = 0;
  if (m_cursorSurface) {
    wl_surface_destroy(m_cursorSurface);
    m_cursorSurface = nullptr;
  }
  if (m_cursorTheme) {
    wl_cursor_theme_destroy(m_cursorTheme);
    m_cursorTheme = nullptr;
  }
  if (m_dataDeviceManager) {
    wl_data_device_manager_destroy(m_dataDeviceManager);
    m_dataDeviceManager = nullptr;
  }
  if (m_decorations) {
    zxdg_decoration_manager_v1_destroy(m_decorations);
    m_decorations = nullptr;
  }
  if (m_importer) {
    zxdg_importer_v2_destroy(m_importer);
    m_importer = nullptr;
  }
  m_importerGlobal = 0;
  if (m_dialogs) {
    xdg_wm_dialog_v1_destroy(m_dialogs);
    m_dialogs = nullptr;
  }
  m_dialogGlobal = 0;
}
void luciole::DesktopIntegration::pointer(const PointerEvent& event) {
  if (event.type == PointerEvent::Type::Enter) {
    m_enterSerial = event.serial;
    setCursorShape(event.serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
  } else if (event.type == PointerEvent::Type::Leave) {
    m_enterSerial = 0;
  }
}
void luciole::DesktopIntegration::setCursorShape(std::uint32_t serial, std::uint32_t shape) {
  if (m_shapeGlobal) {
    m_connection.setCursorShape(serial, shape);
    return;
  }
  const auto enterSerial = m_enterSerial != 0 ? m_enterSerial : serial;
  if (!enterSerial || !m_connection.pointer() || !m_connection.compositor() || !m_connection.shm()) {
    return;
  }
  if (!m_cursorTheme) {
    m_cursorTheme = wl_cursor_theme_load(nullptr, 24, m_connection.shm());
  }
  if (!m_cursorSurface) {
    m_cursorSurface = wl_compositor_create_surface(m_connection.compositor());
  }
  if (!m_cursorTheme || !m_cursorSurface) {
    return;
  }
  const char* name = "left_ptr";
  switch (shape) {
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT:
    name = "xterm";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER:
    name = "hand2";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY:
    name = "copy";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU:
    name = "context-menu";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE:
    name = "move";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP:
    name = "no-drop";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE:
    name = "sb_h_double_arrow";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE:
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE:
    name = "sb_v_double_arrow";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE:
    name = "top_left_corner";
    break;
  case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE:
    name = "top_right_corner";
    break;
  default:
    break;
  }
  auto* cursor = wl_cursor_theme_get_cursor(m_cursorTheme, name);
  if (!cursor || cursor->image_count == 0) {
    return;
  }
  auto* image = cursor->images[0];
  wl_pointer_set_cursor(
      m_connection.pointer(), enterSerial, m_cursorSurface, static_cast<int>(image->hotspot_x),
      static_cast<int>(image->hotspot_y)
  );
  wl_surface_attach(m_cursorSurface, wl_cursor_image_get_buffer(image), 0, 0);
  wl_surface_damage(m_cursorSurface, 0, 0, static_cast<int>(image->width), static_cast<int>(image->height));
  wl_surface_commit(m_cursorSurface);
}

struct luciole::WindowSurface::Decoration {
  zxdg_toplevel_decoration_v1* role = nullptr;
  WindowSurface* surface = nullptr;
  bool server = false;
  bool desired = true; // What Luciole asks for: server-side when set.
  std::uint32_t mode() const {
    return desired ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
  }
  void release() {
    if (role) {
      zxdg_toplevel_decoration_v1_destroy(role);
      role = nullptr;
    }
    server = false;
  }
};
struct luciole::WindowSurface::Parent {
  std::string handle;
  bool modal = false;
  zxdg_imported_v2* imported = nullptr;
  std::unique_ptr<ModalDialog> dialog;
  WindowSurface* surface = nullptr;
  void attach(DesktopIntegration& desktop, xdg_toplevel* role, wl_display* display) {
    constexpr std::string_view kWayland = "wayland:";
    if (!handle.starts_with(kWayland) || !desktop.importer() || !surface) {
      return;
    }
    imported = zxdg_importer_v2_import_toplevel(desktop.importer(), handle.substr(kWayland.size()).c_str());
    static const zxdg_imported_v2_listener listener{.destroyed = [](void* data, zxdg_imported_v2* proxy) {
      zxdg_imported_v2_destroy(proxy);
      static_cast<Parent*>(data)->imported = nullptr;
    }};
    zxdg_imported_v2_add_listener(imported, &listener, this);
    zxdg_imported_v2_set_parent_of(imported, surface->wlSurface());
    if (modal && desktop.dialogManager()) {
      dialog = std::make_unique<ModalDialog>(display, xdg_wm_dialog_v1_get_xdg_dialog(desktop.dialogManager(), role));
    }
  }
  void release() {
    dialog.reset();
    if (imported) {
      zxdg_imported_v2_destroy(imported);
      imported = nullptr;
    }
  }
};
luciole::WindowSurface::WindowSurface(WaylandConnection& connection)
    : ToplevelSurface(connection), m_decoration(std::make_shared<Decoration>()), m_parent(std::make_shared<Parent>()) {
  m_decoration->surface = this;
  m_parent->surface = this;
  auto& desktop = connection.extension<DesktopIntegration>();
  setRoleLifecycleCallbacks(
      [state = m_decoration, parent = m_parent, &desktop, &connection](xdg_toplevel* role) {
        if (auto* manager = desktop.decorationManager()) {
          state->role = zxdg_decoration_manager_v1_get_toplevel_decoration(manager, role);
          static const zxdg_toplevel_decoration_v1_listener listener{
              .configure = [](void* data, zxdg_toplevel_decoration_v1*, std::uint32_t mode) {
                auto& state = *static_cast<Decoration*>(data);
                state.server = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
                if (state.surface) {
                  state.surface->requestLayout();
                }
              }
          };
          zxdg_toplevel_decoration_v1_add_listener(state->role, &listener, state.get());
          zxdg_toplevel_decoration_v1_set_mode(state->role, state->mode());
        }
        parent->attach(desktop, role, connection.display());
      },
      [state = m_decoration, parent = m_parent] {
        parent->release();
        state->release();
      }
  );
}
luciole::WindowSurface::~WindowSurface() {
  m_decoration->surface = nullptr;
  m_parent->surface = nullptr;
  m_parent->release();
  m_decoration->release();
}
bool luciole::WindowSurface::serverDecorated() const { return m_decoration->desired && m_decoration->server; }

bool luciole::WindowSurface::supportsSSD() const { return m_decoration->role != nullptr; }

void luciole::WindowSurface::setUseSSD(bool enabled) {
  if (m_decoration->desired == enabled) {
    return;
  }
  m_decoration->desired = enabled;
  if (m_decoration->role) {
    zxdg_toplevel_decoration_v1_set_mode(m_decoration->role, m_decoration->mode());
  }
}

void luciole::WindowSurface::setParent(std::string handle, bool modal) {
  m_parent->handle = std::move(handle);
  m_parent->modal = modal;
}

void luciole::WindowSurface::completeModal(std::function<void(bool)> ready) {
  if (m_parent->dialog) {
    m_parent->dialog->complete(std::move(ready));
  } else {
    ready(true);
  }
}

bool luciole::WindowSurface::modalReplyPending() const { return m_parent->dialog && m_parent->dialog->pending(); }

void luciole::WindowSurface::setFrameCallbacks(std::function<void()> rendered, std::function<void()> frameDone) {
  m_rendered = std::move(rendered);
  m_frameDone = std::move(frameDone);
}

void luciole::WindowSurface::render() {
  ToplevelSurface::render();
  if (m_rendered) {
    m_rendered();
  }
}

void luciole::WindowSurface::onFrameCallbackDone() {
  ToplevelSurface::onFrameCallbackDone();
  // Last: the callback may destroy this surface.
  if (const auto callback = m_frameDone) {
    callback();
  }
}
