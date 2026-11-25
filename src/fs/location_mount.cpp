#include "fs/location_mount.h"

#include "fs/gio_interop.h"

#include <string>
#include <string_view>
#include <utility>

struct luciole::LocationMount::State : std::enable_shared_from_this<State> {
  GioPtr<GCancellable> cancellable;
  std::unique_ptr<MountInteraction> interaction = std::make_unique<MountInteraction>();
  std::function<void()> changed;
  std::function<void(std::expected<Location, FileError>)> done;
  void begin(std::function<void(std::expected<Location, FileError>)> callback, MountPurpose purpose) {
    interaction = std::make_unique<MountInteraction>(purpose);
    cancellable.reset(g_cancellable_new());
    done = std::move(callback);
    interaction->setChangedCallback([weak = weak_from_this()] {
      if (const auto current = weak.lock()) {
        if (current->interaction->cancelled() && current->cancellable) {
          g_cancellable_cancel(current->cancellable.get());
        }
        if (const auto changed = current->changed) {
          changed();
        }
      }
    });
  }
  void complete(std::expected<Location, FileError> outcome) {
    cancellable.reset();
    interaction->setChangedCallback({});
    interaction = std::make_unique<MountInteraction>();
    if (const auto callback = std::exchange(done, {})) {
      callback(std::move(outcome));
    }
  }
  struct Request {
    std::weak_ptr<State> state;
    GioPtr<GCancellable> token;
    GioPtr<GMountOperation> operation;
    std::optional<Location> location;
    bool mountable;
    GioPtr<GVolume> volume;
    GioPtr<GDrive> drive;
    ~Request() {
      if (drive) {
        g_signal_handlers_disconnect_by_data(drive.get(), this);
      }
    }
  };
  static void finish(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<Request> request(static_cast<Request*>(data));
    GError* error = nullptr;
    GioPtr<GFile> root;
    bool mounted = false;
    if (request->volume) {
      mounted = g_volume_mount_finish(G_VOLUME(source), result, &error) != FALSE;
      if (mounted || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
        const GioPtr<GMount> mount(g_volume_get_mount(G_VOLUME(source)));
        if (mount) {
          root.reset(g_mount_get_root(mount.get()));
        }
      }
    } else if (request->mountable) {
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
    std::expected<Location, FileError> outcome =
        std::unexpected(FileError{FileErrorKind::Other, "The drive mounted without an available folder."});
    if (g_cancellable_is_cancelled(request->token.get()) || current->interaction->cancelled()) {
      outcome = std::unexpected(FileError{FileErrorKind::Cancelled, "Mount cancelled."});
    } else if (!mounted && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
      auto failure = error ? fileError(*error) : FileError{FileErrorKind::Other, "Could not mount the location."};
      if (!current->interaction->problem().empty()) {
        failure = FileError{FileErrorKind::Unsupported, current->interaction->problem()};
      }
      outcome = std::unexpected(std::move(failure));
    } else if (root) {
      outcome = locationFromFile(root.get());
    } else if (request->location) {
      outcome = *request->location;
    }
    current->complete(std::move(outcome));
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
  state->begin(std::move(done), MountPurpose::Network);
  auto* request = new State::Request{
      state,
      GioPtr<GCancellable>(G_CANCELLABLE(g_object_ref(state->cancellable.get()))),
      GioPtr<GMountOperation>(G_MOUNT_OPERATION(g_object_ref(state->interaction->operation()))),
      location,
      mountable,
      {},
      {}
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

void luciole::LocationMount::startVolume(
    GVolume* volume, std::function<void(std::expected<Location, FileError>)> done
) {
  cancel();
  const auto state = m_state;
  state->begin(std::move(done), MountPurpose::Device);
  auto* request = new State::Request{
      state,
      GioPtr<GCancellable>(G_CANCELLABLE(g_object_ref(state->cancellable.get()))),
      GioPtr<GMountOperation>(G_MOUNT_OPERATION(g_object_ref(state->interaction->operation()))),
      std::nullopt,
      false,
      GioPtr<GVolume>(G_VOLUME(g_object_ref(volume))),
      GioPtr<GDrive>(g_volume_get_drive(volume))
  };
  // Unlocking can replace the encrypted GVolume. Only a departing physical drive means disconnection.
  if (request->drive) {
    g_signal_connect(
        request->drive.get(), "disconnected", G_CALLBACK(+[](GDrive*, gpointer data) {
          const auto& request = *static_cast<State::Request*>(data);
          g_cancellable_cancel(request.token.get());
          if (const auto current = request.state.lock(); current && current->cancellable.get() == request.token.get()) {
            current->interaction->cancel();
          }
        }),
        request
    );
  }
  g_volume_mount(volume, G_MOUNT_MOUNT_NONE, request->operation.get(), request->token.get(), State::finish, request);
}

void luciole::LocationMount::startFstabMount(
    const Location& target, std::function<void(std::expected<Location, FileError>)> done
) {
  cancel();
  const auto path = target.nativePath();
  if (!path) {
    done(std::unexpected(FileError{FileErrorKind::Unsupported, "A configured mount needs a local path."}));
    return;
  }
  GError* error = nullptr;
  const GioPtr<GSubprocess> process(
      g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_PIPE, &error, "mount", "--", path->c_str(), nullptr)
  );
  const GErrorPtr held(error);
  if (!process) {
    done(std::unexpected(fileError(*held)));
    return;
  }

  m_state->begin(std::move(done), MountPurpose::Network);
  auto* token = m_state->cancellable.get();
  g_signal_connect_object(
      token, "cancelled", G_CALLBACK(+[](GCancellable*, GSubprocess* child) { g_subprocess_force_exit(child); }),
      process.get(), G_CONNECT_DEFAULT
  );
  struct Request {
    std::weak_ptr<State> state;
    GioPtr<GCancellable> token;
    Location target;
    guint timeout = 0;
  };
  auto* request = new Request{m_state, GioPtr<GCancellable>(G_CANCELLABLE(g_object_ref(token))), target};
  request->timeout = g_timeout_add_seconds(
      10,
      [](gpointer data) -> gboolean {
        auto* request = static_cast<Request*>(data);
        request->timeout = 0;
        g_cancellable_cancel(request->token.get());
        return G_SOURCE_REMOVE;
      },
      request
  );
  g_subprocess_communicate_utf8_async(
      process.get(), nullptr, token,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Request> request(static_cast<Request*>(data));
        if (request->timeout) {
          g_source_remove(request->timeout);
        }
        GError* error = nullptr;
        char* stderrText = nullptr;
        const bool completed =
            g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, nullptr, &stderrText, &error);
        const GErrorPtr held(error);
        const GString message(stderrText);
        // mount ends its message with a newline; the error is shown on one line.
        std::string_view text = message ? std::string_view(message.get()) : std::string_view{};
        while (!text.empty() && g_ascii_isspace(text.back())) {
          text.remove_suffix(1);
        }
        const auto current = request->state.lock();
        if (!current || current->cancellable.get() != request->token.get()) {
          return;
        }
        if (completed && g_subprocess_get_successful(G_SUBPROCESS(source))) {
          current->complete(request->target);
        } else if (request->timeout == 0) {
          current->complete(
              std::unexpected(
                  FileError{FileErrorKind::Unavailable, "Stopped waiting for the mount. It may still complete."}
              )
          );
        } else if (!text.empty()) {
          current->complete(std::unexpected(FileError{FileErrorKind::Other, std::string(text)}));
        } else {
          const auto failure =
              held ? fileError(*held) : FileError{FileErrorKind::Other, "Could not mount the location."};
          current->complete(std::unexpected(failure));
        }
      },
      request
  );
}
