#include "fs/location_mount.h"

#include "fs/gio_interop.h"

#include <utility>

struct luciole::LocationMount::State {
  GioPtr<GCancellable> cancellable;
  std::unique_ptr<MountInteraction> interaction = std::make_unique<MountInteraction>();
  std::function<void()> changed;
  std::function<void(std::expected<Location, FileError>)> done;
  struct Request {
    std::weak_ptr<State> state;
    GioPtr<GCancellable> token;
    GioPtr<GMountOperation> operation;
    Location location;
    bool mountable;
  };
  static void finish(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<Request> request(static_cast<Request*>(data));
    GError* error = nullptr;
    GioPtr<GFile> root;
    bool mounted = false;
    if (request->mountable) {
      root.reset(g_file_mount_mountable_finish(G_FILE(source), result, &error));
      mounted = root != nullptr;
    } else {
      mounted = g_file_mount_enclosing_volume_finish(G_FILE(source), result, &error) != FALSE;
    }
    const GErrorPtr held(error);
    const auto current = request->state.lock();
    if (!current || current->cancellable.get() != request->token.get()) {
      return;
    }
    if (request->mountable && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
      request->mountable = false;
      g_file_mount_enclosing_volume(
          G_FILE(source), G_MOUNT_MOUNT_NONE, request->operation.get(), request->token.get(), finish, request.get()
      );
      (void)request.release();
      return;
    }
    std::expected<Location, FileError> outcome = root ? locationFromFile(root.get()) : request->location;
    if (!mounted && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
      auto failure = error ? fileError(*error) : FileError{FileErrorKind::Other, "Could not mount the location."};
      if (current->interaction->cancelled()) {
        failure = FileError{FileErrorKind::Cancelled, "Connection cancelled."};
      } else if (!current->interaction->problem().empty()) {
        failure = FileError{FileErrorKind::Unsupported, current->interaction->problem()};
      }
      outcome = std::unexpected(std::move(failure));
    }
    current->cancellable.reset();
    current->interaction->setChangedCallback({});
    current->interaction = std::make_unique<MountInteraction>();
    if (const auto callback = std::exchange(current->done, {})) {
      callback(std::move(outcome));
    }
  }
};
luciole::LocationMount::LocationMount() : m_state(std::make_shared<State>()) {}
luciole::LocationMount::~LocationMount() {
  m_state->changed = {};
  cancel();
}
void luciole::LocationMount::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
luciole::MountInteraction& luciole::LocationMount::interaction() { return *m_state->interaction; }
void luciole::LocationMount::cancel() {
  m_state->done = {};
  if (m_state->cancellable) {
    g_cancellable_cancel(m_state->cancellable.get());
    m_state->cancellable.reset();
  }
  m_state->interaction->setChangedCallback({});
  m_state->interaction->cancel();
}
void luciole::LocationMount::start(
    const Location& location, std::function<void(std::expected<Location, FileError>)> done, bool mountable
) {
  cancel();
  const auto state = m_state;
  state->interaction = std::make_unique<MountInteraction>();
  state->interaction->setChangedCallback([weak = std::weak_ptr(state)] {
    if (const auto current = weak.lock(); current && current->changed) {
      const auto callback = current->changed;
      callback();
    }
  });
  state->cancellable.reset(g_cancellable_new());
  state->done = std::move(done);
  auto* request = new State::Request{
      state, GioPtr<GCancellable>(G_CANCELLABLE(g_object_ref(state->cancellable.get()))),
      GioPtr<GMountOperation>(G_MOUNT_OPERATION(g_object_ref(state->interaction->operation()))), location, mountable
  };
  const auto file = gioFile(location);
  if (mountable) {
    g_file_mount_mountable(
        file.get(), G_MOUNT_MOUNT_NONE, request->operation.get(), request->token.get(), State::finish, request
    );
  } else {
    g_file_mount_enclosing_volume(
        file.get(), G_MOUNT_MOUNT_NONE, request->operation.get(), request->token.get(), State::finish, request
    );
  }
}
