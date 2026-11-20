#pragma once

#include "fs/background_job.h"
#include "fs/network_process.h"

#include <gio/gio.h>

namespace luciole {
  // IPC waits have separate bounded queues so stalled shares cannot occupy GIO's pool.
  void runIoTask(GTask* task, IoLane lane, GTaskThreadFunc work);

  template <typename Reply>
  void
  runIoJob(IoLane lane, std::move_only_function<Reply()> work, std::move_only_function<void(JobResult<Reply>&&)> done) {
    runJob<Reply>(std::move(work), std::move(done), lane);
  }

  template <typename Reply>
  void runIoJob(
      IoLane lane, CancelToken token, std::move_only_function<Reply()> work,
      std::move_only_function<void(JobResult<Reply>&&)> done
  ) {
    runJob<Reply>(std::move(token), std::move(work), std::move(done), lane);
  }
} // namespace luciole
