#include "app/operation_hub.h"

#include "fs/background_job.h"
#include "operations/copy_receipt.h"
#include "operations/create_receipt.h"
#include "operations/history_mount.h"
#include "operations/move_receipt.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>
#include <vector>

namespace {
  bool mutating(const OperationJob& job) {
    switch (job.kind()) {
    case JobKind::Rename:
      return job.service<luciole::RenameService>().snapshot().status == luciole::RenameStatus::Renaming;
    case JobKind::Create:
      return job.service<luciole::CreateService>().snapshot().status == luciole::CreateStatus::Creating;
    case JobKind::Permission:
      return job.service<luciole::PermissionService>().snapshot().phase == luciole::PermissionPhase::Saving;
    case JobKind::BatchRename:
      return job.service<luciole::BatchRenameService>().applying();
    case JobKind::Trash:
      return job.service<luciole::TrashService>().snapshot().status == luciole::TrashStatus::Trashing;
    case JobKind::EmptyTrash:
      return job.service<luciole::EmptyTrashService>().snapshot().status == luciole::EmptyTrashStatus::Emptying;
    case JobKind::Archive: {
      const auto phase = job.service<luciole::ArchiveService>().phase();
      return phase == luciole::ArchivePhase::Compressing
          || phase == luciole::ArchivePhase::Extracting
          || phase == luciole::ArchivePhase::Transferring;
    }
    case JobKind::Transfer:
    case JobKind::Restore:
      return job.running();
    }
    std::unreachable();
  }
} // namespace

OperationJob::OperationJob(luciole::JobId id, Service service) : m_id(id), m_service(std::move(service)) {}

OperationJob::~OperationJob() {
  std::visit([](auto& service) { service->setChangedCallback({}); }, m_service);
}

bool OperationJob::running() const {
  return std::visit([](const auto& service) { return service->busy(); }, m_service);
}

bool OperationJob::pending() const {
  switch (kind()) {
  case JobKind::Trash: {
    const auto& trash = service<luciole::TrashService>();
    return trash.busy() || trash.canTrash();
  }
  case JobKind::EmptyTrash:
    return service<luciole::EmptyTrashService>().pending();
  default:
    return running();
  }
}

bool OperationJob::cancellable() const {
  switch (kind()) {
  case JobKind::BatchRename:
    return service<luciole::BatchRenameService>().applying();
  case JobKind::Transfer:
  case JobKind::Archive:
  case JobKind::Trash:
  case JobKind::Restore:
  case JobKind::EmptyTrash:
    return running();
  case JobKind::Permission:
    return running() && service<luciole::PermissionService>().snapshot().phase == luciole::PermissionPhase::Saving;
  case JobKind::Create:
    return running() && service<luciole::CreateService>().snapshot().status == luciole::CreateStatus::Creating;
  default:
    return false;
  }
}

void OperationJob::cancel() {
  std::visit([](auto& service) { service->cancel(); }, m_service);
}

std::optional<luciole::TransferConflict> OperationJob::conflict() const {
  switch (kind()) {
  case JobKind::Transfer:
    return service<luciole::TransferService>().conflict();
  case JobKind::Archive:
    return service<luciole::ArchiveService>().conflict();
  case JobKind::Restore:
    return service<luciole::RestoreService>().conflict();
  default:
    return std::nullopt;
  }
}

bool OperationJob::resolve(std::uint64_t id, luciole::TransferConflictChoice choice, bool applyToAll) {
  switch (kind()) {
  case JobKind::Transfer:
    return service<luciole::TransferService>().resolve(id, choice, applyToAll);
  case JobKind::Archive:
    return service<luciole::ArchiveService>().resolve(id, choice);
  case JobKind::Restore:
    return service<luciole::RestoreService>().resolve(id, choice);
  default:
    return false;
  }
}

OperationHub::OperationHub(const luciole::TabWorkspace& workspace) : m_workspace(workspace) {}

OperationHub::~OperationHub() {
  m_lifetime.reset();
  m_owners.clear();
  for (auto& [id, job] : m_jobs) {
    (void)id;
    std::visit([](auto& service) { service->setChangedCallback({}); }, job->m_service);
    if (auto idle = std::exchange(job->m_idle, {})) {
      idle(*job);
    }
    if (auto finished = std::exchange(job->m_finished, {})) {
      finished(*job);
    }
  }
}

void OperationHub::attach(luciole::TabId page, Owner& owner) { m_owners.insert_or_assign(page, &owner); }

void OperationHub::detach(luciole::TabId page) { m_owners.erase(page); }

OperationJob* OperationHub::find(luciole::JobId id) const {
  const auto found = m_jobs.find(id);
  return found == m_jobs.end() ? nullptr : found->second.get();
}

std::shared_ptr<OperationJob> OperationHub::add(luciole::TabId page, OperationJob::Service service, JobScope scope) {
  const auto id = ++m_next;
  auto job = std::make_shared<OperationJob>(id, std::move(service));
  std::visit(
      [this, raw = job.get()](auto& value) { value->setChangedCallback([this, raw] { changed(*raw); }); },
      job->m_service
  );
  m_routing.add(id, page, m_workspace, scope == JobScope::Follow);
  m_jobs.emplace(id, job);
  return job;
}

void OperationHub::changed(OperationJob& job) {
  if (!job.m_mutationStarted && mutating(job)) {
    job.m_mutationStarted = true;
    job.m_activityStarted = true;
    job.m_historyGeneration = ++m_historyGeneration;
    if (job.historyReplay() == HistoryReplay::None) {
      m_renameReceipt.reset();
      m_batchRenameReceipt.reset();
      m_createReceipt.reset();
      m_moveReceipt.reset();
      m_copyReceipt.reset();
      m_historyRedo = false;
    }
  }
  bool finished = false;
  if (job.running()) {
    std::erase_if(m_finishedActivities, [&](const auto& item) { return item.id == job.id(); });
    job.m_running = true;
    job.m_ran = true;
    job.m_resultTaken = false;
  } else if (std::exchange(job.m_running, false)) {
    finished = true;
    if (job.m_mutationStarted
        && (job.kind() == JobKind::Rename
            || job.kind() == JobKind::BatchRename
            || job.kind() == JobKind::Create
            || job.kind() == JobKind::Transfer)) {
      recordHistory(job);
    }
    job.m_mutationStarted = false;
    if (auto activity = jobActivity(job, true); activity && activity->finished) {
      std::erase_if(m_finishedActivities, [&](const auto& item) { return item.id == job.id(); });
      m_finishedActivities.push_front(std::move(*activity));
      while (m_finishedActivities.size() > 12) {
        m_finishedActivities.pop_back();
      }
    }
    if (auto idle = std::exchange(job.m_idle, {})) {
      idle(job);
    }
  }
  notify(m_routing.owner(job.id()), job);
  notifyActivity();
  if (finished && job.kind() == JobKind::Transfer) {
    for (const auto& [page, owner] : m_owners) {
      (void)page;
      owner->transfersFinished(job.service<luciole::TransferService>().results());
    }
  }
  if (finished && job.kind() == JobKind::Restore) {
    const auto effects = job.service<luciole::RestoreService>().transferResults();
    for (const auto& [page, owner] : m_owners) {
      if (page != m_routing.owner(job.id())) {
        owner->transfersFinished(effects);
      }
    }
  }
}

void OperationHub::notify(luciole::TabId page, OperationJob& job) {
  if (const auto found = m_owners.find(page); found != m_owners.end()) {
    found->second->jobChanged(job);
  }
}

bool OperationHub::retained(const OperationJob& job) const {
  if (job.kind() != JobKind::Transfer || !job.ran()) {
    return false;
  }
  return newest(
             m_routing.owner(job.id()),
             [](const OperationJob& other) { return other.kind() == JobKind::Transfer && other.ran(); }
         )
      == &job;
}

void OperationHub::update(std::optional<luciole::WindowId> preferred) {
  for (auto& error : luciole::CopyReceipt::takeCleanupErrors()) {
    luciole::JobActivity activity;
    activity.id = ++m_next;
    activity.title = "Discard undo data";
    activity.summary = "Retained item needs inspection";
    activity.state = luciole::ActivityState::Failed;
    activity.finished = true;
    activity.report = std::make_shared<const std::string>(std::move(error.message));
    m_finishedActivities.push_front(std::move(activity));
    while (m_finishedActivities.size() > 12) {
      m_finishedActivities.pop_back();
    }
    notifyActivity();
  }
  for (const auto& move : m_routing.update(m_workspace, preferred)) {
    const auto found = m_jobs.find(move.job);
    if (found == m_jobs.end()) {
      continue;
    }
    const auto job = found->second;
    if (move.to) {
      notify(move.to, *job);
    } else if (!m_routing.following(move.job)) {
      job->cancel();
    }
  }
  std::vector<std::shared_ptr<OperationJob>> prepared;
  for (const auto& [id, job] : m_jobs) {
    (void)id;
    if (job->kind() == JobKind::Trash && job->service<luciole::TrashService>().canTrash()) {
      prepared.push_back(job);
    }
  }
  for (const auto& job : prepared) {
    (void)job->service<luciole::TrashService>().trash();
  }
  std::vector<std::shared_ptr<OperationJob>> finished;
  for (const auto& [id, job] : m_jobs) {
    (void)id;
    if (!job->pending() && job->m_finished) {
      finished.push_back(job);
    }
  }
  for (const auto& job : finished) {
    if (auto callback = std::exchange(job->m_finished, {})) {
      callback(*job);
    }
  }
  const auto removed = std::erase_if(m_jobs, [this](const auto& item) {
    const auto& job = *item.second;
    if (item.second.use_count() > 1 || job.pending() || retained(job)) {
      return false;
    }
    m_routing.remove(item.first);
    return true;
  });
  if (removed) {
    notifyActivity();
  }
}

void OperationHub::leaveDrive(const luciole::Location& root) {
  for (const auto& [page, owner] : m_owners) {
    (void)page;
    owner->leaveDrive(root);
  }
}

void OperationHub::prepareDriveRemoval(
    const luciole::Location& root, std::function<void(std::optional<luciole::FileError>)> done
) {
  // Never inspect mutable replay state or discard proofs while work is running or an editor is prepared.
  if (std::ranges::any_of(m_jobs, [](const auto& item) {
        const OperationJob& job = *item.second;
        if (job.pending()) {
          return true;
        }
        switch (job.kind()) {
        case JobKind::Create:
          return job.service<luciole::CreateService>().canCreate();
        case JobKind::Rename:
          return job.service<luciole::RenameService>().canRename();
        case JobKind::BatchRename:
          return job.service<luciole::BatchRenameService>().snapshot().phase == luciole::BatchRenamePhase::Ready;
        default:
          return false;
        }
      })) {
    done(std::nullopt);
    return;
  }
  ++m_historyProbes;
  luciole::runJob<std::array<bool, 5>>(
      [root, rename = m_renameReceipt, batch = m_batchRenameReceipt, create = m_createReceipt, move = m_moveReceipt,
       copy = m_copyReceipt] {
        const luciole::HistoryMount mount(root);
        return std::array{
            rename && rename->usesMount(mount), batch && luciole::batchRenameUsesMount(*batch, mount),
            create && create->usesMount(mount), move && move->usesMount(mount), copy && copy->usesMount(mount)
        };
      },
      [this, generation = m_historyGeneration, lifetime = std::weak_ptr(m_lifetime),
       done = std::move(done)](luciole::JobResult<std::array<bool, 5>> result) {
        if (lifetime.expired()) {
          return;
        }
        --m_historyProbes;
        if (!result) {
          done(result.error());
          return;
        }
        if (generation != m_historyGeneration) {
          done(
              luciole::FileError{
                  luciole::FileErrorKind::Changed, "File operations changed while preparing removal. Try again."
              }
          );
          return;
        }
        if (std::ranges::any_of(*result, [](bool matched) { return matched; })) {
          auto retired = std::tuple{
              (*result)[0] ? std::exchange(m_renameReceipt, {}) : nullptr,
              (*result)[1] ? std::exchange(m_batchRenameReceipt, {}) : nullptr,
              (*result)[2] ? std::exchange(m_createReceipt, {}) : nullptr,
              (*result)[3] ? std::exchange(m_moveReceipt, {}) : nullptr,
              (*result)[4] ? std::exchange(m_copyReceipt, {}) : nullptr
          };
          ++m_historyGeneration;
          m_historyRedo = false;
          ++m_historyRetirements;
          luciole::detail::runInThread(
              [retired = std::move(retired)] mutable { retired = {}; },
              [this, lifetime] {
                if (!lifetime.expired()) {
                  --m_historyRetirements;
                }
              }
          );
          notifyActivity();
        }
        done(std::nullopt);
      }
  );
}

bool OperationHub::removalReady(const luciole::Location& root) const {
  return m_historyRetirements == 0
      && !luciole::CopyReceipt::cleanupPending()
      && !luciole::CreateReceipt::cleanupPending()
      && !luciole::MoveReceipt::cleanupPending()
      && std::ranges::none_of(m_owners, [&](const auto& item) { return item.second->driveReadersBusy(root); });
}

void OperationHub::shutdown() {
  ++m_historyGeneration;
  m_copyReceipt.reset();
  m_moveReceipt.reset();
  for (auto& [id, job] : m_jobs) {
    (void)id;
    if (job->kind() == JobKind::Archive) {
      job->service<luciole::ArchiveService>().shutdown();
    } else {
      job->cancel();
    }
  }
}

bool OperationHub::busy() const {
  return m_historyProbes != 0
      || m_historyRetirements != 0
      || luciole::CopyReceipt::cleanupPending()
      || luciole::CreateReceipt::cleanupPending()
      || luciole::MoveReceipt::cleanupPending()
      || std::ranges::any_of(m_jobs, [](const auto& item) { return item.second->running(); });
}

bool OperationHub::historyReplayBusy() const {
  return m_historyProbes != 0 || std::ranges::any_of(m_jobs, [](const auto& item) {
           return item.second->historyReplay() != HistoryReplay::None && item.second->running();
         });
}

bool OperationHub::canUndo() const {
  return (m_renameReceipt || m_batchRenameReceipt || m_createReceipt || m_moveReceipt || m_copyReceipt)
      && !m_historyRedo
      && !busy();
}
bool OperationHub::canRedo() const {
  return (m_renameReceipt || m_batchRenameReceipt || m_createReceipt || m_moveReceipt || m_copyReceipt)
      && m_historyRedo
      && !busy();
}

std::string_view OperationHub::historyLabel(bool redo) const {
  if (m_copyReceipt) {
    if (m_copyReceipt->action() == luciole::TransferAction::Link) {
      return redo ? "Redo Create Links" : "Undo Create Links";
    }
    return redo ? "Redo Copy" : "Undo Copy";
  }
  if (m_moveReceipt) {
    return redo ? "Redo Move" : "Undo Move";
  }
  if (m_createReceipt) {
    switch (m_createKind) {
    case luciole::CreateKind::Folder:
      return redo ? "Redo Create Folder" : "Undo Create Folder";
    case luciole::CreateKind::EmptyFile:
      return redo ? "Redo Create File" : "Undo Create File";
    case luciole::CreateKind::Document:
      return redo ? "Redo Create Document" : "Undo Create Document";
    case luciole::CreateKind::Image:
      return redo ? "Redo Save Image" : "Undo Save Image";
    case luciole::CreateKind::SymbolicLink:
    case luciole::CreateKind::HardLink:
      return redo ? "Redo Create Link" : "Undo Create Link";
    }
  }
  if (m_batchRenameReceipt) {
    return redo ? "Redo Batch Rename" : "Undo Batch Rename";
  }
  if (m_renameReceipt) {
    return redo ? "Redo Rename" : "Undo Rename";
  }
  return redo ? "Redo" : "Undo";
}

bool OperationHub::replayHistory(luciole::TabId page, bool redo) {
  if (!attached(page) || !(redo ? canRedo() : canUndo())) {
    return false;
  }
  if (m_copyReceipt) {
    auto job = create<luciole::TransferService>(page);
    job->m_historyReplay = redo ? HistoryReplay::Redo : HistoryReplay::Undo;
    return job->service<luciole::TransferService>().replayCopy(m_copyReceipt);
  }
  if (m_moveReceipt) {
    auto job = create<luciole::TransferService>(page);
    job->m_historyReplay = redo ? HistoryReplay::Redo : HistoryReplay::Undo;
    return job->service<luciole::TransferService>().replay(m_moveReceipt);
  }
  if (m_createReceipt) {
    auto job = create<luciole::CreateService>(page);
    job->m_historyReplay = redo ? HistoryReplay::Redo : HistoryReplay::Undo;
    return job->service<luciole::CreateService>().replay(m_createReceipt);
  }
  if (m_batchRenameReceipt) {
    auto job = create<luciole::BatchRenameService>(page);
    job->m_historyReplay = redo ? HistoryReplay::Redo : HistoryReplay::Undo;
    return job->service<luciole::BatchRenameService>().replay(m_batchRenameReceipt);
  }
  auto job = create<luciole::RenameService>(page);
  job->m_historyReplay = redo ? HistoryReplay::Redo : HistoryReplay::Undo;
  return job->service<luciole::RenameService>().replay(m_renameReceipt);
}

void OperationHub::recordHistory(OperationJob& job) {
  if (job.kind() == JobKind::Transfer) {
    auto& service = job.service<luciole::TransferService>();
    auto receipt = service.takeReceipt();
    auto copyReceipt = service.takeCopyReceipt();
    if (job.m_historyGeneration != m_historyGeneration) {
      return;
    }
    if (job.historyReplay() == HistoryReplay::None) {
      copyReceipt = luciole::CopyReceipt::withoutPredecessors(std::move(copyReceipt));
      receipt = luciole::MoveReceipt::withoutPredecessors(std::move(receipt));
    }
    if (copyReceipt) {
      m_copyReceipt = std::move(copyReceipt);
      m_moveReceipt.reset();
      m_renameReceipt.reset();
      m_batchRenameReceipt.reset();
      m_createReceipt.reset();
      m_historyRedo = job.historyReplay() == HistoryReplay::Undo;
    } else if (receipt) {
      m_copyReceipt.reset();
      m_moveReceipt = std::move(receipt);
      m_renameReceipt.reset();
      m_batchRenameReceipt.reset();
      m_createReceipt.reset();
      m_historyRedo = job.historyReplay() == HistoryReplay::Undo;
    } else if (service.historyRetired()) {
      m_moveReceipt.reset();
      m_copyReceipt.reset();
    }
    return;
  }
  if (job.kind() == JobKind::Create) {
    auto receipt = job.service<luciole::CreateService>().takeReceipt();
    if (job.m_historyGeneration != m_historyGeneration) {
      return;
    }
    const auto& snapshot = job.service<luciole::CreateService>().snapshot();
    if (snapshot.status == luciole::CreateStatus::Succeeded || snapshot.status == luciole::CreateStatus::Partial) {
      m_createReceipt = snapshot.status == luciole::CreateStatus::Succeeded ? std::move(receipt) : nullptr;
      m_createKind = snapshot.kind;
      m_renameReceipt.reset();
      m_batchRenameReceipt.reset();
      m_historyRedo = job.historyReplay() == HistoryReplay::Undo;
    } else if (
        snapshot.retireHistory
        || (snapshot.error
            && (snapshot.error->kind == luciole::FileErrorKind::Changed
                || snapshot.error->kind == luciole::FileErrorKind::Missing
                || snapshot.error->kind == luciole::FileErrorKind::NotDirectory))
    ) {
      m_createReceipt.reset();
    }
    return;
  }
  if (job.kind() == JobKind::BatchRename) {
    auto receipt = job.service<luciole::BatchRenameService>().takeReceipt();
    if (job.m_historyGeneration != m_historyGeneration) {
      return;
    }
    const auto& snapshot = job.service<luciole::BatchRenameService>().snapshot();
    if (snapshot.phase == luciole::BatchRenamePhase::Finished) {
      m_batchRenameReceipt = std::move(receipt);
      m_renameReceipt.reset();
      m_historyRedo = job.historyReplay() == HistoryReplay::Undo;
    } else if (
        snapshot.error
        && (snapshot.error->kind == luciole::FileErrorKind::Changed
            || snapshot.error->kind == luciole::FileErrorKind::Missing
            || snapshot.error->kind == luciole::FileErrorKind::NotDirectory)
    ) {
      m_batchRenameReceipt.reset();
    }
    return;
  }
  auto receipt = job.service<luciole::RenameService>().takeReceipt();
  if (job.m_historyGeneration != m_historyGeneration) {
    return;
  }
  const auto& snapshot = job.service<luciole::RenameService>().snapshot();
  if (snapshot.status == luciole::RenameStatus::Succeeded) {
    m_renameReceipt = std::move(receipt);
    m_historyRedo = job.historyReplay() == HistoryReplay::Undo;
  } else if (
      snapshot.error
      && (snapshot.error->kind == luciole::FileErrorKind::Changed
          || snapshot.error->kind == luciole::FileErrorKind::Missing
          || snapshot.error->kind == luciole::FileErrorKind::NotDirectory)
  ) {
    m_renameReceipt.reset();
  }
}

std::vector<luciole::JobActivity> OperationHub::activities() const {
  std::vector<luciole::JobActivity> result;
  result.reserve(m_jobs.size() + m_finishedActivities.size());
  for (const auto& [id, job] : std::views::reverse(m_jobs)) {
    (void)id;
    if (auto activity = jobActivity(*job); activity && !activity->finished) {
      result.push_back(std::move(*activity));
    }
  }
  for (const auto& item : m_finishedActivities) {
    auto activity = item;
    const auto* job = find(item.id);
    activity.canRetry = job
        && job->kind() == JobKind::Transfer
        && !job->running()
        && !historyReplayBusy()
        && !newest(owner(*job), [](const OperationJob& pending) { return pending.pending(); })
        && job->service<luciole::TransferService>().retryable() > 0;
    result.push_back(std::move(activity));
  }
  return result;
}

std::shared_ptr<const std::string> OperationHub::activityReport(luciole::JobId id) const {
  const auto found = std::ranges::find(m_finishedActivities, id, &luciole::JobActivity::id);
  if (found != m_finishedActivities.end()) {
    return found->report;
  }
  const auto* job = find(id);
  const auto activity = job ? jobActivity(*job, true) : std::nullopt;
  return activity ? activity->report : nullptr;
}

bool OperationHub::cancelActivity(luciole::JobId id) {
  auto* job = find(id);
  if (!job || !job->cancellable()) {
    return false;
  }
  job->cancel();
  return true;
}

void OperationHub::clearFinishedActivities() {
  m_finishedActivities.clear();
  notifyActivity();
}

void OperationHub::notifyActivity() {
  const std::vector<std::pair<luciole::TabId, Owner*>> owners(m_owners.begin(), m_owners.end());
  for (const auto& [page, owner] : owners) {
    const auto current = m_owners.find(page);
    if (current != m_owners.end() && current->second == owner) {
      owner->activityChanged();
    }
  }
}
