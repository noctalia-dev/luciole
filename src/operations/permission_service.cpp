#include "operations/permission_service.h"

#include "fs/background_job.h"
#include "fs/cancel_token.h"
#include "fs/system_mounts.h"
#include "operations/permission_jobs.h"
#include "operations/permission_target.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <set>

namespace {
  using namespace luciole;
  using Reply = PermissionReply;

  void summarize(PermissionSnapshot& snapshot, const std::vector<PermissionResult>& rows) {
    snapshot.info.reset();
    snapshot.mixedMode = 0;
    snapshot.mixedOwner = snapshot.mixedGroup = false;
    snapshot.unknownItems = 0;
    for (const auto& row : rows) {
      if (!row.info) {
        ++snapshot.unknownItems;
        continue;
      }
      const auto& info = *row.info;
      if (!snapshot.info) {
        snapshot.info = info;
        snapshot.info->acl.reset(); // Each item retains its own ACL; there is no common table.
        snapshot.info->reason.clear();
        continue;
      }
      auto& summary = *snapshot.info;
      snapshot.mixedMode |= summary.mode ^ info.mode;
      snapshot.mixedOwner = snapshot.mixedOwner || summary.owner != info.owner;
      snapshot.mixedGroup = snapshot.mixedGroup || summary.group != info.group;
      summary.canMode = summary.canMode || info.canMode;
      summary.canOwner = summary.canOwner || info.canOwner;
      summary.canGroup = summary.canGroup || info.canGroup;
      summary.multipleLinks = summary.multipleLinks || info.multipleLinks;
      summary.symlink = summary.symlink || info.symlink;
    }
    if (snapshot.info && snapshot.info->symlink) {
      snapshot.info->reason = "Symbolic links do not support mode changes. Ownership applies to the links themselves.";
    }
    const auto unavailable = std::ranges::count_if(rows, [](const auto& row) {
      return !row.info || (!row.info->canMode && !row.info->canOwner && !row.info->canGroup);
    });
    if (snapshot.info && unavailable > 0) {
      snapshot.info->reason += std::format(" {} items cannot be edited; see Results for details.", unavailable);
    }
  }
  // Holds the automounts of the prepared targets for the job's duration.
  std::vector<Fd> openBatchMounts(const PermissionBatch& batch, const CancelToken& token) {
    std::vector<Location> locations;
    for (std::size_t i = 0; i < batch.targets.size(); ++i) {
      if (batch.targets[i]) {
        locations.push_back((*batch.results)[i].location);
      }
    }
    return openSystemMounts(locations, token, false);
  }
} // namespace

struct luciole::PermissionService::State {
  PermissionSnapshot snapshot;
  std::shared_ptr<PermissionTarget> target;
  std::shared_ptr<PermissionBatch> bulk;
  std::atomic<std::size_t> completed{0};
  std::atomic<std::size_t> total{0};
  std::atomic<bool> collecting{false};
  CancelToken token; // The current job's; each launch takes a fresh one.
  bool active = false;
  std::function<void()> changed;

  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void clearOutcome() {
    snapshot.message.clear();
    snapshot.error.reset();
    snapshot.itemsFailed = snapshot.applied = false;
  }
  static void
  launch(const std::shared_ptr<State>& owner, std::move_only_function<Reply(const CancelToken&)> work, bool saving) {
    owner->active = true;
    owner->token = CancelToken{};
    owner->snapshot.cancelRequested = false;
    const auto token = owner->token;
    // Work always runs, even when cancelled before it starts, so a save reports its state.
    runJob<Reply>(
        [work = std::move(work), token] mutable { return work(token); },
        [owner, token, saving](JobResult<Reply> result) {
          auto reply = result ? std::move(*result) : Reply{.error = std::move(result.error())};
          owner->active = false;
          owner->collecting.store(false);
          auto& snapshot = owner->snapshot;
          snapshot.info = reply.info;
          snapshot.message = std::move(reply.message);
          snapshot.error = std::move(reply.error);
          snapshot.itemsFailed = reply.itemsFailed;
          snapshot.applied = reply.applied;
          owner->target = reply.target;
          owner->bulk = reply.bulk;
          snapshot.results = reply.results;
          snapshot.finished = reply.finished;
          snapshot.report = std::move(reply.report);
          if (reply.bulk) {
            summarize(snapshot, *reply.results);
          }
          snapshot.phase = token.cancelled() && (!saving || reply.bulk || snapshot.aclOperation)
              ? PermissionPhase::Cancelled
              : (reply.info || reply.bulk) ? PermissionPhase::Ready
                                           : PermissionPhase::Error;
          if (token.cancelled()) {
            owner->target.reset();
            owner->bulk.reset();
          }
          owner->notify();
        }
    );
    owner->notify();
  }
};

luciole::PermissionService::PermissionService() : m_state(std::make_shared<State>()) {}
luciole::PermissionService::~PermissionService() {
  m_state->changed = {};
  cancel();
}
bool luciole::PermissionService::busy() const { return m_state->active; }
std::size_t luciole::PermissionService::completed() const { return m_state->completed.load(); }
bool luciole::PermissionService::collecting() const { return m_state->collecting.load(); }
std::size_t luciole::PermissionService::total() const { return m_state->total.load(); }
const luciole::PermissionSnapshot& luciole::PermissionService::snapshot() const { return m_state->snapshot; }
void luciole::PermissionService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void luciole::PermissionService::cancel() {
  m_state->token.cancel();
  if (!busy()) {
    m_state->target.reset();
    m_state->bulk.reset();
  } else {
    m_state->snapshot.cancelRequested = true;
    m_state->notify();
  }
}
bool luciole::PermissionService::prepare(DirectoryEntry entry) { return prepareEntry(std::move(entry), false); }
bool luciole::PermissionService::prepare(const Location& location) {
  return prepareEntry({.place = location, .name = {}, .size = {}, .modifiedSeconds = {}}, true);
}
bool luciole::PermissionService::prepare(std::vector<DirectoryEntry> entries) {
  if (busy() || entries.size() < 2 || entries.size() > 4096) {
    return false;
  }
  m_state->target.reset();
  m_state->bulk.reset();
  m_state->completed.store(0);
  m_state->snapshot = {};
  m_state->snapshot.phase = PermissionPhase::Loading;
  m_state->snapshot.multiple = true;
  State::launch(
      m_state,
      [owner = m_state, entries = std::move(entries)](const CancelToken& token) {
        auto bulk = std::make_shared<PermissionBatch>();
        bulk->results = std::make_shared<std::vector<PermissionResult>>();
        std::set<std::string> uris;
        for (const auto& entry : entries) {
          const auto location = entry.location();
          if (!uris.insert(location.uri()).second) {
            throwFileError(FileErrorKind::InvalidArgument, "The selection contains repeated locations.");
          }
          bulk->results->push_back(
              {.location = location,
               .name = entry.displayName(),
               .info = {},
               .outcome = PermissionOutcome::Cancelled,
               .error = {},
               .message = "Not inspected; cancelled."}
          );
          bulk->targets.push_back({});
        }
        for (std::size_t i = 0; i < entries.size() && !token.cancelled(); ++i) {
          auto& row = (*bulk->results)[i];
          try {
            const auto mount = openSystemMount(entries[i].location(), token, false);
            auto target = preparePermissionTarget(entries[i], false);
            target->loadAcl(true);
            row.info = target->inspect();
            row.outcome = PermissionOutcome::Ready;
            row.message = row.info->reason;
            bulk->targets[i] = std::move(target);
          } catch (const std::exception&) {
            if (token.cancelled()) {
              break;
            }
            row.outcome = PermissionOutcome::Failed;
            row.error = currentFileError();
            row.message.clear();
          }
          owner->completed.fetch_add(1);
        }
        return Reply{.bulk = bulk, .results = bulk->results, .report = permissionReport(*bulk->results)};
      },
      false
  );
  return true;
}
bool luciole::PermissionService::prepareEntry(DirectoryEntry entry, bool captureCurrentLocation) {
  if (busy()) {
    return false;
  }
  m_state->target.reset();
  m_state->bulk.reset();
  m_state->completed.store(0);
  m_state->snapshot = {};
  m_state->snapshot.phase = PermissionPhase::Loading;
  State::launch(
      m_state,
      [entry = std::move(entry), captureCurrentLocation](const CancelToken& token) {
        const auto mount = openSystemMount(entry.location(), token, false);
        auto target = preparePermissionTarget(entry, captureCurrentLocation);
        if (token.cancelled()) {
          return Reply{};
        }
        target->loadAcl(true);
        if (target->identified) {
          target->validate();
        }
        auto info = target->inspect();
        return Reply{.target = std::move(target), .info = std::move(info)};
      },
      false
  );
  return true;
}
bool luciole::PermissionService::apply(PermissionEdit edit) {
  if (busy()
      || (!m_state->target && !m_state->bulk)
      || m_state->snapshot.phase != PermissionPhase::Ready
      || (m_state->snapshot.recursive && m_state->snapshot.finished)) {
    return false;
  }
  const auto& info = m_state->snapshot.info;
  if (!info || (!info->canMode && !info->canOwner && !info->canGroup)) {
    return false;
  }
  m_state->snapshot.recursive = false;
  m_state->snapshot.aclOperation = false;
  m_state->snapshot.phase = PermissionPhase::Saving;
  m_state->clearOutcome();
  if (m_state->bulk) {
    m_state->snapshot.finished = false;
    m_state->completed.store(0);
    State::launch(
        m_state,
        [owner = m_state, bulk = m_state->bulk, edit = std::move(edit)](const CancelToken& token) {
          const auto mounts = openBatchMounts(*bulk, token);
          return applyBulkPermissionEdit(bulk, edit, token, owner->completed);
        },
        true
    );
    return true;
  }
  State::launch(
      m_state,
      [target = m_state->target, edit = std::move(edit)](const CancelToken& token) {
        const auto mount = openSystemMount(target->entry.location(), token, false);
        return applyPermissionEdit(target, edit, token);
      },
      true
  );
  return true;
}
bool luciole::PermissionService::applyRecursive(RecursivePermissionEdit edit) {
  if (busy()
      || m_state->snapshot.phase != PermissionPhase::Ready
      || (m_state->snapshot.recursive && m_state->snapshot.finished)
      || (!m_state->target && !m_state->bulk)) {
    return false;
  }
  auto roots = m_state->bulk;
  if (!roots) {
    roots = std::make_shared<PermissionBatch>();
    roots->targets.push_back(m_state->target);
    roots->results = std::make_shared<std::vector<PermissionResult>>();
    roots->results->push_back(
        {.location = m_state->target->entry.location(),
         .name = m_state->target->entry.displayName(),
         .info = m_state->snapshot.info,
         .outcome = PermissionOutcome::Ready,
         .error = {},
         .message = {}}
    );
  }
  m_state->snapshot.phase = PermissionPhase::Saving;
  m_state->snapshot.recursive = m_state->snapshot.multiple = true;
  m_state->snapshot.aclOperation = false;
  m_state->snapshot.finished = false;
  m_state->clearOutcome();
  m_state->completed.store(0);
  m_state->total.store(0);
  m_state->collecting.store(true);
  State::launch(
      m_state,
      [owner = m_state, roots, edit = std::move(edit)](const CancelToken& token) {
        const auto mounts = openBatchMounts(*roots, token);
        return applyRecursivePermissionEdit(roots, edit, token, owner->completed, owner->total, owner->collecting);
      },
      true
  );
  return true;
}
bool luciole::PermissionService::applyAcl(AclEdit edit) {
  if (busy() || m_state->snapshot.phase != PermissionPhase::Ready || !m_state->target || m_state->bulk) {
    return false;
  }
  m_state->snapshot.phase = PermissionPhase::Saving;
  m_state->snapshot.aclOperation = true;
  m_state->clearOutcome();
  State::launch(
      m_state,
      [target = m_state->target, edit = std::move(edit)](const CancelToken& token) {
        const auto mount = openSystemMount(target->entry.location(), token, false);
        return applyAclEdit(target, edit, token);
      },
      true
  );
  return true;
}
bool luciole::PermissionService::applyAclPatch(AclPatchEdit edit) {
  if (busy()
      || m_state->snapshot.phase != PermissionPhase::Ready
      || !m_state->bulk
      || (m_state->snapshot.recursive && m_state->snapshot.finished)) {
    return false;
  }
  m_state->snapshot.phase = PermissionPhase::Saving;
  m_state->snapshot.aclOperation = true;
  m_state->snapshot.finished = false;
  m_state->clearOutcome();
  m_state->completed.store(0);
  State::launch(
      m_state,
      [owner = m_state, bulk = m_state->bulk, edit = std::move(edit)](const CancelToken& token) {
        const auto mounts = openBatchMounts(*bulk, token);
        return applyBulkAclPatch(bulk, edit, token, owner->completed);
      },
      true
  );
  return true;
}
