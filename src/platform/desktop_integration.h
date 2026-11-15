#pragma once

#include "wayland/toplevel_surface.h"
#include "wayland/wayland_extension.h"

#include <functional>
#include <memory>
#include <string>

struct wl_cursor_theme;
struct wl_data_device_manager;
struct wl_surface;
struct zxdg_decoration_manager_v1;
struct PointerEvent;
class WaylandConnection;

namespace luciole {
  class LaunchActivation;
  class DesktopIntegration final : public WaylandExtension {
  public:
    explicit DesktopIntegration(WaylandConnection& connection);
    ~DesktopIntegration() override;
    void requestActivation(
        wl_surface* source, std::uint32_t serial, const std::string& appId, std::function<void(std::string)> completed
    );
    void activate(const std::string& token, wl_surface* surface);
    void onGlobal(wl_registry*, std::uint32_t, std::string_view, std::uint32_t) override;
    void onGlobalRemove(std::uint32_t name) override;
    void releaseGlobals() override;
    void pointer(const PointerEvent& event);
    void setCursorShape(std::uint32_t serial, std::uint32_t shape);
    std::uint32_t pointerEnterSerial() const { return m_enterSerial; }
    wl_data_device_manager* dataDeviceManager() const { return m_dataDeviceManager; }
    zxdg_decoration_manager_v1* decorationManager() const { return m_decorations; }

  private:
    WaylandConnection& m_connection;
    wl_data_device_manager* m_dataDeviceManager = nullptr;
    zxdg_decoration_manager_v1* m_decorations = nullptr;
    wl_cursor_theme* m_cursorTheme = nullptr;
    wl_surface* m_cursorSurface = nullptr;
    std::uint32_t m_dataGlobal = 0, m_decorationGlobal = 0, m_shapeGlobal = 0;
    std::uint32_t m_enterSerial = 0, m_activationGlobal = 0;
    std::unique_ptr<LaunchActivation> m_activation;
  };

  class WindowSurface final : public ToplevelSurface {
  public:
    explicit WindowSurface(WaylandConnection& connection);
    ~WindowSurface() override;
    bool serverDecorated() const;
    // rendered runs after each drawn frame; frameDone after each completed frame callback and may destroy the surface.
    void setFrameCallbacks(std::function<void()> rendered, std::function<void()> frameDone);

  protected:
    void render() override;
    void onFrameCallbackDone() override;

  private:
    struct Decoration;
    std::shared_ptr<Decoration> m_decoration;
    std::function<void()> m_rendered;
    std::function<void()> m_frameDone;
  };
} // namespace luciole
