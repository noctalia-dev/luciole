#include "app/page/dialog_dropdown.h"

#include "app/page/page_context.h"
#include "platform/desktop_integration.h"
#include "ui/controls/select_dropdown_popup.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <utility>

DialogDropdown::DialogDropdown(PageContext& page, WaylandConnection& connection, RenderContext& renderer)
    : m_page(page), m_connection(connection), m_renderer(renderer) {}

DialogDropdown::~DialogDropdown() = default;

bool DialogDropdown::key(const KeyboardEvent& event) {
  if (!isSelectDropdownOpen()) {
    return false;
  }
  m_dropdown->onKeyboardEvent(event);
  return true;
}

bool DialogDropdown::pointer(const PointerEvent& event) {
  if (!isSelectDropdownOpen()) {
    return false;
  }
  if (event.surface == m_dropdown->wlSurface()) {
    (void)m_dropdown->onPointerEvent(event);
    return true;
  }
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    // Like a native select: a press outside only closes the dropdown.
    m_dropdown->closeSelectDropdown();
    return true;
  }
  return false;
}

bool DialogDropdown::owns(const wl_surface* surface) const {
  return isSelectDropdownOpen() && m_dropdown->wlSurface() == surface;
}

void DialogDropdown::reset() {
  if (m_dropdown) {
    m_dropdown->closeSelectDropdown();
    m_dropdown.reset();
  }
  m_dialogLifetime.reset();
}

void DialogDropdown::openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) {
  auto* dialog = m_page.popupSurface();
  if (!dialog) {
    if (callbacks.onDismiss) {
      callbacks.onDismiss();
    }
    return;
  }
  if (!m_dropdown) {
    m_dropdown = std::make_unique<SelectDropdownPopup>(m_connection, m_renderer);
  }
  if (!m_dialogLifetime) {
    m_dialogLifetime = std::make_shared<int>(0);
  }
  m_dropdown->setParent(
      dialog->xdgSurface(), dialog->wlSurface(), m_connection.outputForSurface(m_page.surface().wlSurface())
  );
  const std::weak_ptr<int> lifetime = m_dialogLifetime;
  m_dropdown->openSelectDropdown(
      request,
      {.onSelect =
           [lifetime, select = std::move(callbacks.onSelect)](std::size_t index) {
             if (!lifetime.expired() && select) {
               select(index);
             }
           },
       .onDismiss =
           [lifetime, dismiss = std::move(callbacks.onDismiss)] {
             if (!lifetime.expired() && dismiss) {
               dismiss();
             }
           }}
  );
}

void DialogDropdown::closeSelectDropdown() {
  if (m_dropdown) {
    m_dropdown->closeSelectDropdown();
  }
}

bool DialogDropdown::isSelectDropdownOpen() const { return m_dropdown && m_dropdown->isSelectDropdownOpen(); }
