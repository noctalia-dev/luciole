#include "event_loop/glib_poll_source.h"
#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/network_job.h"
#include "fs/network_process.h"
#include "fs/network_wire.h"
#include "test_check.h"

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
} // namespace

int main() {
  if (child()) {
    return runNetworkWorker(fixture);
  }
  protocol();
  cancellation();
  retainedCapacity();
  reuse();
  capacity();
  queues();
}
