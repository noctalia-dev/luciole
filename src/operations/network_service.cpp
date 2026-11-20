#include "operations/network_service.h"

#include "fs/network_info.h"

namespace {
  void addTarget(
      luciole::OperationTargets& targets, const luciole::Location& location,
      std::span<const luciole::MountConfiguration> configuration
  ) {
    if (targets.lane == luciole::IoLane::Native) {
      targets.lane = luciole::ioLane(location);
    }
    auto mount = luciole::automountFor(location, configuration);
    if (mount
        && std::ranges::find(targets.mounts, mount->path, &luciole::MountConfiguration::path) == targets.mounts.end()) {
      targets.mounts.push_back(std::move(*mount));
    }
  }
} // namespace

namespace luciole {
  void OperationTargets::set(std::span<const Location> locations, std::optional<Location> extra) {
    *this = {};
    const auto configuration = configuredSystemMounts();
    for (const auto& location : locations) {
      addTarget(*this, location, configuration);
    }
    if (extra) {
      addTarget(*this, *extra, configuration);
    }
  }
  void OperationTargets::set(std::span<const DirectoryEntry> entries, bool restoring) {
    *this = {};
    const auto configuration = configuredSystemMounts();
    for (const auto& entry : entries) {
      addTarget(*this, entry.location(), configuration);
      if (restoring && entry.trashInfo().original) {
        addTarget(*this, *entry.trashInfo().original, configuration);
      }
    }
  }
  void OperationTargets::add(const Location& location) { addTarget(*this, location, configuredSystemMounts()); }

  void beginRemote(RenameSnapshot& value, std::string_view method) {
    value.status = method == "prepare" ? RenameStatus::Preparing : RenameStatus::Renaming;
    value.error.reset();
    value.receipt.reset();
  }
  void beginRemote(CreateSnapshot& value, std::string_view method) {
    value.status = method.starts_with("prepare") ? CreateStatus::Preparing : CreateStatus::Creating;
    value.error.reset();
    value.receipt.reset();
  }
  void beginRemote(TrashSnapshot& value, std::string_view method) {
    value.status = method == "prepare" ? TrashStatus::Preparing : TrashStatus::Trashing;
    value.error.reset();
  }
  void beginRemote(EmptyTrashSnapshot& value, std::string_view method) {
    value.status =
        method == "empty" || method == "deleteSelection" ? EmptyTrashStatus::Emptying : EmptyTrashStatus::Preparing;
    value.error.reset();
  }
  void beginRemote(BatchRenameSnapshot& value, std::string_view method) {
    value.canApply = false;
    value.phase = method == "prepare" ? BatchRenamePhase::Preparing
        : method == "preview"         ? BatchRenamePhase::Previewing
                                      : BatchRenamePhase::Applying;
    value.error.reset();
    value.receipt.reset();
  }
  void beginRemote(PermissionSnapshot& value, std::string_view method) {
    if (method.starts_with("prepare")) {
      value = {};
      value.phase = PermissionPhase::Loading;
      return;
    }
    value.phase = PermissionPhase::Saving;
    value.error.reset();
    value.cancelRequested = value.applied = value.itemsFailed = value.finished = false;
    value.message.clear();
    value.aclOperation = method == "applyAcl" || method == "applyAclPatch";
    value.recursive = method == "applyRecursive";
  }
  void beginRemote(TransferView& value, std::string_view method) {
    if (method != "retryFailed") {
      if (method != "start") {
        value.results.clear();
      }
      value.report.clear();
      ++value.jobId;
    }
    value.receipt.reset();
    value.copyReceipt.reset();
    value.retryable = 0;
  }
  void beginRemote(RestoreView& value, std::string_view) {
    for (auto& result : value.results) {
      result.outcome = RestoreOutcome::Cancelled;
      result.error.reset();
    }
  }
  void beginRemote(ArchiveView& value, std::string_view method) {
    value.opened.reset();
    value.results.clear();
    if (method == "inspect") {
      value.catalog.reset();
    }
    value.phase = method == "inspect" ? ArchivePhase::Reading
        : method == "compress"        ? ArchivePhase::Compressing
                                      : ArchivePhase::Extracting;
    value.error.reset();
  }
  void failRemote(RenameSnapshot& value, FileError error) {
    value.status = error.kind == FileErrorKind::Cancelled ? RenameStatus::Cancelled : RenameStatus::Error;
    value.error = std::move(error);
  }
  void failRemote(CreateSnapshot& value, FileError error) {
    value.status = error.kind == FileErrorKind::Cancelled ? CreateStatus::Cancelled
        : error.kind == FileErrorKind::Changed            ? CreateStatus::Partial
                                                          : CreateStatus::Error;
    value.error = std::move(error);
    value.retireHistory = true;
  }
  void failRemote(TrashSnapshot& value, FileError error) {
    value.status = error.kind == FileErrorKind::Cancelled ? TrashStatus::Cancelled : TrashStatus::Error;
    value.error = std::move(error);
  }
  void failRemote(EmptyTrashSnapshot& value, FileError error) {
    value.status = error.kind == FileErrorKind::Cancelled ? EmptyTrashStatus::Cancelled : EmptyTrashStatus::Error;
    value.error = std::move(error);
  }
  void failRemote(BatchRenameSnapshot& value, FileError error) {
    value.phase = error.kind == FileErrorKind::Cancelled
        ? (value.phase == BatchRenamePhase::Applying && value.results ? BatchRenamePhase::Finished
                                                                      : BatchRenamePhase::Cancelled)
        : BatchRenamePhase::Error;
    value.error = std::move(error);
    value.canApply = false;
  }
  void failRemote(PermissionSnapshot& value, FileError error) {
    const bool cancelled = error.kind == FileErrorKind::Cancelled;
    value.phase = cancelled ? PermissionPhase::Cancelled : PermissionPhase::Error;
    if (cancelled) {
      value.error.reset();
      value.applied = value.itemsFailed = false;
    } else {
      value.error = std::move(error);
    }
    if (value.results) {
      auto rows = std::make_shared<std::vector<PermissionResult>>(*value.results);
      for (auto& row : *rows) {
        if (cancelled) {
          row.outcome = PermissionOutcome::Cancelled;
          row.error.reset();
          row.message = "Not changed; cancelled.";
        } else if (row.info) {
          row.info->current = false;
        }
      }
      value.results = std::move(rows);
    }
    if (!cancelled && value.info) {
      value.info->current = false;
    }
    value.finished = true;
  }
  void failRemote(TransferView& value, const FileError& error) {
    const bool cancelled = error.kind == FileErrorKind::Cancelled;
    for (auto& result : value.results) {
      if (!cancelled || result.outcome == TransferOutcome::Cancelled) {
        result.error = error;
        result.outcome = cancelled                 ? TransferOutcome::Cancelled
            : error.kind == FileErrorKind::Changed ? TransferOutcome::Partial
                                                   : TransferOutcome::Failed;
      }
      result.retryable = false;
    }
    value.report = cancelled && !value.report.empty() ? value.report + "\n" + error.message : error.message;
    value.retryable = 0;
    value.historyRetired = !cancelled;
  }
  void failRemote(RestoreView& value, const FileError& error) {
    for (auto& result : value.results) {
      result.error = error;
      result.outcome = error.kind == FileErrorKind::Changed ? RestoreOutcome::Unconfirmed
          : error.kind == FileErrorKind::Cancelled          ? RestoreOutcome::Cancelled
                                                            : RestoreOutcome::Failed;
    }
  }
  void failRemote(ArchiveView& value, FileError error) {
    value.phase = ArchivePhase::Idle;
    value.error = std::move(error);
  }
} // namespace luciole
