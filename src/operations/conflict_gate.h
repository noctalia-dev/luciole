#pragma once

#include "fs/cancel_token.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace luciole {
  // One pending worker question answered from the main thread. Conflict needs an
  // `id` member; ask() assigns it.
  template <typename Conflict, typename Choice> class ConflictGate {
  public:
    // Worker: publish conflict, call published() unlocked, then block until a
    // choice arrives or token is cancelled (nullopt).
    std::optional<Choice> ask(Conflict conflict, const CancelToken& token, const std::function<void()>& published) {
      std::unique_lock lock(m_mutex);
      m_choice.reset();
      conflict.id = ++m_next;
      m_conflict = std::move(conflict);
      lock.unlock();
      published();
      lock.lock();
      m_decision.wait(lock, [&] { return token.cancelled() || m_choice.has_value(); });
      auto choice = token.cancelled() ? std::nullopt : std::exchange(m_choice, std::nullopt);
      m_conflict.reset();
      m_choice.reset();
      return choice;
    }

    std::optional<Conflict> pending() const {
      const std::scoped_lock lock(m_mutex);
      return m_conflict;
    }

    // Main: answer the pending conflict with this id if accept(conflict) allows it.
    bool resolve(std::uint64_t id, Choice choice, const std::function<bool(const Conflict&)>& accept) {
      {
        const std::scoped_lock lock(m_mutex);
        if (!m_conflict || m_conflict->id != id || !accept(*m_conflict)) {
          return false;
        }
        m_choice = std::move(choice);
        m_conflict.reset();
      }
      m_decision.notify_all();
      return true;
    }

    // Main: wake a waiting worker after its token was cancelled.
    void wake() {
      {
        const std::scoped_lock lock(m_mutex);
      }
      m_decision.notify_all();
    }

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_decision;
    std::optional<Conflict> m_conflict;
    std::optional<Choice> m_choice;
    std::uint64_t m_next = 0;
  };
} // namespace luciole
