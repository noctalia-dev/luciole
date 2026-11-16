#include "thumbnails/provider_process.h"

#include "fs/gio_ptr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

namespace {
  struct ChildLimits {
    pid_t parent;
    rlim_t fileBytes;
    unsigned maxCpus;
    // Launch ordinal: concurrent helpers take successive CPU groups instead of all sharing the first ones.
    unsigned rotation;
  };
  std::atomic<unsigned> launches{0};
  void childSetup(gpointer data) {
    const auto& limits = *static_cast<const ChildLimits*>(data);
    (void)::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != limits.parent) {
      ::_exit(1);
    }
    (void)::setsid();
    errno = 0;
    if (::nice(10) == -1 && errno != 0) {
      ::_exit(1);
    }
    // Keep nested decoder thread pools, and so their stacks and arenas, within the image worker's memory budget.
    if (limits.maxCpus) {
      cpu_set_t available, selected;
      if (::sched_getaffinity(0, sizeof(available), &available) < 0) {
        ::_exit(1);
      }
      std::array<int, CPU_SETSIZE> cpus{};
      unsigned total = 0;
      for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &available)) {
          cpus[total++] = cpu;
        }
      }
      if (total == 0) {
        ::_exit(1);
      }
      CPU_ZERO(&selected);
      const unsigned first = limits.rotation * limits.maxCpus;
      for (unsigned i = 0; i < std::min(limits.maxCpus, total); ++i) {
        CPU_SET(cpus[(first + i) % total], &selected);
      }
      if (::sched_setaffinity(0, sizeof(selected), &selected) < 0) {
        ::_exit(1);
      }
    }
    const rlimit cpu{5, 5}, memory{luciole::kProviderMemoryBytes, luciole::kProviderMemoryBytes},
        file{limits.fileBytes, limits.fileBytes};
    if (::setrlimit(RLIMIT_CPU, &cpu) < 0) {
      ::_exit(1);
    }
#ifndef __SANITIZE_ADDRESS__
    // Cap private writable memory, not address space: glycin's thread pools and malloc arenas reserve far more than
    // they touch (VmPeak around 850 MiB for a 20 MP JPEG), so RLIMIT_AS failed large images at random with
    // "mmap() failed". ASan maps its shadow region before the helper enters main.
    if (::setrlimit(RLIMIT_DATA, &memory) < 0) {
      ::_exit(1);
    }
#else
    (void)memory;
#endif
    if (::setrlimit(RLIMIT_FSIZE, &file) < 0) {
      ::_exit(1);
    }
  }
} // namespace

std::expected<void, std::string> luciole::runProviderProcess(
    std::vector<std::string> args, int input, GCancellable* cancellable, std::uint64_t fileBytes, unsigned maxCpus,
    int output, std::chrono::milliseconds timeout
) {
  if (g_cancellable_is_cancelled(cancellable)) {
    return std::unexpected("thumbnail request cancelled");
  }
  const luciole::GString executable(g_find_program_in_path(args.front().c_str()));
  if (!executable) {
    return std::unexpected("thumbnail provider executable unavailable");
  }
  args.front() = executable.get();
  const luciole::GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(
          (output >= 0 ? G_SUBPROCESS_FLAGS_NONE : G_SUBPROCESS_FLAGS_STDOUT_SILENCE)
          | G_SUBPROCESS_FLAGS_STDERR_SILENCE
      )
  ));
  if (output >= 0) {
    const int destination = ::fcntl(output, F_DUPFD_CLOEXEC, 4);
    if (destination < 0) {
      return std::unexpected("cannot capture provider output");
    }
    g_subprocess_launcher_take_stdout_fd(launcher.get(), destination);
    g_subprocess_launcher_setenv(launcher.get(), "LC_ALL", "C", true);
  }
  ChildLimits limits{::getpid(), fileBytes, maxCpus, launches.fetch_add(1, std::memory_order_relaxed)};
  g_subprocess_launcher_set_child_setup(launcher.get(), childSetup, &limits, nullptr);
  const int source = ::fcntl(input, F_DUPFD_CLOEXEC, 4);
  if (source < 0) {
    return std::unexpected("cannot pass source descriptor to thumbnail provider");
  }
  g_subprocess_launcher_take_fd(launcher.get(), source, 3);
  std::vector<const char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& argument : args) {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);
  const luciole::GioPtr<GSubprocess> process(g_subprocess_launcher_spawnv(launcher.get(), argv.data(), nullptr));
  if (!process) {
    return std::unexpected("cannot start thumbnail provider");
  }
  // Waiting occurs only on a thumbnail worker. The child is also CPU, memory and output bounded.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  struct Wait {
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false, cancelled = false;
  } wait;
  struct Disconnect {
    GCancellable* source;
    gulong id = 0;
    ~Disconnect() {
      if (source) {
        g_cancellable_disconnect(source, id);
      }
    }
  } cancellation{cancellable};
  if (cancellable) {
    cancellation.id = g_cancellable_connect(
        cancellable, G_CALLBACK(+[](GCancellable*, gpointer data) {
          auto& wait = *static_cast<Wait*>(data);
          {
            std::scoped_lock lock(wait.mutex);
            wait.cancelled = true;
          }
          wait.condition.notify_one();
        }),
        &wait, nullptr
    );
  }
  std::thread watchdog([&] {
    std::unique_lock lock(wait.mutex);
    wait.condition.wait_until(lock, deadline, [&] { return wait.finished || wait.cancelled; });
    if (wait.finished) {
      return;
    }
    lock.unlock();
    g_subprocess_force_exit(process.get());
  });
  GError* error = nullptr;
  const bool done = g_subprocess_wait_check(process.get(), nullptr, &error);
  const std::string failure = error ? error->message : "thumbnail provider failed";
  g_clear_error(&error);
  {
    std::scoped_lock lock(wait.mutex);
    wait.finished = true;
  }
  wait.condition.notify_one();
  watchdog.join();
  if (g_cancellable_is_cancelled(cancellable)) {
    return std::unexpected("thumbnail request cancelled");
  }
  if (!done) {
    return std::unexpected(failure);
  }
  return {};
}
