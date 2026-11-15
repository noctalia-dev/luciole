// Headless transfer benchmark: one TransferService job, timed from start to completion.
// Usage: luciole-transfer-bench --action copy|move --destination DIR SOURCE...
// Prints one JSON object on stdout.

#include "core/application_identity.h"
#include "event_loop/glib_poll_source.h"
#include "operations/transfer_service.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <gio/gio.h>
#include <map>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  double processCpuMs() {
    timespec value{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return (static_cast<double>(value.tv_sec) * 1e3) + (static_cast<double>(value.tv_nsec) / 1e6);
  }

  int usage() {
    std::println(stderr, "usage: luciole-transfer-bench --action copy|move --destination DIR SOURCE...");
    return EXIT_FAILURE;
  }
} // namespace

int main(int argc, char** argv) {
  setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
  luciole::TransferRequest request{.sources = {}, .destination = luciole::Location::fromPath("/")};
  bool haveAction = false;
  bool haveDestination = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--action" && i + 1 < argc) {
      const std::string_view action(argv[++i]);
      if (action != "copy" && action != "move") {
        return usage();
      }
      request.action = action == "copy" ? luciole::TransferAction::Copy : luciole::TransferAction::Move;
      haveAction = true;
    } else if (arg == "--destination" && i + 1 < argc) {
      request.destination = luciole::Location::fromPath(argv[++i]);
      haveDestination = true;
    } else {
      request.sources.push_back(luciole::Location::fromPath(std::string(arg)));
    }
  }
  if (!haveAction || !haveDestination || request.sources.empty()) {
    return usage();
  }
  // Mirrors the application's serialized first mount-table query.
  GFile* root = g_file_new_for_path("/");
  GFileInfo* rootInfo = g_file_query_filesystem_info(root, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, nullptr, nullptr);
  g_clear_object(&rootInfo);
  g_object_unref(root);

  GlibPollSource source;
  luciole::TransferService service;
  const double cpuBefore = processCpuMs();
  const auto start = std::chrono::steady_clock::now();
  if (!service.start(std::move(request))) {
    std::println(stderr, "transfer did not start");
    return EXIT_FAILURE;
  }
  while (service.busy()) {
    if (service.conflict()) {
      std::println(stderr, "unexpected conflict; the destination must be empty");
      service.cancel();
    }
    std::vector<pollfd> fds;
    const auto first = source.addPollFds(fds);
    ::poll(fds.data(), fds.size(), 5);
    source.dispatch(fds, first);
  }
  const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::map<int, int> outcomes;
  for (const auto& result : service.results()) {
    ++outcomes[static_cast<int>(result.outcome)];
  }
  std::string counts;
  for (const auto& [outcome, count] : outcomes) {
    counts += std::format("{}\"{}\":{}", counts.empty() ? "" : ",", outcome, count);
  }
  std::println(
      R"({{"wall_ms":{:.1f},"cpu_ms":{:.1f},"bytes":{},"items":{},"outcomes":{{{}}}}})", wallMs,
      processCpuMs() - cpuBefore, service.bytes(), service.completed(), counts
  );
  return EXIT_SUCCESS;
}
