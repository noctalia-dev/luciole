#include "platform/terminal_launcher.h"

#include "fs/cancel_token.h"
#include "fs/gio_interop.h"
#include "fs/system_mounts.h"
#include "platform/terminal_command.h"

struct luciole::TerminalLauncher::State {
  CancelToken token;
  std::string command;
  bool initialized = false, available = false;
  bool active = true;
};
luciole::TerminalLauncher::TerminalLauncher() : m_state(std::make_shared<State>()) { setCommand({}); }
luciole::TerminalLauncher::~TerminalLauncher() {
  m_state->active = false;
  m_state->token.cancel();
}
void luciole::TerminalLauncher::setCommand(std::string command) {
  if (m_state->initialized && m_state->command == command) {
    return;
  }
  m_state->command = std::move(command);
  m_state->initialized = true;
  try {
    m_state->available = !terminalArguments(m_state->command, false).empty();
  } catch (const FileErrorException&) {
    m_state->available = false;
  }
}
bool luciole::TerminalLauncher::available() const { return m_state->available; }
void luciole::TerminalLauncher::open(const Location& folder, std::function<void(std::string)> completed) {
  struct Request {
    std::shared_ptr<State> state;
    Location folder;
    std::string command;
    bool available;
    CancelToken token;
    std::function<void(std::string)> completed;
  };
  GTask* task = g_task_new(
      nullptr, gioCancellable(m_state->token),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
        GError* error = nullptr;
        GioPtr<GSubprocess> process(static_cast<GSubprocess*>(g_task_propagate_pointer(G_TASK(result), &error)));
        if (!request.state->active) {
          g_clear_error(&error);
          return;
        }
        if (!process) {
          std::string message = error ? error->message : "Could not launch the terminal.";
          g_clear_error(&error);
          request.completed(std::move(message));
          return;
        }
        auto* wait = new Request(request);
        g_subprocess_wait_check_async(
            process.get(), gioCancellable(request.state->token),
            [](GObject* source, GAsyncResult* result, gpointer data) {
              const std::unique_ptr<Request> request(static_cast<Request*>(data));
              GError* error = nullptr;
              const bool ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
              std::string message = ok ? "" : error ? error->message : "The terminal exited unsuccessfully.";
              g_clear_error(&error);
              if (request->state->active) {
                request->completed(std::move(message));
              }
            },
            wait
        );
      },
      nullptr
  );
  g_task_set_task_data(
      task, new Request{m_state, folder, m_state->command, m_state->available, m_state->token, std::move(completed)},
      [](gpointer data) { delete static_cast<Request*>(data); }
  );
  g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
    const auto& request = *static_cast<Request*>(data);
    if (!request.available) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No terminal is available. Check Settings > General > Terminal."
      );
      return;
    }
    const auto path = request.folder.nativePath();
    if (!path) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Open Terminal Here requires a native folder."
      );
      return;
    }
    Fd mount;
    try {
      mount = openSystemMount(request.folder, request.token);
    } catch (const FileErrorException& failure) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", failure.what());
      return;
    }
    GError* error = nullptr;
    const GioPtr<GFileInfo> info(g_file_query_info(
        gioFile(request.folder).get(), G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
        G_FILE_QUERY_INFO_NONE, cancellable, &error
    ));
    if (!info) {
      g_task_return_error(task, error);
      return;
    }
    if (g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY
        || (g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)
            && !g_file_info_get_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "The terminal location is not an accessible folder."
      );
      return;
    }
    if (g_task_return_error_if_cancelled(task)) {
      return;
    }
    const GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE));
    g_subprocess_launcher_set_cwd(launcher.get(), path->c_str());
    std::vector<std::string> arguments;
    try {
      arguments = terminalArguments(request.command, false, *path);
    } catch (const FileErrorException& failure) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", failure.what());
      return;
    }
    std::vector<const char*> raw;
    raw.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
      raw.push_back(argument.c_str());
    }
    raw.push_back(nullptr);
    auto* process = g_subprocess_launcher_spawnv(launcher.get(), raw.data(), &error);
    if (!process) {
      g_task_return_error(task, error);
    } else {
      g_task_return_pointer(task, process, g_object_unref);
    }
  });
  g_object_unref(task);
}
