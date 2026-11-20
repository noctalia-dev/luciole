#include "fs/network_process.h"

#include "fs/gio_ptr.h"
#include "fs/network_job.h"
#include "fs/network_wire.h"
#include "network_worker_path.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <gio/gio.h>
#include <glib-unix.h>
#include <mutex>
#include <set>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

int luciole::workerLimit(IoLane lane) {
  using enum IoLane;
  switch (lane) {
  case PreviewDecode:
  case NetworkPreviewDecode:
    return 6;
  case Network:
  case Operation:
  case NetworkOperation:
    return 4;
  case Native:
  case PreviewProbe:
  case NetworkPreviewProbe:
    return 2;
  }
  std::unreachable();
}

namespace {
  bool worker = false;
  std::atomic<std::uint64_t> workerSequence{0};

  std::string executable() {
    if (const char* overridePath = std::getenv("LUCIOLE_IO_WORKER"); overridePath && g_path_is_absolute(overridePath)) {
      return overridePath;
    }
    const luciole::GString self(g_file_read_link("/proc/self/exe", nullptr));
    if (self) {
      const auto folder = std::filesystem::path(self.get()).parent_path();
      for (const auto& candidate : {folder / "luciole-io-worker", folder.parent_path() / "luciole-io-worker"}) {
        if (g_file_test(candidate.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
          return candidate.native();
        }
      }
    }
    return LUCIOLE_IO_WORKER_PATH;
  }

  void childSetup(gpointer data) {
    const auto parent = *static_cast<const pid_t*>(data);
    const rlimit core{0, 0};
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0
        || ::getppid() != parent
        || ::setsid() < 0
        || ::setrlimit(RLIMIT_CORE, &core) != 0
        || ::chdir("/") != 0) {
      ::_exit(126);
    }
  }
} // namespace

struct luciole::NetworkProcess::State : std::enable_shared_from_this<State> {
  GioPtr<GSubprocess> process;
  Fd socket, control;
  std::uint64_t sequence = 0;
  std::vector<std::string> environment;
  IoLane lane = IoLane::Native;
  std::atomic<bool> busy{false}, stopped{false}, reaped{false};
  std::atomic<unsigned> clients{0};
  std::atomic<gint64> lastUse{g_get_monotonic_time()};
  std::mutex releasesMutex;
  std::vector<std::uint64_t> releases;
  std::set<std::uint64_t> cleanupPending;
  std::atomic<bool> flushing{false};
  std::mutex launchMutex;
  std::condition_variable launched;
  bool launchComplete = false;
  std::optional<FileError> launchError;

  void launch(Fd child, Fd childControl) {
    struct Launch {
      std::shared_ptr<State> state;
      Fd child, control;
    };
    auto request = std::make_unique<Launch>(shared_from_this(), std::move(child), std::move(childControl));
    const auto start = +[](gpointer data) -> gboolean {
      const auto* request = static_cast<Launch*>(data);
      const auto& state = request->state;
      std::unique_lock lock(state->launchMutex);
      if (!state->stopped) {
        try {
          const GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(
              static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE)
          ));
          auto parent = ::getpid();
          g_subprocess_launcher_set_child_setup(launcher.get(), childSetup, &parent, nullptr);
          const int supplied = ::fcntl(request->child.get(), F_DUPFD_CLOEXEC, 5);
          if (supplied < 0) {
            throwErrno(errno);
          }
          g_subprocess_launcher_take_fd(launcher.get(), supplied, 3);
          const int control = ::fcntl(request->control.get(), F_DUPFD_CLOEXEC, 5);
          if (control < 0) {
            throwErrno(errno);
          }
          g_subprocess_launcher_take_fd(launcher.get(), control, 4);
          const auto path = executable();
          state->process.reset(g_subprocess_launcher_spawn(launcher.get(), nullptr, path.c_str(), nullptr));
          if (!state->process) {
            throwFileError(FileErrorKind::Unavailable, "Could not start the filesystem worker.");
          }
          g_subprocess_wait_async(
              state->process.get(), nullptr,
              [](GObject* source, GAsyncResult* result, gpointer held) {
                const std::unique_ptr<std::shared_ptr<State>> state(static_cast<std::shared_ptr<State>*>(held));
                (void)g_subprocess_wait_finish(G_SUBPROCESS(source), result, nullptr);
                (*state)->onExit();
              },
              new std::shared_ptr<State>(state)
          );
        } catch (...) {
          state->launchError = currentFileError();
          state->stopped = true;
          state->reaped = true;
        }
      }
      state->launchComplete = true;
      lock.unlock();
      state->launched.notify_all();
      return G_SOURCE_REMOVE;
    };
    // Linux ties PDEATHSIG to the spawning thread. The main thread outlives every prepared history record.
    if (::gettid() == ::getpid()) {
      (void)start(request.get());
    } else {
      auto* source = g_idle_source_new();
      g_source_set_priority(source, G_PRIORITY_DEFAULT);
      g_source_set_callback(source, start, request.release(), [](gpointer data) { delete static_cast<Launch*>(data); });
      g_source_attach(source, g_main_context_default());
      g_source_unref(source);
    }
    std::unique_lock lock(launchMutex);
    if (!launched.wait_for(lock, std::chrono::seconds(2), [&] { return launchComplete; })) {
      stopped = true;
      throwFileError(FileErrorKind::Unavailable, "Filesystem worker startup timed out.");
    }
    if (launchError) {
      throw FileErrorException(*launchError);
    }
  }

  struct Pool {
    std::mutex mutex;
    std::vector<std::weak_ptr<State>> processes;
    std::vector<FileError> cleanupErrors;
    guint idleTimer = 0;
  };
  static Pool& pool() {
    // IPC jobs can finish during process teardown, after the application's owners are gone.
    static auto* value = new Pool;
    return *value;
  }

  static void watchIdle(Pool& pool) {
    if (pool.idleTimer != 0) {
      return;
    }
    pool.idleTimer = g_timeout_add_seconds(
        30,
        [](gpointer data) -> gboolean {
          auto& pool = *static_cast<Pool*>(data);
          const std::scoped_lock lock(pool.mutex);
          const auto cutoff = g_get_monotonic_time() - 30 * G_TIME_SPAN_SECOND;
          std::erase_if(pool.processes, [&](const auto& entry) {
            const auto state = entry.lock();
            if (!state || state->reaped) {
              return true;
            }
            if (!state->stopped && !state->busy && state->clients == 0 && state->lastUse <= cutoff) {
              const std::scoped_lock releases(state->releasesMutex);
              if (state->releases.empty() && !state->flushing) {
                state->stop();
              }
            }
            return false;
          });
          if (!pool.processes.empty()) {
            return G_SOURCE_CONTINUE;
          }
          pool.idleTimer = 0;
          return G_SOURCE_REMOVE;
        },
        &pool
    );
  }

  static void reportLostCleanup(Pool& pool) {
    pool.cleanupErrors.push_back(
        {FileErrorKind::Other,
         "Retained-file cleanup could not be confirmed. Inspect adjacent .luciole-create-* folders."}
    );
  }

  void onExit() {
    auto& workers = pool();
    const std::scoped_lock lock(workers.mutex, releasesMutex);
    stopped = true;
    if (!cleanupPending.empty()) {
      reportLostCleanup(workers);
      cleanupPending.clear();
    }
    releases.clear();
    reaped = true;
  }

  void stop() {
    if (!stopped.exchange(true) && process) {
      g_subprocess_force_exit(process.get());
    }
  }

  void flush() {
    {
      const std::scoped_lock lock(releasesMutex);
      if (stopped || busy || releases.empty() || flushing.exchange(true)) {
        return;
      }
    }
    runIoJob<bool>(
        lane,
        [state = shared_from_this()] {
          NetworkProcess client(state);
          (void)client.call({{"action", "noop"}}, CancelToken{});
          return true;
        },
        [state = shared_from_this()](JobResult<bool> reply) {
          state->flushing = false;
          if (reply || state->stopped) {
            state->flush();
          } else {
            // A full queue must not turn cleanup retries into a main-context busy loop.
            g_timeout_add_full(
                G_PRIORITY_DEFAULT, 100,
                [](gpointer data) -> gboolean {
                  (*static_cast<std::shared_ptr<State>*>(data))->flush();
                  return G_SOURCE_REMOVE;
                },
                new std::shared_ptr<State>(state),
                [](gpointer data) { delete static_cast<std::shared_ptr<State>*>(data); }
            );
          }
        }
    );
  }
};

luciole::NetworkProcess::NetworkProcess(std::shared_ptr<State> state) : m_state(std::move(state)) {
  ++m_state->clients;
}
luciole::NetworkProcess::~NetworkProcess() {
  m_state->lastUse = g_get_monotonic_time();
  --m_state->clients;
  if (m_reserved) {
    m_state->busy = false;
  }
  m_state->flush();
}
luciole::IoLane luciole::NetworkProcess::lane() const { return m_state->lane; }
bool luciole::NetworkProcess::alive() const { return !m_state->stopped; }

std::shared_ptr<luciole::NetworkProcess> luciole::NetworkProcess::acquire(IoLane lane) {
  auto& pool = State::pool();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  std::unique_lock lock(pool.mutex);
  while (true) {
    std::erase_if(pool.processes, [](const auto& entry) {
      const auto state = entry.lock();
      return !state || state->reaped;
    });
    for (const auto& entry : pool.processes) {
      if (const auto state = entry.lock();
          state && state->lane == lane && !state->stopped && state->clients == 0 && !state->busy.exchange(true)) {
        auto result = std::shared_ptr<NetworkProcess>(new NetworkProcess(state));
        result->m_reserved = true;
        return result;
      }
    }
    const auto count = std::ranges::count_if(pool.processes, [lane](const auto& entry) {
      const auto state = entry.lock();
      return state && state->lane == lane;
    });
    if (count >= workerLimit(lane)) {
      // Prefer an unowned helper or a free slot before sharing retained service/history state.
      for (const auto& entry : pool.processes) {
        if (const auto state = entry.lock();
            state && state->lane == lane && !state->stopped && !state->busy.exchange(true)) {
          auto result = std::shared_ptr<NetworkProcess>(new NetworkProcess(state));
          result->m_reserved = true;
          return result;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throwFileError(FileErrorKind::Unavailable, "Filesystem workers are busy. Refresh to retry.");
      }
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      lock.lock();
      continue;
    }
    auto state = std::make_shared<State>();
    int pair[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
      throwErrno(errno);
    }
    state->socket = Fd(pair[0]);
    Fd child(pair[1]);
    int cancellation[2]{};
    if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, cancellation) != 0) {
      throwErrno(errno);
    }
    Fd childControl(cancellation[1]);
    state->control = Fd(cancellation[0]);
    state->lane = lane;
    state->busy = true;
    pool.processes.emplace_back(state);
    State::watchIdle(pool);
    lock.unlock();
    state->launch(std::move(child), std::move(childControl));
    auto result = std::shared_ptr<NetworkProcess>(new NetworkProcess(std::move(state)));
    result->m_reserved = true;
    return result;
  }
}

luciole::NetworkMessage luciole::NetworkProcess::call(
    NetworkMessage request, const CancelToken& cancel, NetworkEvent event, std::chrono::milliseconds timeout,
    int descriptor
) {
  const auto action = request.value("action", std::string());
  const bool mutating = request.value("mutating", false);
  const bool reserved = m_reserved.exchange(false);
  if (cancel.cancelled()) {
    if (reserved) {
      m_state->busy = false;
    }
    throwFileError(FileErrorKind::Cancelled, "Filesystem request cancelled before it started.");
  }
  if (m_state->stopped) {
    if (reserved) {
      m_state->busy = false;
    }
    throwFileError(FileErrorKind::Unavailable, "The filesystem worker is unavailable. Refresh to retry.");
  }
  if (!reserved && m_state->busy.exchange(true)) {
    if (!request.contains("job") && !request.contains("object") && action != "reverse" && action != "noop") {
      return acquire(m_state->lane)->call(std::move(request), cancel, std::move(event), timeout, descriptor);
    }
    const auto available = std::chrono::steady_clock::now() + timeout;
    while (m_state->busy.exchange(true)) {
      if (cancel.cancelled() || m_state->stopped || std::chrono::steady_clock::now() >= available) {
        throwFileError(
            cancel.cancelled() ? FileErrorKind::Cancelled : FileErrorKind::Unavailable,
            "The filesystem worker is busy. Try again."
        );
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  struct Release {
    std::shared_ptr<State> state;
    std::vector<std::uint64_t> ids;
    bool acknowledged = false;
    ~Release() {
      if (acknowledged) {
        const std::scoped_lock lock(state->releasesMutex);
        for (const auto id : ids) {
          state->cleanupPending.erase(id);
        }
      }
      state->busy = false;
      state->flush();
    }
  } release{.state = m_state, .ids = {}, .acknowledged = false};
  bool reportedError = false, submitted = false;
  try {
    if (action == "noop") {
      const std::scoped_lock lock(m_state->releasesMutex);
      release.ids = std::exchange(m_state->releases, {});
      request["release"] = release.ids;
    }
    std::vector<std::string> environment;
    const std::unique_ptr<char*, decltype(&g_strfreev)> current(g_get_environ(), g_strfreev);
    for (char** item = current.get(); *item; ++item) {
      environment.emplace_back(*item);
    }
    if (environment != m_state->environment) {
      for (const auto& variable : m_state->environment) {
        const auto name = variable.substr(0, variable.find('='));
        if (!g_environ_getenv(current.get(), name.c_str())) {
          request["unset"].push_back(name);
        }
      }
      request["environment"] = environment;
      m_state->environment = std::move(environment);
    }
    for (const auto resource : {RLIMIT_NOFILE, RLIMIT_FSIZE}) {
      rlimit limit{};
      if (::getrlimit(resource, &limit) == 0) {
        request["limits"].push_back(
            NetworkMessage::array({static_cast<int>(resource), static_cast<std::uint64_t>(limit.rlim_cur)})
        );
      }
    }
    char* status = nullptr;
    if (g_file_get_contents("/proc/self/status", &status, nullptr, nullptr)) {
      const GString owned(status);
      const std::string_view text(owned.get());
      if (const auto start = text.find("Umask:\t"); start != std::string_view::npos) {
        unsigned mask = 0;
        const auto digits = text.substr(start + 7, 4);
        if (std::from_chars(digits.data(), digits.data() + digits.size(), mask, 8).ec == std::errc{}) {
          request["umask"] = mask;
        }
      }
    }
    const auto sequence = ++m_state->sequence;
    request["sequence"] = sequence;
    const bool graceful = action == "operation" || action == "reverse";
    const CancelToken communication;
    bool cancelling = false;
    auto cancelDeadline = std::chrono::steady_clock::time_point::max();
    network_wire::send(m_state->socket.get(), request, cancel, descriptor);
    submitted = true;
    const auto progressInterval = std::min(timeout / 2, std::chrono::milliseconds(250));
    auto nextProgress = std::chrono::steady_clock::now() + progressInterval;
    std::chrono::steady_clock::time_point deadline;
    const auto tick = [&] {
      const auto now = std::chrono::steady_clock::now();
      if (now >= nextProgress || now >= deadline) {
        nextProgress = now + progressInterval;
        std::uint64_t advanced = 0;
        while (::recv(m_state->control.get(), &advanced, sizeof(advanced), MSG_DONTWAIT) == sizeof(advanced)) {
          if (advanced == sequence) {
            deadline = now + timeout;
          }
        }
      }
      if (graceful && cancel.cancelled() && !cancelling) {
        cancelling = true;
        cancelDeadline = now + std::chrono::seconds(2);
        (void)::send(m_state->control.get(), &sequence, sizeof(sequence), MSG_NOSIGNAL | MSG_DONTWAIT);
      }
      if (now >= cancelDeadline) {
        throwFileError(FileErrorKind::Cancelled, "Filesystem cancellation could not be confirmed.");
      }
    };
    while (true) {
      deadline = std::chrono::steady_clock::now() + timeout;
      auto reply =
          network_wire::receive(m_state->socket.get(), graceful ? communication : cancel, deadline, nullptr, tick);
      const auto kind = reply.at("kind").get<std::string>();
      if (kind == "error") {
        if (!reply.at("code").is_number_unsigned()
            || reply.at("code").get<std::uint64_t>() > std::to_underlying(FileErrorKind::Other)) {
          throwFileError(FileErrorKind::Unavailable, "Invalid filesystem worker error.");
        }
        reportedError = true;
        if (action == "noop") {
          m_state->stop();
        }
        throwFileError(
            static_cast<FileErrorKind>(reply.at("code").get<std::uint8_t>()), reply.at("message").get<std::string>()
        );
      }
      if (kind == "result") {
        auto& value = reply.at("value");
        if (value.is_object() && value.contains("cleanupErrors")) {
          auto& pool = State::pool();
          const std::scoped_lock lock(pool.mutex);
          for (const auto& error : value.at("cleanupErrors")) {
            pool.cleanupErrors.push_back(
                {error.at("kind").get<FileErrorKind>(), error.at("message").get<std::string>()}
            );
          }
        }
        release.acknowledged = true;
        return std::move(value);
      }
      if (kind != "event" || !event) {
        throwFileError(FileErrorKind::Unavailable, "Invalid filesystem worker response.");
      }
      const auto answer = event(std::move(reply.at("value")));
      if (!answer.is_null()) {
        network_wire::send(m_state->socket.get(), answer, graceful ? communication : cancel);
      }
    }
  } catch (...) {
    if (!reportedError) {
      m_state->stop();
      if (mutating && submitted) {
        throwFileError(
            FileErrorKind::Changed,
            "The filesystem request was interrupted. Changes may be incomplete. Inspect the source, destination and "
            "adjacent .luciole-* folders before retrying."
        );
      }
    }
    throw;
  }
}

void luciole::NetworkProcess::discard(std::uint64_t job, bool cleanup) {
  {
    auto& pool = State::pool();
    const std::scoped_lock lock(pool.mutex, m_state->releasesMutex);
    if (m_state->stopped) {
      if (cleanup) {
        State::reportLostCleanup(pool);
      }
      return;
    }
    m_state->releases.push_back(job);
    if (cleanup) {
      m_state->cleanupPending.insert(job);
    }
  }
  m_state->flush();
}

std::vector<luciole::FileError> luciole::takeNetworkCleanupErrors() {
  auto& pool = NetworkProcess::State::pool();
  const std::scoped_lock lock(pool.mutex);
  return std::exchange(pool.cleanupErrors, {});
}

bool luciole::networkCleanupPending() {
  auto& pool = NetworkProcess::State::pool();
  const std::scoped_lock lock(pool.mutex);
  return std::ranges::any_of(pool.processes, [](const auto& weak) {
    if (const auto state = weak.lock()) {
      const std::scoped_lock releases(state->releasesMutex);
      return !state->cleanupPending.empty();
    }
    return false;
  });
}

bool luciole::inNetworkWorker() { return worker; }
void luciole::noteWorkerIoProgress() {
  if (!worker) {
    return;
  }
  // Background file jobs share the control socket with the request thread. Datagrams preserve message boundaries.
  static thread_local gint64 next = 0;
  const auto now = g_get_monotonic_time();
  if (now >= next) {
    next = now + G_TIME_SPAN_SECOND;
    const auto sequence = workerSequence.load(std::memory_order_relaxed);
    (void)::send(4, &sequence, sizeof(sequence), MSG_DONTWAIT | MSG_NOSIGNAL);
  }
}
bool luciole::networkWorkerCancelled() {
  std::uint64_t sequence = 0;
  bool cancelled = false;
  while (::recv(4, &sequence, sizeof(sequence), MSG_DONTWAIT) == sizeof(sequence)) {
    cancelled |= sequence == workerSequence;
  }
  return cancelled;
}

int luciole::runNetworkWorker(const NetworkHandler& handler) {
  int type = 0;
  socklen_t length = sizeof(type);
  if (::getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &length) != 0
      || type != SOCK_STREAM
      || ::getsockopt(4, SOL_SOCKET, SO_TYPE, &type, &length) != 0
      || type != SOCK_DGRAM) {
    return EXIT_FAILURE;
  }
  worker = true;
  const CancelToken token;
  while (true) {
    try {
      bool readable = false;
      (void)g_unix_fd_add(
          3, G_IO_IN,
          [](gint, GIOCondition, gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &readable
      );
      while (!readable) {
        g_main_context_iteration(nullptr, TRUE);
      }
      Fd descriptor;
      const auto request = network_wire::receive(3, token, std::chrono::steady_clock::time_point::max(), &descriptor);
      if (request.contains("unset")) {
        for (const auto& name : request.at("unset")) {
          (void)::unsetenv(name.get_ref<const std::string&>().c_str());
        }
      }
      if (request.contains("environment")) {
        const auto next = request.at("environment").get<std::vector<std::string>>();
        for (const auto& entry : next) {
          if (const auto equal = entry.find('='); equal != std::string::npos) {
            (void)::setenv(entry.substr(0, equal).c_str(), entry.substr(equal + 1).c_str(), 1);
          }
        }
      }
      if (request.contains("umask")) {
        ::umask(request.at("umask").get<mode_t>());
      }
      for (const auto& value : request.value("limits", NetworkMessage::array())) {
        const auto resource = value.at(0).get<int>();
        if (resource != RLIMIT_NOFILE && resource != RLIMIT_FSIZE) {
          return EXIT_FAILURE;
        }
        rlimit limit{};
        if (::getrlimit(resource, &limit) != 0) {
          return EXIT_FAILURE;
        }
        limit.rlim_cur = std::min(limit.rlim_max, value.at(1).get<rlim_t>());
        if (::setrlimit(resource, &limit) != 0) {
          return EXIT_FAILURE;
        }
      }
      workerSequence = request.value("sequence", std::uint64_t{0});
      const NetworkEvent event = [&](NetworkMessage value) {
        const bool question = value.contains("question");
        network_wire::send(3, {{"kind", "event"}, {"value", std::move(value)}}, token);
        if (question) {
          return network_wire::receive(3, token, std::chrono::steady_clock::time_point::max());
        }
        return NetworkMessage{};
      };
      bool completed = false;
      try {
        auto result = handler(request, event, descriptor.get());
        completed = true;
        network_wire::send(3, {{"kind", "result"}, {"value", std::move(result)}}, token);
      } catch (...) {
        auto error = currentFileError();
        if (completed && request.value("mutating", false)) {
          error = {
              FileErrorKind::Changed,
              "The operation completed, but its result could not be delivered. Inspect the affected folders before "
              "retrying."
          };
        }
        network_wire::send(3, {{"kind", "error"}, {"code", error.kind}, {"message", error.message}}, token);
        if (completed && request.value("mutating", false)) {
          ::_exit(EXIT_FAILURE);
        }
      }
    } catch (...) {
      return EXIT_FAILURE;
    }
  }
}
