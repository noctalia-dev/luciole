#include "operations/archive_provider_process.h"

#include "fs/gio_interop.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <ranges>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
  using namespace luciole;
  using namespace luciole::archive_work;
  struct Limits {
    pid_t parent;
    rlim_t fileBytes, seconds;
  };
  void childSetup(gpointer data) {
    const auto& limits = *static_cast<const Limits*>(data);
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || ::getppid() != limits.parent || ::setsid() < 0) {
      ::_exit(1);
    }
    const rlimit cpu{limits.seconds, limits.seconds}, file{limits.fileBytes, limits.fileBytes}, core{0, 0};
    // Preserve inherited resource policy; only bound payloads by their validated sizes and explicit deadlines.
    if ((limits.seconds != RLIM_INFINITY && ::setrlimit(RLIMIT_CPU, &cpu) < 0)
        || ::setrlimit(RLIMIT_FSIZE, &file) < 0
        || ::setrlimit(RLIMIT_CORE, &core) < 0) {
      ::_exit(1);
    }
  }
  struct Child {
    GioPtr<GSubprocess> process;
    bool reaped = false;
    ~Child() {
      if (process && !reaped) {
        g_subprocess_force_exit(process.get());
        (void)g_subprocess_wait(process.get(), nullptr, nullptr);
      }
    }
  };
  void sendSecret(int fd, std::string_view bytes, const Work& work) {
    while (!bytes.empty()) {
      work.check();
      const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        return; // Classify an early provider exit after reaping it.
      }
      if (count <= 0) {
        systemError("Could not pass the archive password");
      }
      bytes.remove_prefix(static_cast<std::size_t>(count));
    }
  }
  void copyOutput(Child& child, int output, std::uint64_t maximum, Work& work) {
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t total = 0;
    auto* stream = g_subprocess_get_stdout_pipe(child.process.get());
    while (true) {
      work.check();
      const auto count =
          g_input_stream_read(stream, buffer.data(), buffer.size(), gioCancellable(work.cancel), nullptr);
      if (count < 0) {
        work.check();
        throwFileError(FileErrorKind::Other, "Could not read the archive provider output.");
      }
      if (count == 0) {
        return;
      }
      const auto bytes = static_cast<std::size_t>(count);
      if (bytes > maximum - total) {
        throwFileError(FileErrorKind::Unsupported, "The archive provider output exceeds its limit.");
      }
      total += bytes;
      std::size_t offset = 0;
      while (offset < bytes) {
        work.check();
        const auto written = ::write(output, buffer.data() + offset, bytes - offset);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written <= 0) {
          systemError("Could not write archive provider output");
        }
        offset += static_cast<std::size_t>(written);
      }
    }
  }
  void collect(Child& child, int output, std::uint64_t maximum, Work& work) {
    struct State {
      std::mutex mutex;
      std::condition_variable condition;
      bool finished = false, cancelled = false;
    } state;
    struct Disconnect {
      GCancellable* source;
      gulong id = 0;
      ~Disconnect() {
        if (id) {
          g_cancellable_disconnect(source, id);
        }
      }
    } cancellation{gioCancellable(work.cancel)};
    cancellation.id = g_cancellable_connect(
        cancellation.source, G_CALLBACK(+[](GCancellable*, gpointer data) {
          auto& state = *static_cast<State*>(data);
          {
            std::scoped_lock lock(state.mutex);
            state.cancelled = true;
          }
          state.condition.notify_one();
        }),
        &state, nullptr
    );
    std::thread watchdog([&] {
      std::unique_lock lock(state.mutex);
      const auto done = [&] { return state.finished || state.cancelled; };
      if (work.deadline == Clock::time_point::max()) {
        state.condition.wait(lock, done);
      } else {
        state.condition.wait_until(lock, work.deadline, done);
      }
      if (!state.finished) {
        g_subprocess_force_exit(child.process.get());
      }
    });
    std::exception_ptr failure;
    try {
      copyOutput(child, output, maximum, work);
      child.reaped = g_subprocess_wait(child.process.get(), nullptr, nullptr);
    } catch (...) {
      failure = std::current_exception();
      g_subprocess_force_exit(child.process.get());
      child.reaped = g_subprocess_wait(child.process.get(), nullptr, nullptr);
    }
    {
      std::scoped_lock lock(state.mutex);
      state.finished = true;
    }
    state.condition.notify_one();
    watchdog.join();
    if (failure) {
      std::rethrow_exception(failure);
    }
    work.check();
    if (!child.reaped) {
      throwFileError(FileErrorKind::Other, "Could not wait for the archive provider.");
    }
  }
} // namespace

int luciole::archive_work::runArchiveProvider(
    Input& input, Work& work, std::string_view password, std::span<const char* const> argv, int output,
    std::uint64_t outputLimit, std::uint64_t fileLimit
) {
  work.check();
  if (::ftruncate(output, 0) != 0 || ::lseek(output, 0, SEEK_SET) < 0) {
    systemError("Could not prepare private archive output");
  }
  Stage scratch;
  const GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDERR_SILENCE | G_SUBPROCESS_FLAGS_STDOUT_PIPE)
  ));
  g_subprocess_launcher_set_cwd(launcher.get(), scratch.path.c_str());
  g_subprocess_launcher_setenv(launcher.get(), "LC_ALL", "C.UTF-8", true);
  g_subprocess_launcher_setenv(launcher.get(), "TMPDIR", scratch.path.c_str(), true);
  int sockets[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    systemError("Could not create the archive password channel");
  }
  const Fd sender(sockets[0]);
  g_subprocess_launcher_take_stdin_fd(launcher.get(), sockets[1]);
  const auto source = input.duplicateSingle();
  const int descriptor = ::fcntl(source.get(), F_DUPFD_CLOEXEC, 4);
  if (descriptor < 0) {
    systemError("Could not pass the held archive source");
  }
  g_subprocess_launcher_take_fd(launcher.get(), descriptor, 3);
  rlim_t seconds = RLIM_INFINITY;
  if (work.deadline != Clock::time_point::max()) {
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(work.deadline - Clock::now()).count();
    seconds = static_cast<rlim_t>(std::max<std::int64_t>(1, remaining + 1));
  }
  Limits limits{::getpid(), static_cast<rlim_t>(fileLimit), seconds};
  g_subprocess_launcher_set_child_setup(launcher.get(), childSetup, &limits, nullptr);
  Child child{GioPtr<GSubprocess>(g_subprocess_launcher_spawnv(launcher.get(), argv.data(), nullptr))};
  if (!child.process) {
    throwFileError(FileErrorKind::Unavailable, "Could not start the archive provider.");
  }
  sendSecret(sender.get(), password, work);
  sendSecret(sender.get(), "\n", work);
  (void)::shutdown(sender.get(), SHUT_WR);
  collect(child, output, outputLimit, work);
  input.unchanged();
  if (g_subprocess_get_if_signaled(child.process.get())) {
    if (g_subprocess_get_term_sig(child.process.get()) == SIGXFSZ) {
      throwFileError(FileErrorKind::Unsupported, "Archive data exceeds its expected size.");
    }
    throwFileError(FileErrorKind::Other, "The archive provider terminated before completing.");
  }
  const auto status = g_subprocess_get_exit_status(child.process.get());
  return status;
}
