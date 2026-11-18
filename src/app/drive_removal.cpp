#include "app/drive_removal.h"

#include <algorithm>
#include <chrono>
#include <glib.h>
#include <memory>
#include <unistd.h>
#include <utility>

namespace {
  struct Removal : std::enable_shared_from_this<Removal> {
    std::function<void(std::function<void(luciole::DriveRemoveResult)>)> remove;
    std::function<bool()> ready;
    std::function<void(luciole::DriveRemoveResult)> done;
    std::chrono::steady_clock::time_point deadline;
    luciole::DriveRemoveResult last;
    unsigned attempts = 0;

    void attempt() {
      ++attempts;
      remove([self = shared_from_this()](luciole::DriveRemoveResult result) {
        const bool ownFiles = result.busy
            && !result.blockers.empty()
            && std::ranges::all_of(result.blockers, [](const auto& blocker) { return blocker.pid == getpid(); });
        if (!ownFiles || self->attempts >= 8 || std::chrono::steady_clock::now() >= self->deadline) {
          self->done(std::move(result));
          return;
        }
        self->last = std::move(result);
        self->wait(250);
      });
    }
    void wait(unsigned milliseconds) {
      auto* held = new std::shared_ptr<Removal>(shared_from_this());
      g_timeout_add_full(
          G_PRIORITY_DEFAULT, milliseconds,
          +[](gpointer data) -> gboolean {
            const auto self = *static_cast<std::shared_ptr<Removal>*>(data);
            if (std::chrono::steady_clock::now() >= self->deadline) {
              self->done(std::move(self->last));
            } else if (self->ready()) {
              self->attempt();
            } else {
              self->wait(50);
            }
            return G_SOURCE_REMOVE;
          },
          held, +[](gpointer data) { delete static_cast<std::shared_ptr<Removal>*>(data); }
      );
    }
  };
} // namespace

void removeDriveAfterPreparation(
    const std::function<void(std::function<void(std::optional<luciole::FileError>)>)>& prepare,
    std::function<bool()> ready, std::function<void(std::function<void(luciole::DriveRemoveResult)>)> remove,
    std::function<void(luciole::DriveRemoveResult)> done
) {
  const auto request = std::make_shared<Removal>();
  request->remove = std::move(remove);
  request->ready = std::move(ready);
  request->done = std::move(done);
  prepare([request](std::optional<luciole::FileError> error) {
    if (error) {
      request->done({.error = std::move(error->message), .busy = false, .blockers = {}});
      return;
    }
    request->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    request->last = {
        .error = "Luciole is still releasing files on this drive. Try again.",
        .busy = true,
        .blockers = {{.pid = getpid(), .name = "Luciole"}}
    };
    if (request->ready()) {
      request->attempt();
    } else {
      request->wait(50);
    }
  });
}
