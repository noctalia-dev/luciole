#include "fs/folder_totals.h"

#include "fs/background_job.h"
#include "fs/folder_totals_walk.h"

#include <algorithm>
#include <glib.h>
#include <mutex>
#include <utility>

namespace {
  struct Progress {
    std::mutex mutex;
    luciole::FolderTotalsSnapshot value;
  };
} // namespace

struct luciole::FolderTotalsService::State : std::enable_shared_from_this<State> {
  FolderTotalsSnapshot snapshot;
  CancelToken cancel;
  std::shared_ptr<Progress> progress;
  std::function<void()> changed;
  NetworkFilesystemCheck check;
  bool sameDeviceOnly = false;
  GSource* timer = nullptr;
  std::uint64_t serial = 0;
  bool alive = true, running = false;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void stopTimer() {
    if (timer) {
      g_source_destroy(timer);
      g_source_unref(std::exchange(timer, nullptr));
    }
  }
  void poll(bool sendNotification = true) {
    FolderTotalsSnapshot next;
    {
      const std::scoped_lock lock(progress->mutex);
      next = progress->value;
    }
    next.cancelRequested = snapshot.cancelRequested;
    if (next != snapshot) {
      snapshot = std::move(next);
      if (sendNotification) {
        notify();
      }
    }
  }
  void startTimer() {
    timer = g_timeout_source_new(100);
    g_source_set_callback(
        timer,
        [](gpointer data) -> gboolean {
          const auto state = static_cast<std::weak_ptr<State>*>(data)->lock();
          if (state && state->alive && state->timer) {
            state->poll();
            return state->alive && state->timer ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
    g_source_attach(timer, g_main_context_default());
  }
};

luciole::FolderTotalsService::FolderTotalsService() : m_state(std::make_shared<State>()) {}
luciole::FolderTotalsService::~FolderTotalsService() {
  m_state->alive = false;
  m_state->changed = {};
  clear();
}
bool luciole::FolderTotalsService::start(std::span<const DirectoryEntry> entries) {
  const auto state = m_state;
  if (entries.empty()) {
    return false;
  }
  // A start while a traversal runs supersedes it: clear() below invalidates the old completion through the serial
  // and cancels the old worker, so only the newest walk can publish.
  clear();
  std::vector<DirectoryEntry> roots(entries.begin(), entries.begin() + std::min<std::size_t>(4096, entries.size()));
  state->snapshot.phase = FolderTotalsPhase::Running;
  state->snapshot.limited = roots.size() != entries.size();
  state->cancel = CancelToken{};
  state->progress = std::make_shared<Progress>();
  state->progress->value = state->snapshot;
  state->running = true;
  state->startTimer();
  runJob<FolderTotalsSnapshot>(
      state->cancel,
      [roots = std::move(roots), token = state->cancel, progress = state->progress, limited = state->snapshot.limited,
       check = state->check, sameDevice = state->sameDeviceOnly] mutable {
        if (check && std::ranges::any_of(roots, [&check](const auto& root) {
              const auto native = root.location().nativePath();
              return native && check(*native);
            })) {
          FolderTotalsSnapshot snapshot;
          snapshot.phase = FolderTotalsPhase::Unsupported;
          snapshot.unsupported = roots.size();
          snapshot.limited = limited;
          {
            const std::scoped_lock lock(progress->mutex);
            progress->value = snapshot;
          }
          return snapshot;
        }
        return walkFolderTotals(
            std::move(roots), token,
            [progress, limited](const FolderTotalsSnapshot& value) {
              const std::scoped_lock lock(progress->mutex);
              progress->value = value;
              progress->value.limited |= limited;
            },
            FolderTotalsLimits{.sameDevice = sameDevice}
        );
      },
      [state, serial = state->serial](auto reply) {
        if (!state->alive || state->serial != serial) {
          return;
        }
        state->running = false;
        state->stopTimer();
        state->poll(false);
        if (!reply) {
          state->snapshot.phase =
              state->cancel.cancelled() ? FolderTotalsPhase::Cancelled : FolderTotalsPhase::Complete;
          if (reply.error().kind != FileErrorKind::Cancelled) {
            ++state->snapshot.unavailable;
            state->snapshot.error = reply.error().message.substr(0, 256);
          }
        }
        if (state->cancel.cancelled()) {
          state->snapshot.phase = FolderTotalsPhase::Cancelled;
        }
        state->notify();
      }
  );
  return true;
}
void luciole::FolderTotalsService::cancel() {
  if (m_state->running) {
    m_state->cancel.cancel();
    m_state->snapshot.cancelRequested = true;
  }
}
void luciole::FolderTotalsService::clear() {
  ++m_state->serial;
  m_state->cancel.cancel();
  m_state->running = false;
  m_state->stopTimer();
  m_state->snapshot = {};
}
bool luciole::FolderTotalsService::busy() const { return m_state->running; }
const luciole::FolderTotalsSnapshot& luciole::FolderTotalsService::snapshot() const { return m_state->snapshot; }
void luciole::FolderTotalsService::setChangedCallback(std::function<void()> changed) {
  m_state->changed = std::move(changed);
}
void luciole::FolderTotalsService::setNetworkFilesystemCheck(NetworkFilesystemCheck check) {
  m_state->check = std::move(check);
}
void luciole::FolderTotalsService::setSameDeviceOnly(bool enabled) { m_state->sameDeviceOnly = enabled; }
