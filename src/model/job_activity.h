#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace luciole {
  enum class ActivityState { Preparing, Working, Waiting, Succeeded, Failed, Partial, Cancelled };
  // Byte and file measurement of a running job.
  struct JobDetail {
    std::optional<std::uint64_t> bytes, bytesTotal, bytesPerSecond;
    std::optional<std::size_t> files, filesTotal;
    std::optional<std::int64_t> elapsedMs; // Duration of the finished attempt.
    std::size_t items = 0, itemsTotal = 0;
    std::string current;     // Display text of the item being processed.
    std::string meters;      // Row text: totals, speed and time; current item on a second line.
    std::string info;        // Details dialog text: every measured field, one per line.
    bool estimating = false; // The source size scan is still running.
    bool partial = false;    // The scan was bounded: totals are lower bounds.
  };
  struct JobActivity {
    std::uint64_t id = 0;
    std::string title, summary;
    ActivityState state = ActivityState::Preparing;
    std::optional<float> progress;
    std::shared_ptr<const std::string> report;
    std::optional<JobDetail> detail;
    bool finished = false, canCancel = false, canRetry = false, canReview = false;
    bool failed() const { return state == ActivityState::Failed || state == ActivityState::Partial; }
  };

  // True while a job is still running or a failed one is still listed, so the user has something to look at.
  inline bool activitiesNeedAttention(std::span<const JobActivity> activities) {
    return std::ranges::any_of(activities, [](const JobActivity& item) { return !item.finished || item.failed(); });
  }
} // namespace luciole
