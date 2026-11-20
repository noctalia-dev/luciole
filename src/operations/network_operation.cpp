#include "operations/network_operation.h"

#include "fs/network_job.h"
#include "operations/conflict_gate.h"

#include <atomic>
#include <glib.h>
#include <mutex>

namespace {
  std::atomic<std::uint64_t> nextJob{1};
  struct Question {
    std::uint64_t id = 0;
    luciole::NetworkMessage value;
  };
} // namespace

struct luciole::NetworkOperation::State : std::enable_shared_from_this<State> {
  std::string service;
  std::uint64_t id = nextJob.fetch_add(1);
  std::uint64_t generation = 0;
  CancelToken token;
  std::atomic<bool> active{false};
  bool alive = true;
  bool awaitingDispatch = false;
  std::mutex mutex;
  std::shared_ptr<NetworkProcess> process;
  Delivery pending;
  GSource* source = nullptr;
  std::shared_ptr<GMainContext> context;
  ConflictGate<Question, NetworkMessage> questions;
  std::optional<NetworkMessage> visibleQuestion;

  void clearPending() {
    const std::scoped_lock lock(mutex);
    pending = {};
    if (source) {
      g_source_destroy(source);
      g_source_unref(std::exchange(source, nullptr));
    }
  }

  void publish(Delivery delivery) {
    const std::scoped_lock lock(mutex);
    pending = std::move(delivery);
    if (source) {
      return;
    }
    source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          const auto state = static_cast<std::weak_ptr<State>*>(data)->lock();
          if (!state) {
            return G_SOURCE_REMOVE;
          }
          Delivery action;
          {
            const std::scoped_lock lock(state->mutex);
            g_source_unref(std::exchange(state->source, nullptr));
            action = std::move(state->pending);
          }
          const bool submitting = std::exchange(state->awaitingDispatch, false);
          if (state->alive && (submitting || !state->token.cancelled()) && action) {
            action();
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
    g_source_attach(source, context.get());
  }

  void submit(
      IoLane lane, std::move_only_function<Delivery()> work, std::move_only_function<void(JobResult<Delivery>)> finish
  ) {
    // Let the initiating event cancel or destroy the job before any filesystem changes begin.
    awaitingDispatch = true;
    publish([lane, token = this->token, work = std::move(work), finish = std::move(finish)] mutable {
      if (token.cancelled()) {
        finish(std::unexpected(FileError{FileErrorKind::Cancelled, "Filesystem request cancelled before it started."}));
      } else {
        runIoJob<Delivery>(lane, std::move(token), std::move(work), std::move(finish));
      }
    });
  }
};

luciole::NetworkOperation::NetworkOperation(std::string service) : m_state(std::make_shared<State>()) {
  m_state->service = std::move(service);
}
luciole::NetworkOperation::~NetworkOperation() {
  m_state->alive = false;
  if (m_state->awaitingDispatch) {
    m_state->clearPending();
    m_state->active = false;
  }
  cancel();
}
bool luciole::NetworkOperation::busy() const { return m_state->active; }
void luciole::NetworkOperation::useProcess(std::shared_ptr<NetworkProcess> process) {
  const std::scoped_lock lock(m_state->mutex);
  if (m_state->process && m_state->process != process) {
    m_state->process->discard(m_state->id);
    m_state->id = nextJob.fetch_add(1);
  }
  m_state->process = std::move(process);
}
void luciole::NetworkOperation::cancel() {
  m_state->visibleQuestion.reset();
  m_state->token.cancel();
  m_state->questions.wake();
  if (!m_state->active) {
    const std::scoped_lock lock(m_state->mutex);
    if (m_state->process) {
      m_state->process->discard(m_state->id);
      m_state->process.reset();
    }
  }
}

std::optional<luciole::NetworkMessage> luciole::NetworkOperation::question() const { return m_state->visibleQuestion; }
bool luciole::NetworkOperation::answer(std::uint64_t id, NetworkMessage value) {
  if (!m_state->visibleQuestion || m_state->visibleQuestion->at("id").get<std::uint64_t>() != id) {
    return false;
  }
  m_state->visibleQuestion.reset();
  // As with initial dispatch, cancellation in this UI event must precede any filesystem changes.
  m_state->publish([state = m_state.get(), id, value = std::move(value)] mutable {
    state->questions.resolve(id, std::move(value), [](const Question&) { return true; });
  });
  return true;
}

bool luciole::NetworkOperation::start(
    std::string method, NetworkMessage arguments, IoLane lane, bool mutating, DecodeDelivery decode, ErrorDelivery error
) {
  const auto state = m_state;
  if (state->active) {
    return false;
  }
  state->active = true;
  state->context = std::shared_ptr<GMainContext>(g_main_context_ref_thread_default(), g_main_context_unref);
  state->token = CancelToken{};
  const auto generation = ++state->generation;
  NetworkMessage request{
      {"action", "operation"},
      {"service", state->service},
      {"method", std::move(method)},
      {"arguments", std::move(arguments)},
      {"job", state->id},
      {"mutating", mutating}
  };
  auto fail = std::make_shared<ErrorDelivery>(std::move(error));
  const auto jobLane = lane == IoLane::Network ? IoLane::NetworkOperation : IoLane::Operation;
  state->submit(
      jobLane,
      [state, request = std::move(request), jobLane, mutating, decode = std::move(decode), fail,
       token = state->token] mutable -> Delivery {
        bool received = false;
        try {
          std::shared_ptr<NetworkProcess> process;
          {
            const std::scoped_lock lock(state->mutex);
            process = state->process;
          }
          if (request.at("arguments").contains("receipt") && (!process || !process->alive())) {
            throwFileError(FileErrorKind::Changed, "The worker holding this file history is no longer available.");
          }
          if (!process || !process->alive()) {
            process = NetworkProcess::acquire(jobLane);
            const std::scoped_lock lock(state->mutex);
            state->process = process;
          }
          const auto reply = process->call(std::move(request), token, [&](const NetworkMessage& event) {
            if (event.contains("question")) {
              const auto answer = state->questions.ask(Question{.id = 0, .value = event.at("question")}, token, [&] {
                const auto pending = state->questions.pending();
                auto question = pending->value;
                question["id"] = pending->id;
                state->publish([state, question = std::move(question),
                                deliver = decode(event, process, false)] mutable {
                  state->visibleQuestion = std::move(question);
                  deliver();
                });
              });
              return answer.value_or(NetworkMessage{{"cancel", true}});
            }
            state->publish(decode(event, process, false));
            return NetworkMessage{};
          });
          received = true;
          return decode(reply, process, true);
        } catch (...) {
          auto failure = currentFileError();
          if (mutating && received) {
            failure = {
                FileErrorKind::Changed,
                "The operation returned an invalid result. Inspect the affected folders before retrying."
            };
          }
          return (*fail)(std::move(failure));
        }
      },
      [state, generation, fail](JobResult<Delivery> reply) {
        if (generation != state->generation) {
          return;
        }
        state->active = false;
        state->visibleQuestion.reset();
        if (state->token.cancelled()) {
          const std::scoped_lock lock(state->mutex);
          if (state->process) {
            state->process->discard(state->id);
            state->process.reset();
          }
        }
        state->clearPending();
        if (!reply) {
          reply = (*fail)(reply.error());
        }
        if (state->alive && reply && *reply) {
          (*reply)();
        }
      }
  );
  return true;
}
