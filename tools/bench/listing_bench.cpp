// Headless listing benchmark: DirectorySession publication plus DirectoryModel preparation.
// Prints one JSON object per measured run on stdout.

#include "browser/directory_model.h"
#include "core/application_identity.h"
#include "event_loop/glib_poll_source.h"
#include "fs/directory_session.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <malloc.h>
#include <memory>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using Clock = std::chrono::steady_clock;

  double milliseconds(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

  double processCpuMs() {
    timespec value{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return (static_cast<double>(value.tv_sec) * 1e3) + (static_cast<double>(value.tv_nsec) / 1e6);
  }

  long pssKib() {
    std::ifstream file("/proc/self/smaps_rollup");
    std::string line;
    while (std::getline(file, line)) {
      if (line.starts_with("Pss:")) {
        return std::strtol(line.c_str() + 4, nullptr, 10);
      }
    }
    return -1;
  }

  class Pump {
  public:
    // Runs the default context until done() holds; returns the longest single dispatch.
    double until(const std::function<bool()>& done) {
      double longest = 0;
      const auto deadline = Clock::now() + std::chrono::seconds(120);
      while (!done()) {
        if (Clock::now() > deadline) {
          throw std::runtime_error("timed out waiting for the listing");
        }
        longest = std::max(longest, once(-1));
      }
      return longest;
    }

    // Keeps dispatching for durationMs, so low-priority idle work (heap trimming) runs.
    void idle(int durationMs) {
      const auto end = Clock::now() + std::chrono::milliseconds(durationMs);
      while (Clock::now() < end) {
        once(10);
      }
    }

  private:
    // Polls with the source's deadline (bounded by timeout when >= 0); returns the dispatch time.
    double once(int timeout) {
      std::vector<pollfd> fds;
      const auto first = m_source.addPollFds(fds);
      if (const int deadline = m_source.pollTimeoutMs(); deadline >= 0 && (timeout < 0 || deadline < timeout)) {
        timeout = deadline;
      }
      ::poll(fds.data(), fds.size(), timeout);
      const auto start = Clock::now();
      m_source.dispatch(fds, first);
      return milliseconds(Clock::now() - start);
    }

    GlibPollSource m_source;
  };

  bool settled(const luciole::DirectorySession& session) {
    const auto status = session.snapshot().status;
    return status == luciole::DirectoryStatus::Ready || status == luciole::DirectoryStatus::Error;
  }
} // namespace

int main(int argc, char** argv) {
  setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
  std::string folder;
  std::string empty;
  int runs = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--runs" && i + 1 < argc) {
      const std::string_view value(argv[++i]);
      if (std::from_chars(value.data(), value.data() + value.size(), runs).ec != std::errc{} || runs < 1) {
        std::println(stderr, "luciole-listing-bench: --runs needs a positive integer");
        return EXIT_FAILURE;
      }
    } else if (arg == "--then-empty" && i + 1 < argc) {
      empty = argv[++i];
    } else if (folder.empty()) {
      folder = arg;
    } else {
      std::println(stderr, "usage: luciole-listing-bench FOLDER [--runs N] [--then-empty EMPTY_FOLDER]");
      return EXIT_FAILURE;
    }
  }
  if (folder.empty()) {
    std::println(stderr, "usage: luciole-listing-bench FOLDER [--runs N] [--then-empty EMPTY_FOLDER]");
    return EXIT_FAILURE;
  }
  // Mirrors the application's allocator settings so the memory figures match it.
  mallopt(M_ARENA_MAX, 2);
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  // Mirrors the application's serialized first mount-table query.
  GFile* root = g_file_new_for_path("/");
  GFileInfo* rootInfo = g_file_query_filesystem_info(root, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, nullptr, nullptr);
  g_clear_object(&rootInfo);
  g_object_unref(root);

  Pump pump;
  for (int run = 0; run <= runs; ++run) {
    // Return the previous run's freed pages so the growth below is this run's own.
    malloc_trim(0);
    const long pssBefore = pssKib();
    const double cpuBefore = processCpuMs();
    auto session = std::make_unique<luciole::DirectorySession>();
    luciole::DirectoryModel model;
    const auto opened = Clock::now();
    session->open(luciole::Location::fromPath(folder));
    const double longest = pump.until([&] { return settled(*session); });
    const auto ready = Clock::now();
    const long pssSession = pssKib();
    model.applySnapshot(session->snapshot());
    const auto applied = Clock::now();
    const double cpuAfter = processCpuMs();
    const long pssModel = pssKib();
    const auto entries = session->snapshot().entries.size();
    // Sniffed content types of uncertain names follow the listing.
    pump.until([&] { return !session->snapshot().contentTypesPending; });
    const auto typed = Clock::now();
    const double cpuTyped = processCpuMs();
    long pssEmpty = -1;
    if (!empty.empty()) {
      session->open(luciole::Location::fromPath(empty));
      pump.until([&] { return settled(*session); });
      model.applySnapshot(session->snapshot());
      pump.idle(200);
      pssEmpty = pssKib();
    }
    if (run == 0) {
      continue; // Warm-up: page cache, MIME database and allocator.
    }
    std::println(
        R"({{"run":{},"entries":{},"session_ms":{:.2f},"apply_ms":{:.2f},"cpu_ms":{:.2f},"longest_dispatch_ms":{:.2f},)"
        R"("typed_ms":{:.2f},"typed_cpu_ms":{:.2f},)"
        R"("pss_before_kib":{},"pss_after_session_kib":{},"pss_after_model_kib":{},"bytes_per_entry":{:.1f},)"
        R"("pss_after_empty_kib":{}}})",
        run, entries, milliseconds(ready - opened), milliseconds(applied - ready), cpuAfter - cpuBefore, longest,
        milliseconds(typed - opened), cpuTyped - cpuBefore, pssBefore, pssSession, pssModel,
        entries == 0 ? 0.0 : static_cast<double>(pssModel - pssBefore) * 1024.0 / static_cast<double>(entries), pssEmpty
    );
  }
  return EXIT_SUCCESS;
}
