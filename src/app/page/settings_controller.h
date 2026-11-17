#pragma once

#include "config/settings.h"
#include "ui/controls/select_popup_context.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace luciole {
  class ThumbnailService;
  class ViewPreferenceWriter;
  class WindowSurface;
} // namespace luciole

struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
class DialogWindow;
class SettingsDialog;
class PageContext;
class RenderContext;
class SelectDropdownPopup;
class WaylandConnection;

// Settings: the Settings window, its dropdowns, and applying saved preference changes to the page.
class SettingsController final : public SelectPopupContext {
public:
  SettingsController(
      PageContext& page, luciole::ViewPreferenceWriter& preferences, luciole::ThumbnailService& thumbnails,
      WaylandConnection& connection, RenderContext& renderer, std::function<void()> clearThumbnails
  );
  ~SettingsController() override;

  // Opens the Settings window, or closes it when it is already open. Opening it closes another page's.
  void toggle();
  // True while this page's Settings window is shown.
  bool open() const { return m_view != nullptr; }
  // Closes the window now.
  void close();
  // The page moved to another browser window.
  void setParent(luciole::WindowSurface& parent);
  // Preferences were saved or reloaded; `error` is empty when that worked.
  void preferencesChanged(const std::string& error);
  // Thumbnail cache state changed: sync on the next frame.
  void markDirty();
  // The last preference load or save error; empty when there is none.
  const std::string& error() const { return m_error; }
  // Frame slot in BrowserPage::prepareFrame(): applies saved preferences to the page.
  void onFrame();

  void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override;
  void closeSelectDropdown() override;
  bool isSelectDropdownOpen() const override;

private:
  void sync();
  void markViewDirty();
  // The window's frame: syncs the view, so it stays current while its page's tab is hidden.
  void frame(float width);
  // Compact navigation on narrow windows.
  void layoutDialog(float width);
  // Window keys: the open dropdown and the Settings shortcut. False lets the window handle it.
  bool key(const KeyboardEvent& event);
  // True when `surface` is the open dropdown's popup.
  bool dropdownOwns(const wl_surface* surface) const;
  // Window pointer events: feeds the open dropdown; a press elsewhere only closes it.
  bool dropdownPointer(const PointerEvent& event);

  PageContext& m_page;
  luciole::ViewPreferenceWriter& m_preferences;
  luciole::ThumbnailService& m_thumbnails;
  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  std::function<void()> m_clearThumbnails;
  SettingsDialog* m_view = nullptr;
  std::unique_ptr<DialogWindow> m_window;
  std::unique_ptr<SelectDropdownPopup> m_dropdown;
  std::optional<luciole::Settings> m_pending;
  std::string m_error;
  bool m_dirty = false;
  bool m_viewDirty = false;
  // Lives while the view is shown; dropdown callbacks check it.
  std::shared_ptr<int> m_viewLifetime;
};
