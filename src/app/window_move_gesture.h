#pragma once

#include "ui/style.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

// A left press on window chrome that may become an interactive window move. The move starts only once the pointer
// travels the drag threshold, so a plain click never hands the window to the compositor. xdg_toplevel.move needs
// the serial of a press whose implicit grab is still held, so the move reuses the press serial while the button
// stays down.
class WindowMoveGesture {
public:
  void press(std::uint32_t serial, float x, float y) noexcept {
    m_serial = serial;
    m_pressX = x;
    m_pressY = y;
  }

  // The press serial, once, when the pointer has travelled the drag threshold from the press.
  [[nodiscard]] std::optional<std::uint32_t> motion(float x, float y) noexcept {
    if (!m_serial || std::hypot(x - m_pressX, y - m_pressY) < Style::dragStartThreshold) {
      return std::nullopt;
    }
    return std::exchange(m_serial, std::nullopt);
  }

  void cancel() noexcept { m_serial.reset(); }
  [[nodiscard]] bool pending() const noexcept { return m_serial.has_value(); }

private:
  std::optional<std::uint32_t> m_serial;
  float m_pressX = 0.0F;
  float m_pressY = 0.0F;
};
