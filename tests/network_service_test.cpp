#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "fs/network_job.h"
#include "operations/create_receipt.h"
#include "operations/empty_trash_service.h"
#include "operations/history_mount.h"
#include "operations/network_history.h"
#include "operations/restore_service.h"
#include "test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
  using namespace luciole;
  using namespace std::chrono_literals;

  void pumpUntil(const std::function<bool()>& done) {
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto first = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 10) >= 0);
      source.dispatch(fds, first);
    }
  }

  void startupFailures() {
    const char* previous = std::getenv("LUCIOLE_IO_WORKER");
    const auto saved = previous ? std::optional<std::string>(previous) : std::nullopt;
    TEST_CHECK(::setenv("LUCIOLE_IO_WORKER", "/nonexistent/luciole-review-worker", 1) == 0);
    {
      EmptyTrashService service;
      TEST_CHECK(service.prepare());
      TEST_CHECK(service.busy() && service.pending());
      pumpUntil([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().error && !service.pending());
    }
    {
      DirectoryEntry item;
      item.place = Location::fromUri("trash:///review-item");
      item.name = "review-item";
      RestoreService service;
      TEST_CHECK(service.start({item}));
      pumpUntil([&] { return !service.busy(); });
      TEST_CHECK(service.total() == 1 && service.completed() == 1 && service.results().size() == 1);
      const auto& result = service.results().front();
      TEST_CHECK(result.source == item.location() && result.error && result.outcome == RestoreOutcome::Failed);
    }
    if (saved) {
      TEST_CHECK(::setenv("LUCIOLE_IO_WORKER", saved->c_str(), 1) == 0);
    } else {
      TEST_CHECK(::unsetenv("LUCIOLE_IO_WORKER") == 0);
    }
  }

  void creationAndUndo() {
    const luciole::GString directory(g_dir_make_tmp("luciole-service-test-XXXXXX", nullptr));
    TEST_CHECK(directory);
    const auto parent = Location::fromPath(directory.get());
    const auto target = std::filesystem::path(directory.get()) / "created";
    NetworkService<CreateSnapshot> service("create");
    TEST_CHECK(service.invoke("prepare", {{"parent", parent}, {"kind", CreateKind::EmptyFile}}));
    pumpUntil([&] { return !service.busy(); });
    TEST_CHECK(service.view.ready);
    TEST_CHECK(service.invoke("create", {{"name", "created"}}, true));
    pumpUntil([&] { return !service.busy(); });
    TEST_CHECK(service.view.snapshot.status == CreateStatus::Succeeded);
    TEST_CHECK(service.view.snapshot.receipt && std::filesystem::is_regular_file(target));
    std::optional<JobResult<CreateReplayResult>> undone;
    runIoJob<CreateReplayResult>(
        IoLane::Operation,
        [receipt = service.view.snapshot.receipt, parent] {
          TEST_CHECK(receipt->usesMount(HistoryMount(parent)));
          TEST_CHECK(!receipt->usesMount(HistoryMount(Location::fromPath("/proc"))));
          return receipt->reverse(CancelToken{});
        },
        [&](auto result) { undone = std::move(result); }
    );
    pumpUntil([&] { return undone.has_value(); });
    TEST_CHECK(*undone && (**undone).removed && !std::filesystem::exists(target));
    // A lost receipt belongs to its original process. Replaying it must never start a replacement.
    const auto stale = (**undone).receipt;
    TEST_CHECK(stale);
    const auto history = historyHandle(stale);
    try {
      (void)history->process->call({{"action", "noop"}}, CancelToken{}, {}, 0ms);
    } catch (const FileErrorException& error) {
      TEST_CHECK(error.error().kind == FileErrorKind::Unavailable);
    }
    TEST_CHECK(!history->process->alive());
    const auto executable = std::filesystem::path(directory.get()) / "worker";
    std::ofstream(executable) << "#!/bin/sh\ntouch \"$0.started\"\n";
    std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
    const luciole::GString previous(g_strdup(g_getenv("LUCIOLE_IO_WORKER")));
    TEST_CHECK(::setenv("LUCIOLE_IO_WORKER", executable.c_str(), 1) == 0);
    CreateService replay;
    TEST_CHECK(replay.replay(stale));
    pumpUntil([&] { return !replay.busy(); });
    TEST_CHECK(replay.snapshot().error && replay.snapshot().error->kind == FileErrorKind::Changed);
    TEST_CHECK(!std::filesystem::exists(executable.string() + ".started") && !std::filesystem::exists(target));
    if (previous) {
      TEST_CHECK(::setenv("LUCIOLE_IO_WORKER", previous.get(), 1) == 0);
    } else {
      TEST_CHECK(::unsetenv("LUCIOLE_IO_WORKER") == 0);
    }
    std::filesystem::remove_all(directory.get());
  }

  void stoppedAutomount() {
    const luciole::GString directory(g_dir_make_tmp("luciole-stopped-share-XXXXXX", nullptr));
    TEST_CHECK(directory);
    const std::filesystem::path root(directory.get());
    const auto target = root / "share";
    std::filesystem::create_directory(target);
    std::ofstream(root / "keep") << "local";
    const auto previous = knownNativeMounts();
    publishNativeMounts(
        {{.path = target.string(), .source = "test:/share", .type = "nfs", .options = {}, .automount = true}}
    );
    TransferService transfer;
    TEST_CHECK(transfer.start(
        {{Location::fromPath((root / "keep").native())}, Location::fromPath(target.native()), TransferAction::Move}
    ));
    pumpUntil([&] { return !transfer.busy(); });
    TEST_CHECK(transfer.results().size() == 1 && transfer.results().front().error);
    TEST_CHECK(transfer.results().front().error->kind == FileErrorKind::NotMounted);
    TEST_CHECK(std::filesystem::is_regular_file(root / "keep") && std::filesystem::is_empty(target));
    publishNativeMounts(previous);
    std::filesystem::remove_all(root);
  }
} // namespace

int main() {
  startupFailures();
  creationAndUndo();
  stoppedAutomount();
}
