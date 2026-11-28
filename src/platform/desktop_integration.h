#pragma once

#include "wayland/toplevel_surface.h"
#include "wayland/wayland_extension.h"

#include <functional>
#include <memory>
#include <string>

struct wl_cursor_theme;
struct wl_data_device_manager;
struct wl_surface;
struct xdg_wm_dialog_v1;
struct zxdg_decoration_manager_v1;
struct zxdg_importer_v2;
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
    zxdg_importer_v2* importer() const { return m_importer; }
    xdg_wm_dialog_v1* dialogManager() const { return m_dialogs; }

  private:
    WaylandConnection& m_connection;
    wl_data_device_manager* m_dataDeviceManager = nullptr;
    zxdg_decoration_manager_v1* m_decorations = nullptr;
    zxdg_importer_v2* m_importer = nullptr;
    xdg_wm_dialog_v1* m_dialogs = nullptr;
    wl_cursor_theme* m_cursorTheme = nullptr;
    wl_surface* m_cursorSurface = nullptr;
    std::uint32_t m_dataGlobal = 0, m_decorationGlobal = 0, m_shapeGlobal = 0;
    std::uint32_t m_importerGlobal = 0, m_dialogGlobal = 0;
    std::uint32_t m_enterSerial = 0, m_activationGlobal = 0;
    std::unique_ptr<LaunchActivation> m_activation;
  };

  class WindowSurface final : public ToplevelSurface {
  public:
    explicit WindowSurface(WaylandConnection& connection);
    ~WindowSurface() override;
    bool serverDecorated() const;
    bool supportsSSD() const;
    // Asks the compositor to leave the decorations to Luciole (false) or to draw them (true). Takes effect on an
    // existing window too; the compositor's reply arrives as a layout request.
    void setUseSSD(bool enabled);
    // Parents the window to a portal `parent_window` handle; call before initialize(). Only "wayland:"
    // handles take effect; `modal` asks the compositor to treat the window as a modal dialog of it.
    void setParent(std::string handle, bool modal);
    // Finish modality before replying to a portal client, while retaining its imported parent.
    void completeModal(std::function<void(bool)> ready);
    bool modalReplyPending() const;
    // rendered runs after each drawn frame; frameDone after each completed frame callback and may destroy the surface.
    void setFrameCallbacks(std::function<void()> rendered, std::function<void()> frameDone);

  protected:
    void render() override;
    void onFrameCallbackDone() override;

  private:
    struct Decoration;
    struct Parent;
    std::shared_ptr<Decoration> m_decoration;
    std::shared_ptr<Parent> m_parent;
    std::function<void()> m_rendered;
    std::function<void()> m_frameDone;
  };
} // namespace luciole
