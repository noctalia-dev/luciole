#pragma once

#include "ui/controls/select_popup_context.h"

#include <memory>

struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
class PageContext;
class RenderContext;
class SelectDropdownPopup;
class WaylandConnection;

// Select dropdowns of the open dialog: a child popup of the dialog surface. Dialog roots opt in with
// setPopupContext(&page.dialogDropdown()); BrowserPage routes the dropdown's input and closes it with the dialog.
class DialogDropdown final : public SelectPopupContext {
public:
  DialogDropdown(PageContext& page, WaylandConnection& connection, RenderContext& renderer);
  ~DialogDropdown() override;

  // Feeds the open dropdown. False when no dropdown is open.
  bool key(const KeyboardEvent& event);
  // Feeds the open dropdown; a press elsewhere only closes it. False when the event is not the dropdown's.
  bool pointer(const PointerEvent& event);
  // True when `surface` is the open dropdown's popup.
  bool owns(const wl_surface* surface) const;
  // Dialog close: drops the dropdown before its parent popup, and the callbacks of the closed dialog.
  void reset();

  void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override;
  void closeSelectDropdown() override;
  bool isSelectDropdownOpen() const override;

private:
  PageContext& m_page;
  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  std::unique_ptr<SelectDropdownPopup> m_dropdown;
  // Lives until the dialog that opened the dropdown closes; dropdown callbacks check it.
  std::shared_ptr<int> m_dialogLifetime;
};
