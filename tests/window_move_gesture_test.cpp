#include "app/window_move_gesture.h"
#include "test_check.h"

#include <exception>
#include <print>

namespace {
  void testClickNeverMoves() {
    WindowMoveGesture gesture;
    gesture.press(7, 100.0F, 20.0F);
    TEST_CHECK(gesture.pending());
    TEST_CHECK(!gesture.motion(101.0F, 21.0F));
    TEST_CHECK(!gesture.motion(100.0F + Style::dragStartThreshold - 1.0F, 20.0F));
    gesture.cancel();
    TEST_CHECK(!gesture.pending());
    TEST_CHECK(!gesture.motion(300.0F, 300.0F));
  }

  void testDragMovesOnceWithPressSerial() {
    WindowMoveGesture gesture;
    gesture.press(42, 100.0F, 20.0F);
    TEST_CHECK(!gesture.motion(102.0F, 21.0F));
    TEST_CHECK(gesture.motion(100.0F, 20.0F + Style::dragStartThreshold) == 42U);
    TEST_CHECK(!gesture.pending());
    TEST_CHECK(!gesture.motion(200.0F, 200.0F));
  }

  void testDiagonalDistance() {
    WindowMoveGesture gesture;
    gesture.press(3, 0.0F, 0.0F);
    // Each axis stays under the threshold while the distance passes it.
    const float step = Style::dragStartThreshold * 0.75F;
    TEST_CHECK(!gesture.motion(step, 0.0F));
    TEST_CHECK(gesture.motion(step, step) == 3U);
  }

  void testNewPressReplacesPending() {
    WindowMoveGesture gesture;
    gesture.press(1, 0.0F, 0.0F);
    gesture.press(2, 50.0F, 50.0F);
    TEST_CHECK(!gesture.motion(50.0F + Style::dragStartThreshold - 1.0F, 50.0F));
    TEST_CHECK(gesture.motion(50.0F + Style::dragStartThreshold, 50.0F) == 2U);
  }
} // namespace

int main() {
  try {
    testClickNeverMoves();
    testDragMovesOnceWithPressSerial();
    testDiagonalDistance();
    testNewPressReplacesPending();
    std::println("window move gesture checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "window move gesture test: {}", error.what());
    return 1;
  }
}
