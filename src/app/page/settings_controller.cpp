#include "app/page/settings_controller.h"

#include "app/dialog_window.h"
#include "app/page/page_context.h"
#include "config/view_preference_writer.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/dialogs/settings_dialog.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <utility>

namespace {
  // The in-window dialog's height cap, which the layout was designed for.
  constexpr float kMaxHeight = 640.0F;
  // Narrower windows switch the navigation to its compact form.
  constexpr float kCompactWidth = 720.0F;
} // namespace

SettingsController::SettingsController(
    PageContext& page, luciole::ViewPreferenceWriter& preferences, luciole::ThumbnailService& thumbnails,
    WaylandConnection& connection, RenderContext& renderer, std::function<void()> clearThumbnails
)
    : m_page(page), m_preferences(preferences), m_thumbnails(thumbnails), m_connection(connection),
      m_renderer(renderer), m_clearThumbnails(std::move(clearThumbnails)) {}

SettingsController::~SettingsController() = default;

void SettingsController::preferencesChanged(const std::string& error) {
  m_error = error;
  m_pending = m_preferences.settings();
  m_dirty = true;
  markViewDirty();
}

void SettingsController::markDirty() {
  m_dirty = true;
  markViewDirty();
}

void SettingsController::markViewDirty() {
  m_viewDirty = true;
  if (m_window) {
    m_window->requestLayout();
  }
}

void SettingsController::onFrame() {
  if (std::exchange(m_dirty, false) && m_pending) {
    m_page.applySettings(m_pending->browser, !m_error.empty());
  }
}

void SettingsController::frame(float width) {
  layoutDialog(width);
  if (std::exchange(m_viewDirty, false)) {
    sync();
  }
}

void SettingsController::layoutDialog(float width) {
  if (!m_view) {
    return;
  }
  // A width change moves the open dropdown's anchor.
  if (width != m_view->width()) {
    closeSelectDropdown();
  }
  m_view->setCompact(width < kCompactWidth);
}

void SettingsController::sync() {
  const auto& inherited = m_preferences.inheritedSettings();
  // The saved preferences rather than the page's copy, which waits for the page's next frame while its tab is hidden.
  m_view->sync(
      m_pending ? m_pending->browser : m_page.settings(), inherited ? inherited->browser : luciole::BrowserSettings{},
      m_preferences.busy() || m_thumbnails.clearing(), m_error.empty() ? m_thumbnails.error() : m_error,
      m_thumbnails.clearing()
  );
}

void SettingsController::close() {
  if (!m_window) {
    return;
  }
  if (m_dropdown) {
    m_dropdown->closeSelectDropdown();
    m_dropdown.reset();
  }
  m_viewLifetime.reset();
  m_view = nullptr;
  m_window.reset();
  m_page.focusFileList();
}

void SettingsController::setParent(luciole::WindowSurface& parent) {
  if (m_window) {
    m_window->setParent(parent);
  }
}

void SettingsController::toggle() {
  if (m_window) {
    m_window->dismiss();
    return;
  }
  m_viewLifetime = std::make_shared<int>(0);
  auto root = std::make_unique<SettingsDialog>(
      [this](luciole::OverrideEdit edit) {
        m_preferences.submit({std::move(edit)});
        if (m_window) {
          m_window->requestLayout();
        }
      },
      [this] { m_preferences.resetAll(); },
      [this] {
        if (m_window) {
          m_window->dismiss();
        }
      },
      [this] { m_clearThumbnails(); }
  );
  root->setPopupContext(this);
  m_view = root.get();
  sync();
  m_view->setCompact(dialogTierWidth(DialogWidth::Large) < kCompactWidth);
  // One Settings window for the whole application: preferences are shared by every page.
  m_window = m_page.openDialogWindow(std::move(root), "Settings", DialogWidth::Large, kMaxHeight, "settings");
  if (!m_window) {
    m_view = nullptr;
    m_viewLifetime.reset();
    m_page.actionError("Settings could not open a window.");
    return;
  }
  m_window->setClosedCallback([this] { close(); });
  m_window->setKeyHandler([this](const KeyboardEvent& event) { return key(event); });
  m_window->setPointerHandler([this](const PointerEvent& event) { return dropdownPointer(event); });
  m_window->setSurfaceOwner([this](const wl_surface* surface) { return dropdownOwns(surface); });
  m_window->setLayoutCallback([this](float width) { frame(width); });
  m_window->dispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
    if (m_view) {
      m_view->reveal(focused);
      m_window->requestLayout();
    }
  });
  m_window->dispatcher().setFocus(m_window->dispatcher().firstTabFocusUnder(m_view->navigation()));
}

bool SettingsController::key(const KeyboardEvent& event) {
  if (!m_view) {
    return false;
  }
  if (isSelectDropdownOpen()) {
    m_dropdown->onKeyboardEvent(event);
    return true;
  }
  if (event.pressed && luciole::shortcutMatches(luciole::BrowserAction::Settings, event.sym, event.modifiers)) {
    m_window->dismiss();
    return true;
  }
  return false;
}

bool SettingsController::dropdownOwns(const wl_surface* surface) const {
  return isSelectDropdownOpen() && m_dropdown->wlSurface() == surface;
}

bool SettingsController::dropdownPointer(const PointerEvent& event) {
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

void SettingsController::openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) {
  if (!m_view || !m_window) {
    if (callbacks.onDismiss) {
      callbacks.onDismiss();
    }
    return;
  }
  if (!m_dropdown) {
    m_dropdown = std::make_unique<SelectDropdownPopup>(m_connection, m_renderer);
  }
  auto& dialog = m_window->surface();
  m_dropdown->setParent(dialog.xdgSurface(), dialog.wlSurface(), m_connection.outputForSurface(dialog.wlSurface()));
  const std::weak_ptr<int> lifetime = m_viewLifetime;
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

void SettingsController::closeSelectDropdown() {
  if (m_dropdown) {
    m_dropdown->closeSelectDropdown();
  }
}

bool SettingsController::isSelectDropdownOpen() const { return m_dropdown && m_dropdown->isSelectDropdownOpen(); }
