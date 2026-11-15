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
} // namespace luciole

struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
class SettingsDialog;
class PageContext;
class RenderContext;
class SelectDropdownPopup;
class WaylandConnection;

// Settings: the settings dialog, its dropdowns, and applying saved preference changes to the page.
class SettingsController final : public SelectPopupContext {
public:
  SettingsController(
      PageContext& page, luciole::ViewPreferenceWriter& preferences, luciole::ThumbnailService& thumbnails,
      WaylandConnection& connection, RenderContext& renderer, std::function<void()> clearThumbnails
  );
  ~SettingsController() override;

  // Opens the Settings dialog, or closes it when it is already open.
  void toggle();
  // True while the Settings dialog is shown.
  bool open() const { return m_view != nullptr; }
  // Preferences were saved or reloaded; `error` is empty when that worked.
  void preferencesChanged(const std::string& error);
  // Thumbnail cache state changed: sync on the next frame.
  void markDirty() { m_dirty = true; }
  // The last preference load or save error; empty when there is none.
  const std::string& error() const { return m_error; }
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // Dialog layout hook with the dialog's width: compact navigation on narrow windows.
  void layoutDialog(float width);
  // Keyboard hook for the dialog popup: the open dropdown and the Settings shortcut. False lets the dialog handle it.
  bool key(const KeyboardEvent& event);
  // True when `surface` is the open dropdown's popup.
  bool dropdownOwns(const wl_surface* surface) const;
  // Pointer hook in BrowserPage::pointer(): feeds the open dropdown; a press elsewhere only closes it.
  bool dropdownPointer(const PointerEvent& event);

  void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override;
  void closeSelectDropdown() override;
  bool isSelectDropdownOpen() const override;

private:
  void sync();
  // Dialog close: drops the dropdown before its parent popup and forgets the view.
  void teardown();

  PageContext& m_page;
  luciole::ViewPreferenceWriter& m_preferences;
  luciole::ThumbnailService& m_thumbnails;
  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  std::function<void()> m_clearThumbnails;
  SettingsDialog* m_view = nullptr;
  std::unique_ptr<SelectDropdownPopup> m_dropdown;
  std::optional<luciole::Settings> m_pending;
  std::string m_error;
  bool m_dirty = false;
  // Lives while the view is shown; dropdown callbacks check it.
  std::shared_ptr<int> m_viewLifetime;
};
