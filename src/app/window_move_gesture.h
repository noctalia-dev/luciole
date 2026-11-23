#pragma once

#include "ui/style.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

// A left press on window chrome that may become an interactive window move. The move starts only once the pointer
// travels the drag threshold, so a plain click never hands the window to the compositor. xdg_toplevel.move needs
// the serial of a press whose implicit grab is still held, so the move reuses the press serial while the button
// stays down. It also tells a double click on the chrome from two separate clicks.
class WindowMoveGesture {
public:
  // As for double clicks in text fields.
  static constexpr std::uint32_t kDoubleClickMs = 400;

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
    // A press that moved the window never pairs with the next one.
    m_lastClick.reset();
    return std::exchange(m_serial, std::nullopt);
  }

  // Whether a left press at `timeMs` (the Wayland event time, which wraps) completes a double click: the previous
  // press was close in time and place. The pair is then used up, so a third press starts over.
  [[nodiscard]] bool doubleClick(std::uint32_t timeMs, float x, float y) noexcept {
    const bool second = m_lastClick
        && timeMs - m_lastClick->timeMs <= kDoubleClickMs
        && std::hypot(x - m_lastClick->x, y - m_lastClick->y) < Style::dragStartThreshold;
    m_lastClick = second ? std::nullopt : std::optional<Click>{Click{.timeMs = timeMs, .x = x, .y = y}};
    return second;
  }

  void cancel() noexcept { m_serial.reset(); }
  [[nodiscard]] bool pending() const noexcept { return m_serial.has_value(); }

private:
  struct Click {
    std::uint32_t timeMs = 0;
    float x = 0.0F, y = 0.0F;
  };
  std::optional<std::uint32_t> m_serial;
  std::optional<Click> m_lastClick;
  float m_pressX = 0.0F;
  float m_pressY = 0.0F;
};
