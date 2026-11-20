#include "fs/network_job.h"

#include "fs/gio_interop.h"

#include <array>
#include <mutex>

namespace {
  struct Task {
    luciole::GioPtr<GTask> task;
    GTaskThreadFunc work;
  };
  struct Queue {
    std::mutex mutex;
    GThreadPool* pool = nullptr;
  };
  auto& queues() {
    // Pending completions may outlive application teardown.
    static auto* value = new std::array<Queue, 8>;
    return *value;
  }
} // namespace

void luciole::runIoTask(GTask* task, IoLane lane, GTaskThreadFunc work) {
  const auto index = static_cast<std::size_t>(lane);
  auto& queue = queues().at(index);
  const std::scoped_lock lock(queue.mutex);
  GError* error = nullptr;
  if (!queue.pool) {
    queue.pool = g_thread_pool_new(
        [](gpointer data, gpointer) {
          const std::unique_ptr<Task> request(static_cast<Task*>(data));
          auto* task = request->task.get();
          if (!g_task_return_error_if_cancelled(task)) {
            try {
              request->work(
                  task, g_task_get_source_object(task), g_task_get_task_data(task), g_task_get_cancellable(task)
              );
            } catch (...) {
              const auto failure = luciole::currentFileError();
              g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", failure.message.c_str());
            }
          }
        },
        nullptr, workerLimit(lane), FALSE, &error
    );
  }
  if (error) {
    g_task_return_error(task, error);
  } else if (g_thread_pool_unprocessed(queue.pool) >= 64) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, "Too many pending filesystem requests.");
  } else {
    auto request = std::make_unique<Task>(GioPtr<GTask>(G_TASK(g_object_ref(task))), work);
    // GLib takes ownership even if starting an additional pool thread fails.
    g_thread_pool_push(queue.pool, request.release(), nullptr);
  }
}
