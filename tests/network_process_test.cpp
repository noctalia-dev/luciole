#include "event_loop/glib_poll_source.h"
#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/network_job.h"
#include "fs/network_process.h"
#include "fs/network_wire.h"
#include "operations/create_service.h"
#include "operations/transfer_service.h"
#include "test_check.h"
#include "thumbnails/thumbnail_service.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <print>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
  using namespace luciole;
  using namespace std::chrono_literals;

  std::size_t pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds limit = 5s) {
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::size_t ticks = 0;
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto first = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 10) >= 0);
      source.dispatch(fds, first);
      ++ticks;
    }
    return ticks;
  }
  void fails(FileErrorKind expected, const std::function<void()>& request) {
    try {
      request();
      TEST_CHECK(false);
    } catch (const FileErrorException& error) {
      TEST_CHECK(error.error().kind == expected);
    }
  }
  NetworkMessage fixture(const NetworkMessage& request, const NetworkEvent& event, int descriptor) {
    const auto action = request.at("action").get<std::string>();
    if (action == "pid") {
      return ::getpid();
    }
    if (action == "environment") {
      const char* value = std::getenv("LUCIOLE_TEST_WORKER_ENV");
      return value ? NetworkMessage(value) : NetworkMessage{};
    }
    if (action == "crash") {
      ::_exit(7);
    }
    if (action == "oversize") {
      const auto size = htonl(UINT32_MAX);
      TEST_CHECK(::write(3, &size, sizeof(size)) == sizeof(size));
      while (true) {
        ::pause();
      }
    }
    if (action == "advancing") {
      std::this_thread::sleep_for(650ms);
      noteWorkerIoProgress();
      std::this_thread::sleep_for(650ms);
      return true;
    }
    if (action == "operation" || action == "stall") {
      (void)event({{"started", true}});
      while (true) {
        ::pause();
      }
    }
    if (action == "error") {
      throwFileError(FileErrorKind::Denied, "Fixture denied.");
    }
    if (action == "descriptor") {
      TEST_CHECK(descriptor >= 0 && ::write(descriptor, "ok", 2) == 2);
    }
    return request.value("value", NetworkMessage(nullptr));
  }
  bool child() {
    int type = 0;
    socklen_t length = sizeof(type);
    return ::getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &length) == 0
        && type == SOCK_STREAM
        && ::getsockopt(4, SOL_SOCKET, SO_TYPE, &type, &length) == 0
        && type == SOCK_DGRAM;
  }
  void protocol() {
    const luciole::GString self(g_file_read_link("/proc/self/exe", nullptr));
    TEST_CHECK(self);
    TEST_CHECK(::setenv("LUCIOLE_IO_WORKER", self.get(), 1) == 0);
    std::optional<JobResult<bool>> reply;
    runJob<bool>(
        [] {
          const CancelToken token;
          auto process = NetworkProcess::acquire();
          TEST_CHECK(process->call({{"action", "echo"}, {"value", "raw-\xff"}}, token) == "raw-\xff");
          fails(FileErrorKind::Denied, [&] { (void)process->call({{"action", "error"}}, token); });
          TEST_CHECK(process->alive());
          TEST_CHECK(process->call({{"action", "echo"}, {"value", 42}}, token) == 42);
          const Fd local(::memfd_create("luciole-protocol-test", MFD_CLOEXEC));
          TEST_CHECK(local.valid());
          (void)process->call({{"action", "descriptor"}}, token, {}, 1s, local.get());
          char contents[2]{};
          TEST_CHECK(::pread(local.get(), contents, sizeof(contents), 0) == 2 && std::string_view(contents, 2) == "ok");
          TEST_CHECK(process->call({{"action", "advancing"}}, token, {}, 1s) == true);
          const auto started = std::chrono::steady_clock::now();
          fails(FileErrorKind::Unavailable, [&] {
            (void)process->call({{"action", "stall"}}, token, [](const auto&) { return NetworkMessage{}; }, 100ms);
          });
          TEST_CHECK(std::chrono::steady_clock::now() - started < 1s && !process->alive());
          for (const auto* action : {"crash", "oversize"}) {
            auto broken = NetworkProcess::acquire();
            fails(FileErrorKind::Unavailable, [&] { (void)broken->call({{"action", action}}, token); });
          }
          return true;
        },
        [&](auto value) { reply.emplace(std::move(value)); }
    );
    pumpUntil([&] { return reply.has_value(); });
    TEST_CHECK(*reply);
  }
  void cancellation() {
    const CancelToken token;
    std::atomic<bool> started{false};
    std::optional<JobResult<NetworkMessage>> reply;
    runJob<NetworkMessage>(
        [token, &started] {
          return NetworkProcess::acquire(IoLane::NetworkOperation)
              ->call({{"action", "operation"}, {"mutating", true}}, token, [&](const auto&) {
                started = true;
                return NetworkMessage{};
              });
        },
        [&](auto value) { reply.emplace(std::move(value)); }
    );
    pumpUntil([&] { return started.load(); });
    token.cancel();
    const auto ticks = pumpUntil([&] { return reply.has_value(); }, 3s);
    TEST_CHECK(!*reply && reply->error().kind == FileErrorKind::Changed && ticks > 20);
    // Stalled network mutations leave a separate lane available for ordinary filesystem reads.
    const auto local = NetworkProcess::acquire(IoLane::Native);
    TEST_CHECK(local->call({{"action", "echo"}, {"value", "local"}}, CancelToken{}) == "local");
  }
  void retainedCapacity() {
    const auto first = NetworkProcess::acquire(IoLane::Native);
    const auto firstPid = first->call({{"action", "pid"}}, CancelToken{});
    const auto second = NetworkProcess::acquire(IoLane::Native);
    const auto secondPid = second->call({{"action", "pid"}}, CancelToken{});
    TEST_CHECK(firstPid != secondPid);
    // Retained undo history can share an idle helper when the lane is at capacity.
    const auto third = NetworkProcess::acquire(IoLane::Native);
    const auto thirdPid = third->call({{"action", "pid"}}, CancelToken{});
    TEST_CHECK(thirdPid == firstPid || thirdPid == secondPid);
  }
  void reuse() {
    TEST_CHECK(::setenv("LUCIOLE_TEST_WORKER_ENV", "first", 1) == 0);
    const auto process = NetworkProcess::acquire(IoLane::Operation);
    const CancelToken token;
    TEST_CHECK(process->call({{"action", "environment"}}, token) == "first");
    TEST_CHECK(::unsetenv("LUCIOLE_TEST_WORKER_ENV") == 0);
    TEST_CHECK(process->call({{"action", "environment"}}, token).is_null());

    // Cleanup belongs to the process that retained the files.
    fails(FileErrorKind::Unavailable, [&] { (void)process->call({{"action", "crash"}}, token); });
    process->discard(42, true);
    TEST_CHECK(!networkCleanupPending());
    TEST_CHECK(takeNetworkCleanupErrors().size() == 1);
  }
  void capacity() {
    std::vector<std::shared_ptr<NetworkProcess>> held;
    for (int i = 0; i < 4; ++i) {
      held.push_back(NetworkProcess::acquire(IoLane::Network));
    }
    fails(FileErrorKind::Unavailable, [] { (void)NetworkProcess::acquire(IoLane::Network); });
    TEST_CHECK(NetworkProcess::acquire(IoLane::Native));
    held.clear();
  }
  void queues() {
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    std::atomic<int> started{0};
    int completed = 0;
    for (int i = 0; i < 4; ++i) {
      runIoJob<bool>(
          IoLane::Network,
          [&] {
            ++started;
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return release; });
            return true;
          },
          [&](auto result) {
            TEST_CHECK(result && *result);
            ++completed;
          }
      );
    }
    pumpUntil([&] { return started == 4; });
    for (int i = 0; i < 64; ++i) {
      runIoJob<bool>(
          IoLane::Network, [] { return true; },
          [&](auto result) {
            TEST_CHECK(result && *result);
            ++completed;
          }
      );
    }
    bool refused = false, local = false, gio = false;
    runIoJob<bool>(
        IoLane::Network,
        [] {
          TEST_CHECK(false);
          return false;
        },
        [&](auto result) {
          TEST_CHECK(!result && result.error().kind == FileErrorKind::Unavailable);
          refused = true;
        }
    );
    runIoJob<bool>(
        IoLane::Native, [] { return true; },
        [&](auto result) {
          TEST_CHECK(result && *result);
          local = true;
        }
    );
    runJob<bool>(
        [] { return true; },
        [&](auto result) {
          TEST_CHECK(result && *result);
          gio = true;
        }
    );
    pumpUntil([&] { return refused && local && gio; });
    {
      const std::scoped_lock lock(mutex);
      release = true;
    }
    wake.notify_all();
    pumpUntil([&] { return completed == 68; });
  }
  void disconnected(const std::string& path, bool shutdown) {
    const auto target = Location::fromPath(path);
    DirectorySession remote;
    remote.open(target);
    pumpUntil([&] { return remote.snapshot().status != DirectoryStatus::Loading; }, 15s);
    TEST_CHECK(remote.snapshot().status == DirectoryStatus::Ready);
    const auto retained = remote.snapshot().entries.size();
    const auto picture = std::ranges::find(remote.snapshot().entries, "preview.png", &DirectoryEntry::name);
    TEST_CHECK(picture != remote.snapshot().entries.end());
    const auto image = *picture;
    CreateService create;
    TEST_CHECK(create.prepare(target, CreateKind::EmptyFile));
    pumpUntil([&] { return !create.busy(); });
    TEST_CHECK(create.canCreate());
    const luciole::GString temporary(g_dir_make_tmp("luciole-disconnect-XXXXXX", nullptr));
    TEST_CHECK(temporary);
    const auto destination = Location::fromPath(temporary.get());
    TransferService copy;
    if (!shutdown) {
      TEST_CHECK(
          copy.start({.sources = {target.child("large")}, .destination = destination, .action = TransferAction::Copy})
      );
      pumpUntil([&] { return copy.bytes() > 0 || !copy.busy(); }, 15s);
      TEST_CHECK(copy.busy());
    }
    std::println("ready for disconnect");
    std::fflush(stdout);
    TEST_CHECK(std::getchar() == 'g');
    remote.refresh();
    TEST_CHECK(create.create("interrupted-create"));
    ThumbnailService thumbnails;
    thumbnails.setOptions({.policy = ThumbnailPolicy::All});
    const auto preview = thumbnails.request(image, 128);
    TEST_CHECK(preview);
    std::filesystem::create_symlink(
        target.child("large").nativePath().value(), std::filesystem::path(temporary.get()) / "remote-link"
    );
    DirectorySession local;
    local.open(destination);
    const auto ticks = pumpUntil(
        [&] {
          return local.snapshot().status == DirectoryStatus::Ready
              && (shutdown
                  || (!copy.busy()
                      && !create.busy()
                      && preview->ready
                      && remote.snapshot().status == DirectoryStatus::Error));
        },
        15s
    );
    TEST_CHECK(ticks > 0 && local.snapshot().status == DirectoryStatus::Ready);
    if (shutdown) {
      remote.cancel();
      create.cancel();
      pumpUntil([&] { return !create.busy(); }, 3s);
      std::println("shutdown before reconnect passed");
      std::fflush(stdout);
      return;
    }
    TEST_CHECK(remote.snapshot().entries.size() == retained && !preview->pixels);
    if (!create.snapshot().error || create.snapshot().error->kind != FileErrorKind::Changed) {
      std::println(
          stderr, "Interrupted create: status {}; error {}: {}", std::to_underlying(create.snapshot().status),
          create.snapshot().error ? std::to_underlying(create.snapshot().error->kind) : -1,
          create.snapshot().error ? create.snapshot().error->message : "none"
      );
    }
    TEST_CHECK(create.snapshot().error && create.snapshot().error->kind == FileErrorKind::Changed);
    if (copy.results().size() != 1 || copy.results().front().outcome != TransferOutcome::Partial) {
      std::println(stderr, "Interrupted copy: {} results; report {}", copy.results().size(), copy.report());
    }
    TEST_CHECK(
        copy.results().size() == 1
        && copy.results().front().outcome == TransferOutcome::Partial
        && copy.retryable() == 0
    );
    std::println("disconnect browsing, preview and mutation passed; ticks {}", ticks);
    std::fflush(stdout);
    TEST_CHECK(std::getchar() == 'r');
    remote.refresh();
    pumpUntil([&] { return remote.snapshot().status != DirectoryStatus::Loading; }, 15s);
    TEST_CHECK(remote.snapshot().status == DirectoryStatus::Ready);
    std::filesystem::remove_all(temporary.get());
    std::println("reconnect refresh passed");
  }
} // namespace

int main(int argc, char** argv) {
  if (child()) {
    return runNetworkWorker(fixture);
  }
  if (argc == 3) {
    disconnected(argv[2], std::string_view(argv[1]) == "--shutdown");
    return 0;
  }
  protocol();
  cancellation();
  retainedCapacity();
  reuse();
  capacity();
  queues();
}
