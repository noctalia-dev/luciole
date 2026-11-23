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

  void testDoubleClick() {
    WindowMoveGesture gesture;
    TEST_CHECK(!gesture.doubleClick(1000, 10.0F, 10.0F));
    TEST_CHECK(gesture.doubleClick(1000 + WindowMoveGesture::kDoubleClickMs, 12.0F, 11.0F));
    // The pair is used up: a third press starts a new one.
    TEST_CHECK(!gesture.doubleClick(1500, 12.0F, 11.0F));
    // Too slow, then too far away.
    TEST_CHECK(!gesture.doubleClick(1501 + WindowMoveGesture::kDoubleClickMs, 12.0F, 11.0F));
    TEST_CHECK(
        !gesture.doubleClick(1600 + WindowMoveGesture::kDoubleClickMs, 12.0F + Style::dragStartThreshold, 11.0F)
    );
    // The event clock wraps.
    WindowMoveGesture wrapping;
    TEST_CHECK(!wrapping.doubleClick(0xFFFFFF00U, 0.0F, 0.0F));
    TEST_CHECK(wrapping.doubleClick(0x40U, 0.0F, 0.0F));
  }

  void testMoveEndsDoubleClick() {
    WindowMoveGesture gesture;
    TEST_CHECK(!gesture.doubleClick(100, 10.0F, 10.0F));
    gesture.press(5, 10.0F, 10.0F);
    TEST_CHECK(gesture.motion(10.0F + Style::dragStartThreshold, 10.0F) == 5U);
    // The window moved under the pointer, so the same local spot is a new click.
    TEST_CHECK(!gesture.doubleClick(200, 10.0F, 10.0F));
  }
} // namespace

int main() {
  try {
    testClickNeverMoves();
    testDragMovesOnceWithPressSerial();
    testDiagonalDistance();
    testNewPressReplacesPending();
    testDoubleClick();
    testMoveEndsDoubleClick();
    std::println("window move gesture checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "window move gesture test: {}", error.what());
    return 1;
  }
}
