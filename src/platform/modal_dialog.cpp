#include "platform/modal_dialog.h"

#include "xdg-dialog-v1-client-protocol.h"

#include <cstdint>
#include <utility>
#include <wayland-client.h>

luciole::ModalDialog::ModalDialog(wl_display* display, xdg_dialog_v1* dialog) : m_display(display), m_dialog(dialog) {
  xdg_dialog_v1_set_modal(m_dialog);
}

luciole::ModalDialog::~ModalDialog() {
  xdg_dialog_v1_destroy(m_dialog);
  finish(false);
}

void luciole::ModalDialog::complete(std::function<void(bool)> ready) {
  if (m_result) {
    ready(*m_result);
    return;
  }
  m_ready.push_back(std::move(ready));
  if (m_sync) {
    return;
  }
  xdg_dialog_v1_unset_modal(m_dialog);
  if (wl_display_get_error(m_display) != 0 || !(m_sync = wl_display_sync(m_display))) {
    finish(false);
    return;
  }
  static const wl_callback_listener listener{.done = [](void* data, wl_callback*, std::uint32_t) {
    static_cast<ModalDialog*>(data)->finish(true);
  }};
  wl_callback_add_listener(m_sync, &listener, this);
}

void luciole::ModalDialog::finish(bool acknowledged) {
  if (m_sync) {
    wl_callback_destroy(std::exchange(m_sync, nullptr));
  }
  m_result = acknowledged;
  auto ready = std::move(m_ready);
  // A continuation may destroy this dialog.
  for (auto& callback : ready) {
    callback(acknowledged);
  }
}
