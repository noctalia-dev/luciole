#include "app/startup_trace.h"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <print>

namespace {
  enum class Content : std::uint8_t { Waiting, Pending, Armed, Done };

  struct Trace {
    bool enabled = false;
    bool firstFrame = false;
    bool listing = false;
    Content content = Content::Waiting;
  };

  Trace& trace() {
    static Trace state{.enabled = [] {
      const char* value = std::getenv("LUCIOLE_STARTUP_TRACE");
      return value != nullptr && std::string_view(value) == "1";
    }()};
    return state;
  }

  std::uint64_t nowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000U) + static_cast<std::uint64_t>(now.tv_nsec);
  }
} // namespace

void luciole::startupTrace(std::string_view event) {
  if (trace().enabled) {
    std::println(stderr, "luciole-trace {} {}", nowNs(), event);
  }
}

void luciole::startupListingReady(std::size_t entries) {
  auto& state = trace();
  if (!state.enabled || state.listing) {
    return;
  }
  state.listing = true;
  state.content = Content::Pending;
  startupTrace(std::format("listing-ready entries={}", entries));
}

void luciole::startupRendered() {
  auto& state = trace();
  if (state.content == Content::Pending) {
    state.content = Content::Armed;
  }
}

void luciole::startupFrameDone() {
  auto& state = trace();
  if (!state.enabled) {
    return;
  }
  if (!state.firstFrame) {
    state.firstFrame = true;
    startupTrace("first-frame");
  }
  if (state.content == Content::Armed) {
    state.content = Content::Done;
    startupTrace("content-presented");
  }
}
