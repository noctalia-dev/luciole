#pragma once

#include <cstddef>
#include <string_view>

// Startup milestones for benchmarks, printed to stderr only when LUCIOLE_STARTUP_TRACE=1.
// Each line is "luciole-trace <CLOCK_MONOTONIC ns> <event>". Main thread only.
namespace luciole {
  void startupTrace(std::string_view event);
  // The first ready listing; arms "content-presented" for the next frame drawn after it.
  void startupListingReady(std::size_t entries);
  // A window drew a frame.
  void startupRendered();
  // A window's frame callback completed: "first-frame" once, then "content-presented" once.
  void startupFrameDone();
} // namespace luciole
