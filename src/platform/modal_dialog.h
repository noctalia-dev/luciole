#pragma once

#include <functional>
#include <optional>
#include <vector>

struct wl_callback;
struct wl_display;
struct xdg_dialog_v1;

namespace luciole {
  // Owns a modal xdg-dialog. Keep its foreign parent attached until completion: clients may release their
  // exported parent as soon as they receive a portal reply. The display must outlive this object.
  class ModalDialog {
  public:
    ModalDialog(wl_display* display, xdg_dialog_v1* dialog);
    ~ModalDialog();
    ModalDialog(const ModalDialog&) = delete;
    ModalDialog& operator=(const ModalDialog&) = delete;

    // Clears modality and waits asynchronously until the compositor has processed it. False means the
    // display failed or the dialog was destroyed before acknowledgment. Repeated calls share the barrier.
    void complete(std::function<void(bool)> ready);
    bool pending() const noexcept { return m_sync != nullptr; }

  private:
    void finish(bool acknowledged);
    wl_display* m_display;
    xdg_dialog_v1* m_dialog;
    wl_callback* m_sync = nullptr;
    std::optional<bool> m_result;
    std::vector<std::function<void(bool)>> m_ready;
  };
} // namespace luciole
