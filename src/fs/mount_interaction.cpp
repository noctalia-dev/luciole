#include "fs/mount_interaction.h"

#include <utility>

namespace {
  std::uint64_t nextPrompt = 0;
  std::string text(const char* value) {
    const luciole::GString valid(g_utf8_make_valid(value ? value : "", -1));
    std::string result(valid.get());
    if (result.size() > 4096) {
      const auto* end = g_utf8_find_prev_char(result.data(), result.data() + 4096);
      result.resize(static_cast<std::size_t>(end - result.data()));
    }
    return result;
  }
} // namespace
struct luciole::MountInteraction::State : std::enable_shared_from_this<State> {
  GioPtr<GMountOperation> operation{g_mount_operation_new()};
  std::optional<MountPrompt> prompt;
  std::function<void()> changed;
  std::string problem;
  bool cancelled = false;
  void notify() const {
    if (const auto callback = changed) {
      callback();
    }
  }
  void unsupported() {
    problem = "The server requested an unsupported authentication method.";
    prompt.reset();
    g_mount_operation_reply(operation.get(), G_MOUNT_OPERATION_ABORTED);
    notify();
  }
};
luciole::MountInteraction::MountInteraction() : m_state(std::make_shared<State>()) {
  g_signal_connect(
      m_state->operation.get(), "ask-password",
      G_CALLBACK((+[](GMountOperation* operation, const char* message, const char* user, const char* domain,
                      GAskPasswordFlags flags, gpointer data) {
        g_signal_stop_emission_by_name(operation, "ask-password");
        const auto state = static_cast<State*>(data)->shared_from_this();
        constexpr unsigned supported = G_ASK_PASSWORD_NEED_PASSWORD
            | G_ASK_PASSWORD_NEED_USERNAME
            | G_ASK_PASSWORD_NEED_DOMAIN
            | G_ASK_PASSWORD_SAVING_SUPPORTED
            | G_ASK_PASSWORD_ANONYMOUS_SUPPORTED;
        if ((static_cast<unsigned>(flags) & ~supported) != 0) {
          state->unsupported();
          return;
        }
        state->prompt = MountPrompt{++nextPrompt, text(message), text(user), text(domain), flags, {}, true};
        state->notify();
      })),
      m_state.get()
  );
  g_signal_connect(
      m_state->operation.get(), "ask-question",
      G_CALLBACK((+[](GMountOperation* operation, const char* message, const char* const* choices, gpointer data) {
        g_signal_stop_emission_by_name(operation, "ask-question");
        const auto state = static_cast<State*>(data)->shared_from_this();
        MountPrompt prompt{++nextPrompt, text(message), {}, {}, {}, {}, false};
        for (std::size_t i = 0; choices && choices[i]; ++i) {
          if (i == 16) {
            state->unsupported();
            return;
          }
          prompt.choices.push_back(text(choices[i]));
        }
        if (prompt.choices.empty()) {
          state->unsupported();
          return;
        }
        state->prompt = std::move(prompt);
        state->notify();
      })),
      m_state.get()
  );
  g_signal_connect(
      m_state->operation.get(), "aborted", G_CALLBACK((+[](GMountOperation*, gpointer data) {
        const auto state = static_cast<State*>(data)->shared_from_this();
        state->prompt.reset();
        state->notify();
      })),
      m_state.get()
  );
}
luciole::MountInteraction::~MountInteraction() {
  m_state->changed = {};
  g_signal_handlers_disconnect_by_data(m_state->operation.get(), m_state.get());
  cancel();
  g_mount_operation_set_password(m_state->operation.get(), "");
}
GMountOperation* luciole::MountInteraction::operation() const { return m_state->operation.get(); }
const std::optional<luciole::MountPrompt>& luciole::MountInteraction::prompt() const { return m_state->prompt; }
const std::string& luciole::MountInteraction::problem() const { return m_state->problem; }
void luciole::MountInteraction::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::MountInteraction::answer(std::uint64_t id, const MountCredentials& credentials) {
  const auto state = m_state;
  if (!state->prompt
      || state->prompt->id != id
      || !state->prompt->password
      || (credentials.anonymous && !(state->prompt->flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED))
      || credentials.user.contains('\0')
      || credentials.domain.contains('\0')
      || credentials.password.contains('\0')) {
    return false;
  }
  const auto flags = state->prompt->flags;
  if (!credentials.anonymous && (flags & G_ASK_PASSWORD_NEED_USERNAME) && credentials.user.empty()) {
    return false;
  }
  const auto& user = flags & G_ASK_PASSWORD_NEED_USERNAME ? credentials.user : state->prompt->user;
  const auto& domain = flags & G_ASK_PASSWORD_NEED_DOMAIN ? credentials.domain : state->prompt->domain;
  g_mount_operation_set_anonymous(state->operation.get(), credentials.anonymous);
  g_mount_operation_set_username(state->operation.get(), credentials.anonymous ? "" : user.c_str());
  g_mount_operation_set_domain(state->operation.get(), credentials.anonymous ? "" : domain.c_str());
  g_mount_operation_set_password(state->operation.get(), credentials.anonymous ? "" : credentials.password.c_str());
  g_mount_operation_set_password_save(
      state->operation.get(),
      flags & G_ASK_PASSWORD_SAVING_SUPPORTED ? G_PASSWORD_SAVE_FOR_SESSION : G_PASSWORD_SAVE_NEVER
  );
  state->prompt.reset();
  g_mount_operation_reply(state->operation.get(), G_MOUNT_OPERATION_HANDLED);
  state->notify();
  return true;
}
bool luciole::MountInteraction::choose(std::uint64_t id, std::size_t choice) {
  const auto state = m_state;
  if (!state->prompt || state->prompt->id != id || state->prompt->password || choice >= state->prompt->choices.size()) {
    return false;
  }
  g_mount_operation_set_choice(state->operation.get(), static_cast<int>(choice));
  state->prompt.reset();
  g_mount_operation_reply(state->operation.get(), G_MOUNT_OPERATION_HANDLED);
  state->notify();
  return true;
}
bool luciole::MountInteraction::cancelled() const { return m_state->cancelled; }
void luciole::MountInteraction::cancel() {
  const auto state = m_state;
  g_mount_operation_set_password(state->operation.get(), "");
  if (state->prompt) {
    state->cancelled = true;
    state->prompt.reset();
    g_mount_operation_reply(state->operation.get(), G_MOUNT_OPERATION_ABORTED);
    state->notify();
  }
}
