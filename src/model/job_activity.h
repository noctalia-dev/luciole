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
} // namespace luciole
