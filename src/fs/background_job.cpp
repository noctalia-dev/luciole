#include "fs/background_job.h"

#include "fs/gio_interop.h"
#include "fs/network_job.h"

#include <gio/gio.h>

void luciole::detail::runInThread(std::move_only_function<void()> work, std::move_only_function<void()> done) {
  runInThread(std::nullopt, std::move(work), [done = std::move(done)](const std::optional<FileError>&) mutable {
    done();
  });
}

void luciole::detail::runInThread(
    std::optional<IoLane> lane, std::move_only_function<void()> work,
    std::move_only_function<void(std::optional<FileError>)> done
) {
  struct Request {
    std::move_only_function<void()> work;
    std::move_only_function<void(std::optional<FileError>)> done;
  };
  const GioPtr<GTask> task(g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        // GTask's worker reference may outlive delivery. Release completion captures
        // before returning, so finished services cannot retain hidden worker ownership.
        auto done = std::move(static_cast<Request*>(g_task_get_task_data(G_TASK(result)))->done);
        GError* error = nullptr;
        (void)g_task_propagate_boolean(G_TASK(result), &error);
        const GErrorPtr owned(error);
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
          done(FileError{FileErrorKind::Unavailable, error->message});
        } else {
          done(error ? std::optional{fileError(*error)} : std::nullopt);
        }
      },
      nullptr
  ));
  g_task_set_task_data(task.get(), new Request{std::move(work), std::move(done)}, [](gpointer data) {
    delete static_cast<Request*>(data);
  });
  const auto run = [](GTask* task, gpointer, gpointer data, GCancellable*) {
    {
      auto work = std::move(static_cast<Request*>(data)->work);
      work();
    }
    g_task_return_boolean(task, TRUE);
  };
  if (lane) {
    runIoTask(task.get(), *lane, run);
  } else {
    g_task_run_in_thread(task.get(), run);
  }
}
