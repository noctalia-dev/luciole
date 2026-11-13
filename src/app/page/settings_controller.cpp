#include "app/page/settings_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "config/view_preference_writer.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/dialogs/settings_dialog.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <utility>

namespace {
  constexpr float kDialogWidth = 880.0F;
  constexpr float kDialogHeight = 640.0F;
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
}

void SettingsController::onFrame() {
  if (std::exchange(m_dirty, false)) {
    if (m_pending) {
      m_page.applySettings(m_pending->browser, !m_error.empty());
    }
    if (m_view) {
      sync();
      if (auto* surface = m_page.menuSurface()) {
        surface->requestLayout();
      }
    }
  }
}

void SettingsController::layoutDialog(float width) {
  if (!m_view) {
    return;
  }
  const float height = std::min(kDialogHeight, static_cast<float>(m_page.surface().height()) - 64.0F);
  if (width != m_view->width() || height != m_view->height()) {
    closeSelectDropdown();
  }
  m_view->setCompact(width < 720.0F);
  m_view->setSize(width, height);
}

void SettingsController::sync() {
  const auto& inherited = m_preferences.inheritedSettings();
  m_view->sync(
      m_page.settings(), inherited ? inherited->browser : luciole::BrowserSettings{},
      m_preferences.busy() || m_thumbnails.clearing(), m_error.empty() ? m_thumbnails.error() : m_error,
      m_thumbnails.clearing()
  );
}

void SettingsController::teardown() {
  if (m_dropdown) {
    m_dropdown->closeSelectDropdown();
    m_dropdown.reset();
  }
  m_viewLifetime.reset();
  m_view = nullptr;
  m_page.focusFileList();
}

void SettingsController::toggle() {
  if (m_view) {
    m_page.dismissMenu();
    return;
  }
  m_viewLifetime = std::make_shared<int>(0);
  auto root = std::make_unique<SettingsDialog>(
      [this](luciole::OverrideEdit edit) {
        m_preferences.submit({std::move(edit)});
        if (auto* surface = m_page.menuSurface()) {
          surface->requestLayout();
        }
      },
      [this] { m_preferences.resetAll(); }, [this] { m_page.dismissMenu(); }, [this] { m_clearThumbnails(); }
  );
  root->setPopupContext(this);
  m_view = root.get();
  sync();
  auto& surface = m_page.surface();
  const float width = std::min(kDialogWidth, static_cast<float>(surface.width()) - 32.0F);
  const float height = std::min(kDialogHeight, static_cast<float>(surface.height()) - 64.0F);
  m_view->setCompact(width < 720.0F);
  root->setSize(width, height);
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false, PopupMode::Dialog
  );
  if (!m_page.menuSurface()) {
    m_view = nullptr;
    m_viewLifetime.reset();
    return;
  }
  m_page.activeDialog().bind(ActiveDialog::Kind::Settings, [this] { teardown(); });
  // Keep the preferred width so a dialog opened in a small window can grow again.
  m_page.setDialogSize(kDialogWidth, height);
  m_page.menuDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
    if (m_view) {
      m_view->reveal(focused);
      m_page.menuSurface()->requestLayout();
    }
  });
  m_page.menuDispatcher().setFocus(m_page.menuDispatcher().firstTabFocusUnder(m_view));
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
    m_page.dismissMenu();
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
  auto* dialog = m_page.menuSurface();
  if (!m_view || !dialog) {
    if (callbacks.onDismiss) {
      callbacks.onDismiss();
    }
    return;
  }
  if (!m_dropdown) {
    m_dropdown = std::make_unique<SelectDropdownPopup>(m_connection, m_renderer);
  }
  m_dropdown->setParent(
      dialog->xdgSurface(), dialog->wlSurface(), m_connection.outputForSurface(m_page.surface().wlSurface())
  );
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
