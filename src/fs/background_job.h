#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace luciole {
  enum class IoLane : std::uint8_t;
  namespace detail {
    void runInThread(std::move_only_function<void()> work, std::move_only_function<void()> done);
    void runInThread(
        std::optional<IoLane> lane, std::move_only_function<void()> work,
        std::move_only_function<void(std::optional<FileError>)> done
    );
  } // namespace detail

  template <typename Reply> using JobResult = std::expected<Reply, FileError>;

  // Runs work on GIO's worker pool or the selected I/O lane, then done on the calling thread's main context.
  // An exception from work is delivered as its FileError (see currentFileError).
  template <typename Reply>
  void runJob(
      std::move_only_function<Reply()> work, std::move_only_function<void(JobResult<Reply>&&)> done,
      std::optional<IoLane> lane = {}
  ) {
    auto result = std::make_shared<std::optional<JobResult<Reply>>>();
    detail::runInThread(
        lane,
        [result, work = std::move(work)] mutable {
          try {
            result->emplace(work());
          } catch (...) {
            result->emplace(std::unexpected(currentFileError()));
          }
        },
        [result, done = std::move(done)](std::optional<FileError> error) mutable {
          if (error) {
            result->emplace(std::unexpected(std::move(*error)));
          }
          done(std::move(**result));
        }
    );
  }

  // As above, but a token cancelled before the worker starts skips work and
  // delivers a Cancelled error.
  template <typename Reply>
  void runJob(
      CancelToken token, std::move_only_function<Reply()> work, std::move_only_function<void(JobResult<Reply>&&)> done,
      std::optional<IoLane> lane = {}
  ) {
    runJob<Reply>(
        [token = std::move(token), work = std::move(work)] mutable -> Reply {
          if (token.cancelled()) {
            throwFileError(FileErrorKind::Cancelled, "Cancelled.");
          }
          return work();
        },
        std::move(done), lane
    );
  }
} // namespace luciole
