#include "app/page/settings_controller.h"

#include "app/page/active_popup.h"
#include "app/page/dialog_dropdown.h"
#include "app/page/page_context.h"
#include "config/view_preference_writer.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/dialogs/settings_dialog.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <utility>

SettingsController::SettingsController(
    PageContext& page, luciole::ViewPreferenceWriter& preferences, luciole::ThumbnailService& thumbnails,
    std::function<void()> clearThumbnails
)
    : m_page(page), m_preferences(preferences), m_thumbnails(thumbnails),
      m_clearThumbnails(std::move(clearThumbnails)) {}

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
      if (auto* surface = m_page.popupSurface()) {
        surface->requestLayout();
      }
    }
  }
}

void SettingsController::layoutDialog(float width) {
  if (m_view) {
    m_view->setCompact(width < 720.0F);
  }
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
  m_view = nullptr;
  m_page.focusFileList();
}

void SettingsController::toggle() {
  if (m_view) {
    m_page.dismissPopup();
    return;
  }
  auto root = std::make_unique<SettingsDialog>(
      [this](luciole::OverrideEdit edit) {
        m_preferences.submit({std::move(edit)});
        if (auto* surface = m_page.popupSurface()) {
          surface->requestLayout();
        }
      },
      [this] { m_preferences.resetAll(); }, [this] { m_page.dismissPopup(); }, [this] { m_clearThumbnails(); }
  );
  root->setPopupContext(&m_page.dialogDropdown());
  m_view = root.get();
  sync();
  m_view->setCompact(m_page.dialogWidth(DialogWidth::Large) < 720.0F);
  m_page.showDialog(std::move(root), DialogWidth::Large);
  if (!m_page.popupSurface()) {
    m_view = nullptr;
    return;
  }
  m_page.activePopup().bind(ActivePopup::Kind::Settings, [this] { teardown(); });
  m_page.popupDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
    if (m_view) {
      m_view->reveal(focused);
      m_page.popupSurface()->requestLayout();
    }
  });
  m_page.popupDispatcher().setFocus(m_page.popupDispatcher().firstTabFocusUnder(m_view->navigation()));
}

bool SettingsController::key(const KeyboardEvent& event) {
  if (!m_view) {
    return false;
  }
  if (event.pressed && luciole::shortcutMatches(luciole::BrowserAction::Settings, event.sym, event.modifiers)) {
    m_page.dismissPopup();
    return true;
  }
  return false;
}
