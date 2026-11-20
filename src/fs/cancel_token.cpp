#include "fs/cancel_token.h"

#include "fs/gio_interop.h"

struct luciole::CancelToken::State {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
};

struct luciole::CancelTokenAccess {
  static GCancellable* get(const CancelToken& token) { return token.m_state->cancellable.get(); }
  static CancelToken share(GCancellable* cancellable) {
    CancelToken token;
    if (cancellable) {
      token.m_state->cancellable.reset(G_CANCELLABLE(g_object_ref(cancellable)));
    }
    return token;
  }
};

luciole::CancelToken::CancelToken() : m_state(std::make_shared<State>()) {}
void luciole::CancelToken::cancel() const { g_cancellable_cancel(m_state->cancellable.get()); }
bool luciole::CancelToken::cancelled() const { return g_cancellable_is_cancelled(m_state->cancellable.get()) != FALSE; }

GCancellable* luciole::gioCancellable(const CancelToken& token) { return CancelTokenAccess::get(token); }
luciole::CancelToken luciole::cancellationToken(GCancellable* cancellable) {
  return CancelTokenAccess::share(cancellable);
}
