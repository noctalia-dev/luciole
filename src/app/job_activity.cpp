#include "app/job_activity.h"

#include "app/operation_hub.h"
#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <array>
#include <format>
#include <glib.h>
#include <string_view>

namespace {
  using luciole::ActivityState;
  using luciole::JobActivity;
  constexpr std::size_t kReportBytes = 64 * 1024;
  std::string bounded(std::string_view text) {
    constexpr std::string_view suffix = "\nReport truncated.\n";
    const auto count = std::min(text.size(), kReportBytes);
    const luciole::GString valid(g_utf8_make_valid(text.data(), static_cast<gssize>(count)));
    std::string result(valid.get());
    if (result.size() > kReportBytes - suffix.size() || text.size() > count) {
      auto limit = std::min(result.size(), kReportBytes - suffix.size());
      while (limit && (static_cast<unsigned char>(result[limit]) & 0xc0U) == 0x80U) {
        --limit;
      }
      result.resize(limit);
      result += suffix;
    }
    return result;
  }
  std::string sizeText(std::uint64_t bytes) {
    const luciole::GString text(g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS));
    return text.get();
  }
  std::string itemName(const luciole::Location& location) { return location.displayName().value_or("Item"); }
  struct Report {
    std::string text;
    std::size_t rows = 0;
    void add(const std::string& name, const std::string& outcome, const std::optional<luciole::FileError>& error = {}) {
      if (rows++ >= 2048 || text.size() >= kReportBytes) {
        return;
      }
      text += name + "\n  " + outcome + (error ? ": " + error->message : "") + '\n';
    }
    void publish(JobActivity& activity, bool details) const {
      if (details) {
        activity.report = std::make_shared<const std::string>(bounded(
            activity.title + "\n" + activity.summary + "\n\n" + text + (rows > 2048 ? "\nItem list truncated.\n" : "")
        ));
      }
    }
  };
  struct Counts {
    std::size_t done = 0, failed = 0, cancelled = 0, skipped = 0;
    bool partial = false;
    void finish(JobActivity& activity) const {
      activity.finished = true;
      activity.state = failed || partial ? (done || partial ? ActivityState::Partial : ActivityState::Failed)
          : cancelled                    ? ActivityState::Cancelled
                                         : ActivityState::Succeeded;
      activity.summary = std::format("{} completed", done);
      if (failed) {
        activity.summary += std::format(" · {} failed", failed);
      }
      if (skipped) {
        activity.summary += std::format(" · {} skipped", skipped);
      }
      if (cancelled) {
        activity.summary += std::format(" · {} cancelled", cancelled);
      }
      if (partial) {
        activity.summary += " · Incomplete changes";
      }
    }
  };
  void progress(JobActivity& activity, std::size_t done, std::size_t total) {
    activity.state = ActivityState::Working;
    activity.summary = total ? std::format("{} of {} items", done, total) : std::format("{} items", done);
    if (total) {
      activity.progress = std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0F, 1.0F);
    }
  }
  void transferResults(JobActivity& activity, const std::vector<luciole::TransferResult>& results, bool details) {
    constexpr std::array names{"Moved", "Copied", "Skipped", "Failed", "Cancelled", "Partial", "Linked"};
    Counts counts;
    Report report;
    for (const auto& result : results) {
      switch (result.outcome) {
      case luciole::TransferOutcome::Moved:
      case luciole::TransferOutcome::Copied:
      case luciole::TransferOutcome::Linked:
        ++counts.done;
        break;
      case luciole::TransferOutcome::Skipped:
        ++counts.skipped;
        break;
      case luciole::TransferOutcome::Failed:
        ++counts.failed;
        break;
      case luciole::TransferOutcome::Cancelled:
        ++counts.cancelled;
        break;
      case luciole::TransferOutcome::Partial:
        counts.partial = true;
        break;
      }
      if (details) {
        const auto append = [&](auto&& self, const luciole::TransferResult& item, unsigned depth) -> void {
          if (report.rows >= 2048 || report.text.size() >= kReportBytes || depth > 128) {
            report.rows = 2049;
            return;
          }
          report.add(itemName(item.source), names.at(static_cast<std::size_t>(item.outcome)), item.error);
          for (const auto& child : item.children) {
            self(self, child, depth + 1);
          }
        };
        append(append, result, 0);
      }
    }
    counts.finish(activity);
    report.publish(activity, details);
  }
  void simple(
      JobActivity& activity, bool running, bool ready, bool succeeded, bool cancelled,
      const std::optional<luciole::FileError>& error, bool details, const std::string& name = ""
  ) {
    activity.finished = !running && !ready;
    activity.state = error && error->kind != luciole::FileErrorKind::Cancelled     ? ActivityState::Failed
        : cancelled || (error && error->kind == luciole::FileErrorKind::Cancelled) ? ActivityState::Cancelled
        : succeeded                                                                ? ActivityState::Succeeded
        : ready                                                                    ? ActivityState::Waiting
                                                                                   : ActivityState::Preparing;
    activity.summary = error ? error->message
        : cancelled          ? "Cancelled"
        : succeeded          ? "Completed"
        : ready              ? "Waiting for confirmation"
                             : "Preparing…";
    if (running && activity.state != ActivityState::Preparing) {
      activity.state = ActivityState::Working;
    }
    activity.canReview = ready;
    Report report;
    if (!name.empty()) {
      report.add(name, activity.summary);
    }
    report.publish(activity, details);
  }
  void transfer(const OperationJob& job, JobActivity& activity, bool details) {
    const auto& service = job.service<luciole::TransferService>();
    activity.title = service.action() == luciole::TransferAction::Copy ? "Copy files"
        : service.action() == luciole::TransferAction::Move            ? "Move files"
                                                                       : "Create links";
    if (job.historyReplay() != HistoryReplay::None) {
      activity.title = (job.historyReplay() == HistoryReplay::Undo ? "Undo: " : "Redo: ") + activity.title;
    }
    if (job.running()) {
      activity.state = service.bytes() || service.completed() ? ActivityState::Working : ActivityState::Preparing;
      activity.summary = std::format(
          "{} transferred · {} of {} source items", sizeText(service.bytes()), service.completed(), service.total()
      );
    } else {
      transferResults(activity, service.results(), details);
      activity.canRetry = service.retryable() > 0;
      if (details && !service.report().empty()) {
        activity.report = std::make_shared<const std::string>(bounded(service.report()));
      }
    }
  }
  void archive(const OperationJob& job, JobActivity& activity, bool details) {
    const auto& service = job.service<luciole::ArchiveService>();
    activity.title = "Archive operation";
    if (job.running()) {
      switch (service.phase()) {
      case luciole::ArchivePhase::Reading:
        activity.summary = "Reading archive…";
        break;
      case luciole::ArchivePhase::Compressing:
        activity.summary = "Compressing…";
        break;
      case luciole::ArchivePhase::Extracting:
        activity.summary = "Extracting…";
        break;
      case luciole::ArchivePhase::Transferring:
        activity.summary = "Publishing output…";
        break;
      case luciole::ArchivePhase::Cleaning:
        activity.summary = "Cleaning up…";
        break;
      case luciole::ArchivePhase::WaitingPassword:
        activity.summary = "Waiting for a password";
        activity.canReview = true;
        break;
      case luciole::ArchivePhase::Idle:
        activity.summary = "Preparing…";
        break;
      }
      activity.summary += std::format(" · {} entries · {} processed", service.items(), sizeText(service.bytes()));
      activity.state = activity.canReview ? ActivityState::Waiting : ActivityState::Working;
    } else if (!service.results().empty()) {
      transferResults(activity, service.results(), details);
    } else {
      simple(activity, false, false, !service.error(), false, service.error(), details);
    }
    if (service.error()) {
      const bool hasResults = !service.results().empty();
      if (!hasResults || !activity.failed()) {
        activity.state = service.error()->kind == luciole::FileErrorKind::Cancelled ? ActivityState::Cancelled
                                                                                    : ActivityState::Failed;
      }
      activity.summary = (hasResults ? activity.summary + " · " : "") + service.error()->message;
      if (details) {
        activity.report = std::make_shared<const std::string>(
            bounded((activity.report ? *activity.report + "\n" : "") + service.error()->message)
        );
      }
    }
  }
  void single(const OperationJob& job, JobActivity& activity, bool details) {
    if (job.kind() == JobKind::Rename) {
      const auto& service = job.service<luciole::RenameService>();
      const auto& snapshot = service.snapshot();
      activity.title = job.historyReplay() == HistoryReplay::Undo ? "Undo rename"
          : job.historyReplay() == HistoryReplay::Redo            ? "Redo rename"
                                                                  : "Rename item";
      simple(
          activity, job.running(), service.canRename(), snapshot.status == luciole::RenameStatus::Succeeded,
          snapshot.status == luciole::RenameStatus::Cancelled, snapshot.error, details,
          snapshot.source ? itemName(*snapshot.source) : ""
      );
      if (snapshot.status == luciole::RenameStatus::Renaming) {
        activity.state = ActivityState::Working;
        activity.summary = "Renaming…";
      }
    } else {
      const auto& service = job.service<luciole::CreateService>();
      const auto& snapshot = service.snapshot();
      activity.title = snapshot.kind == luciole::CreateKind::Folder ? "Create folder"
          : snapshot.kind == luciole::CreateKind::EmptyFile         ? "Create file"
          : snapshot.kind == luciole::CreateKind::Document          ? "Create document"
                                                                    : "Create link";
      if (job.historyReplay() != HistoryReplay::None) {
        activity.title = (job.historyReplay() == HistoryReplay::Undo ? "Undo: " : "Redo: ") + activity.title;
      }
      simple(
          activity, job.running(), service.canCreate(), snapshot.status == luciole::CreateStatus::Succeeded,
          snapshot.status == luciole::CreateStatus::Cancelled, snapshot.error, details,
          snapshot.destination ? itemName(*snapshot.destination) : ""
      );
      if (snapshot.status == luciole::CreateStatus::Creating) {
        activity.state = ActivityState::Working;
        activity.summary = job.historyReplay() == HistoryReplay::Undo ? "Undoing creation…" : "Creating…";
      }
      if (snapshot.status == luciole::CreateStatus::Partial) {
        activity.state = ActivityState::Partial;
      }
    }
  }
  void trash(const OperationJob& job, JobActivity& activity, bool details) {
    Counts counts;
    Report report;
    if (job.kind() == JobKind::Trash) {
      const auto& service = job.service<luciole::TrashService>();
      const auto& snapshot = service.snapshot();
      activity.title = "Move to Trash";
      simple(
          activity, job.running(), service.canTrash(), snapshot.status == luciole::TrashStatus::Finished,
          snapshot.status == luciole::TrashStatus::Cancelled, snapshot.error, false
      );
      if (job.running() && snapshot.status == luciole::TrashStatus::Trashing) {
        progress(activity, service.completed(), snapshot.results.size());
      }
      if (!job.running()) {
        for (const auto& result : snapshot.results) {
          if (result.outcome == luciole::TrashOutcome::Trashed) {
            ++counts.done;
          } else if (result.outcome == luciole::TrashOutcome::Failed) {
            ++counts.failed;
          } else {
            ++counts.cancelled;
          }
          if (details) {
            report.add(
                itemName(result.source), result.outcome == luciole::TrashOutcome::Trashed ? "Trashed" : "Not trashed",
                result.error
            );
          }
        }
      }
      if (snapshot.status == luciole::TrashStatus::Finished) {
        counts.finish(activity);
      }
    } else {
      const auto& service = job.service<luciole::EmptyTrashService>();
      const auto& snapshot = service.snapshot();
      activity.title = snapshot.outsideTrash ? "Delete permanently"
          : snapshot.selection               ? "Delete from Trash"
                                             : "Empty Trash";
      simple(
          activity, job.running(), service.canEmpty(), snapshot.status == luciole::EmptyTrashStatus::Finished,
          snapshot.status == luciole::EmptyTrashStatus::Cancelled, snapshot.error, false
      );
      if (job.running() && snapshot.status == luciole::EmptyTrashStatus::Emptying) {
        progress(activity, service.completed(), snapshot.results.size());
      }
      if (!job.running()) {
        for (const auto& result : snapshot.results) {
          if (result.outcome == luciole::EmptyTrashOutcome::Deleted) {
            ++counts.done;
          } else if (result.outcome == luciole::EmptyTrashOutcome::Skipped) {
            ++counts.skipped;
          } else if (result.outcome == luciole::EmptyTrashOutcome::Cancelled) {
            ++counts.cancelled;
          } else {
            ++counts.failed;
            counts.partial |= result.outcome == luciole::EmptyTrashOutcome::Unconfirmed;
          }
          if (details) {
            report.add(
                result.displayName,
                result.outcome == luciole::EmptyTrashOutcome::Deleted           ? "Deleted"
                    : result.outcome == luciole::EmptyTrashOutcome::Unconfirmed ? "Deletion unconfirmed"
                                                                                : "Not deleted",
                result.error
            );
          }
        }
      }
      if (snapshot.status == luciole::EmptyTrashStatus::Finished) {
        counts.finish(activity);
      }
    }
    report.publish(activity, details);
  }
  void restore(const OperationJob& job, JobActivity& activity, bool details) {
    const auto& service = job.service<luciole::RestoreService>();
    activity.title = service.moving() ? "Move out of Trash" : "Restore from Trash";
    if (job.running()) {
      activity.state = ActivityState::Working;
      progress(activity, service.completed(), service.total());
      return;
    }
    Counts counts;
    Report report;
    for (const auto& result : service.results()) {
      if (result.outcome == luciole::RestoreOutcome::Restored) {
        ++counts.done;
      } else if (result.outcome == luciole::RestoreOutcome::Skipped) {
        ++counts.skipped;
      } else if (result.outcome == luciole::RestoreOutcome::Cancelled) {
        ++counts.cancelled;
      } else {
        ++counts.failed;
        counts.partial |= result.outcome == luciole::RestoreOutcome::Unconfirmed;
      }
      if (details) {
        report.add(
            result.displayName,
            result.outcome == luciole::RestoreOutcome::Restored ? (service.moving() ? "Moved" : "Restored")
                : result.outcome == luciole::RestoreOutcome::Unconfirmed
                ? (service.moving() ? "Move unconfirmed" : "Restoration unconfirmed")
                : (service.moving() ? "Not moved" : "Not restored"),
            result.error
        );
      }
    }
    counts.finish(activity);
    report.publish(activity, details);
  }
  void editing(const OperationJob& job, JobActivity& activity, bool details) {
    activity.state = ActivityState::Working;
    if (job.kind() == JobKind::BatchRename) {
      const auto& service = job.service<luciole::BatchRenameService>();
      const auto& snapshot = service.snapshot();
      activity.title = job.historyReplay() == HistoryReplay::Undo ? "Undo batch rename"
          : job.historyReplay() == HistoryReplay::Redo            ? "Redo batch rename"
                                                                  : "Batch rename";
      if (job.running()) {
        if (service.applying()) {
          progress(activity, service.completed(), snapshot.rows ? snapshot.rows->size() : 0);
        } else {
          activity.state = ActivityState::Preparing;
          activity.summary = "Checking rename preview…";
        }
      } else {
        simple(
            activity, false, snapshot.phase == luciole::BatchRenamePhase::Ready,
            snapshot.phase == luciole::BatchRenamePhase::Finished,
            snapshot.phase == luciole::BatchRenamePhase::Cancelled, snapshot.error, details
        );
        if (snapshot.results) {
          Counts counts;
          for (const auto& result : *snapshot.results) {
            if (result.outcome == luciole::BatchRenameOutcome::Renamed
                || result.outcome == luciole::BatchRenameOutcome::Unchanged) {
              ++counts.done;
            } else if (result.outcome == luciole::BatchRenameOutcome::Cancelled) {
              ++counts.cancelled;
            } else {
              ++counts.failed;
              counts.partial |= result.outcome != luciole::BatchRenameOutcome::Failed;
            }
          }
          counts.finish(activity);
        }
      }
      if (details && !snapshot.report.empty()) {
        activity.report = std::make_shared<const std::string>(bounded(snapshot.report));
      }
    } else {
      const auto& service = job.service<luciole::PermissionService>();
      const auto& snapshot = service.snapshot();
      activity.title = "Change permissions";
      activity.finished = !job.running();
      if (job.running()) {
        if (service.collecting()) {
          activity.summary = "Collecting targets…";
        } else {
          progress(activity, service.completed(), service.total());
        }
      } else {
        activity.state = snapshot.failed() ? (snapshot.applied ? ActivityState::Partial : ActivityState::Failed)
            : snapshot.cancelRequested     ? ActivityState::Cancelled
                                           : ActivityState::Succeeded;
        activity.summary = snapshot.message.empty() ? "Permissions updated" : snapshot.message;
        if (snapshot.error) {
          activity.summary = snapshot.error->message;
        }
      }
      if (details) {
        activity.report =
            std::make_shared<const std::string>(bounded(snapshot.report.empty() ? activity.summary : snapshot.report));
      }
    }
  }
} // namespace

std::optional<luciole::JobActivity> jobActivity(const OperationJob& job, bool details) {
  if (!job.ran() || (job.kind() == JobKind::Permission && !job.activityStarted())) {
    return {};
  }
  luciole::JobActivity activity;
  activity.id = job.id();
  activity.canCancel = job.cancellable();
  switch (job.kind()) {
  case JobKind::Transfer:
    transfer(job, activity, details);
    break;
  case JobKind::Archive:
    archive(job, activity, details);
    break;
  case JobKind::Rename:
  case JobKind::Create:
    single(job, activity, details);
    break;
  case JobKind::Trash:
  case JobKind::EmptyTrash:
    trash(job, activity, details);
    break;
  case JobKind::Restore:
    restore(job, activity, details);
    break;
  case JobKind::BatchRename:
  case JobKind::Permission:
    editing(job, activity, details);
    break;
  }
  if (job.conflict()) {
    activity.state = luciole::ActivityState::Waiting;
    activity.summary = "Waiting for a conflict decision";
    activity.canReview = true;
  }
  if (activity.finished && !job.activityStarted() && !activity.failed() && job.kind() != JobKind::Archive) {
    return {};
  }
  if (details && !activity.report) {
    Report{}.publish(activity, true);
  }
  activity.title = bounded(activity.title);
  activity.summary = bounded(activity.summary);
  return activity;
}
