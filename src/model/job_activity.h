#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace luciole {
  enum class ActivityState { Preparing, Working, Waiting, Succeeded, Failed, Partial, Cancelled };
  struct JobActivity {
    std::uint64_t id = 0;
    std::string title, summary;
    ActivityState state = ActivityState::Preparing;
    std::optional<float> progress;
    std::shared_ptr<const std::string> report;
    // Transfer only: the row's live meters line, and every measured field for the details dialog.
    std::string meters, info;
    bool finished = false, canCancel = false, canRetry = false, canReview = false;
    bool failed() const { return state == ActivityState::Failed || state == ActivityState::Partial; }
  };

  // True while a job is still running or a failed one is still listed, so the user has something to look at.
  inline bool activitiesNeedAttention(std::span<const JobActivity> activities) {
    return std::ranges::any_of(activities, [](const JobActivity& item) { return !item.finished || item.failed(); });
  }

  // Average progress of the running jobs, for the toolbar button. Empty when none runs or any of them does not know
  // its progress, so the bar never claims more than is known.
  inline std::optional<float> runningProgress(std::span<const JobActivity> activities) {
    float sum = 0.0F;
    int running = 0;
    for (const auto& item : activities) {
      if (item.finished) {
        continue;
      }
      if (!item.progress) {
        return std::nullopt;
      }
      sum += *item.progress;
      ++running;
    }
    return running ? std::optional(sum / static_cast<float>(running)) : std::nullopt;
  }
} // namespace luciole
