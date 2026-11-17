#include "app/page/mount_controller.h"

#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "render/scene/input_dispatcher.h"
#include "ui/dialogs/mount_dialog.h"
#include "wayland/popup_surface.h"

#include <utility>

luciole::MountInteraction* MountController::current(std::uint64_t context, std::uint64_t request) {
  if (m_page.browser().contextSerial() != context) {
    return nullptr;
  }
  auto* interaction = m_page.browser().mountInteraction();
  return interaction && interaction->prompt() && interaction->prompt()->id == request ? interaction : nullptr;
}
void MountController::onFrame() {
  if (!m_page.popupAvailable()) {
    return;
  }
  if (m_active) {
    if (!current(m_active->context, m_active->request)) {
      m_active.reset();
      if (m_page.activePopup().kind() == ActivePopup::Kind::Mount) {
        m_page.dismissPopup();
      }
    }
    return;
  }
  auto* interaction = m_page.browser().mountInteraction();
  if (!m_page.visible()
      || !m_page.active()
      || m_page.overlay()
      || m_page.popupSurface()
      || !interaction
      || !interaction->prompt()) {
    return;
  }
  const auto prompt = *interaction->prompt();
  const Active active{m_page.browser().contextSerial(), prompt.id};
  auto root = std::make_unique<MountDialog>(
      prompt,
      [this, active](const luciole::MountCredentials& credentials) {
        auto* request = current(active.context, active.request);
        if (!request || !request->answer(active.request, credentials)) {
          return false;
        }
        m_active.reset();
        m_page.dismissPopup();
        return true;
      },
      [this, active](std::size_t choice) {
        if (auto* request = current(active.context, active.request);
            request && request->choose(active.request, choice)) {
          m_active.reset();
          m_page.dismissPopup();
        }
      },
      [this] { m_page.dismissPopup(); }
  );
  auto* dialog = root.get();
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface()) {
    m_active = active;
    m_page.activePopup().bind(ActivePopup::Kind::Mount, [this, active] {
      if (std::exchange(m_active, std::nullopt)) {
        if (auto* request = current(active.context, active.request)) {
          request->cancel();
        }
      }
    });
    if (auto* focus = dialog->focusArea()) {
      m_page.popupDispatcher().setFocus(focus);
    }
  } else {
    interaction->cancel();
  }
}
