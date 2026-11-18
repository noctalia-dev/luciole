#pragma once

#include "app/job_activity.h"
#include "browser/job_routing.h"
#include "operations/archive_service.h"
#include "operations/batch_rename_service.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/permission_service.h"
#include "operations/rename_service.h"
#include "operations/restore_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <variant>

// In variant order of OperationJob::Service.
enum class JobKind : std::uint8_t {
  Transfer,
  Archive,
  Trash,
  Restore,
  EmptyTrash,
  Rename,
  BatchRename,
  Permission,
  Create
};

// Follow: the job moves to another page when its page leaves the window. Page: the job only feeds its page, as a
// dialog's preparation does; it is cancelled when the page leaves, unless OperationHub::follow() released it first.
enum class JobScope : std::uint8_t { Follow, Page };
enum class HistoryReplay : std::uint8_t { None, Undo, Redo };

// One file operation with a service instance of its own. A page starts it; the page that owns it shows it.
class OperationJob : public std::enable_shared_from_this<OperationJob> {
public:
  using Service = std::variant<
      std::unique_ptr<luciole::TransferService>, std::unique_ptr<luciole::ArchiveService>,
      std::unique_ptr<luciole::TrashService>, std::unique_ptr<luciole::RestoreService>,
      std::unique_ptr<luciole::EmptyTrashService>, std::unique_ptr<luciole::RenameService>,
      std::unique_ptr<luciole::BatchRenameService>, std::unique_ptr<luciole::PermissionService>,
      std::unique_ptr<luciole::CreateService>>;

  OperationJob(luciole::JobId id, Service service);
  ~OperationJob();
  OperationJob(const OperationJob&) = delete;
  OperationJob& operator=(const OperationJob&) = delete;

  luciole::JobId id() const { return m_id; }
  JobKind kind() const { return static_cast<JobKind>(m_service.index()); }
  template <typename T> T& service() { return *std::get<std::unique_ptr<T>>(m_service); }
  template <typename T> const T& service() const { return *std::get<std::unique_ptr<T>>(m_service); }

  // The service runs work.
  bool running() const;
  // True once the service has run.
  bool ran() const { return m_ran; }
  HistoryReplay historyReplay() const { return m_historyReplay; }
  bool activityStarted() const { return m_activityStarted; }
  // Running, or prepared and waiting to start; a page disables actions that start a job meanwhile.
  bool pending() const;
  // The operation can currently be stopped through the activity controls.
  bool cancellable() const;
  void cancel();
  // The restore, archive or transfer conflict waiting for an answer.
  std::optional<luciole::TransferConflict> conflict() const;
  bool resolve(std::uint64_t id, luciole::TransferConflictChoice choice, bool applyToAll = false);
  // True the first time it is called after a run ends: the owner reports each outcome once.
  bool takeResult() { return !std::exchange(m_resultTaken, true); }
  // Runs `callback` once the current run ends, or when the hub goes away first.
  void whenIdle(std::function<void(OperationJob&)> callback) { m_idle = std::move(callback); }
  // Runs after preparation and mutation finish, or on hub teardown to release external drop offers.
  void whenFinished(std::function<void(OperationJob&)> callback) { m_finished = std::move(callback); }

private:
  friend class OperationHub;
  luciole::JobId m_id;
  Service m_service;
  bool m_running = false, m_ran = false, m_resultTaken = false;
  bool m_mutationStarted = false;
  bool m_activityStarted = false;
  std::uint64_t m_historyGeneration = 0;
  HistoryReplay m_historyReplay = HistoryReplay::None;
  std::function<void(OperationJob&)> m_idle;
  std::function<void(OperationJob&)> m_finished;
};

// The application's file jobs. Each job keeps running when its page closes; JobRouting picks the page that shows
// it. The hub drops a job once it is idle and nobody holds it, keeping each page's newest transfer for retry and
// its report.
class OperationHub {
public:
  // A page that shows the jobs it owns. BrowserPage implements it.
  class Owner {
  public:
    // A job this page owns changed, or the page just took it over.
    virtual void jobChanged(OperationJob& job) = 0;
    // All pages observe completed transfers, including providers without directory monitors.
    virtual void transfersFinished(const std::vector<luciole::TransferResult>& results) = 0;
    virtual void activityChanged() {}
    virtual void leaveDrive(const luciole::Location&) {}
    virtual bool driveReadersBusy(const luciole::Location&) const { return false; }

  protected:
    Owner() = default;
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    ~Owner() = default;
  };

  explicit OperationHub(const luciole::TabWorkspace& workspace);
  ~OperationHub();
  OperationHub(const OperationHub&) = delete;
  OperationHub& operator=(const OperationHub&) = delete;

  void attach(luciole::TabId page, Owner& owner);
  void detach(luciole::TabId page);

  // A new idle job owned by `page`; start its service right away. The hub drops the job once it is idle and only
  // the hub holds it.
  template <typename Service>
  std::shared_ptr<OperationJob> create(luciole::TabId page, JobScope scope = JobScope::Follow) {
    return add(page, std::make_unique<Service>(), scope);
  }
  // Lets a page-scoped job move with its window from now on, as a job that applies changes does.
  void follow(const OperationJob& job) { m_routing.follow(job.id()); }
  luciole::TabId owner(const OperationJob& job) const { return m_routing.owner(job.id()); }
  // True while `page` belongs to a window; a page that left keeps no job.
  bool attached(luciole::TabId page) const { return m_workspace.owner(page).has_value(); }
  // The newest or oldest job `page` owns that `accept` takes, or null.
  template <typename Predicate> OperationJob* newest(luciole::TabId page, Predicate accept) const {
    for (const auto& [id, job] : std::views::reverse(m_jobs)) {
      if (m_routing.owner(id) == page && accept(*job)) {
        return job.get();
      }
    }
    return nullptr;
  }
  template <typename Predicate> OperationJob* oldest(luciole::TabId page, Predicate accept) const {
    for (const auto& [id, job] : m_jobs) {
      if (m_routing.owner(id) == page && accept(*job)) {
        return job.get();
      }
    }
    return nullptr;
  }
  OperationJob* find(luciole::JobId id) const;

  // Main loop: hands the jobs of pages that left their window to another page and cancels their page-scoped jobs,
  // starts prepared moves to Trash, then drops finished jobs.
  void update(std::optional<luciole::WindowId> preferred);
  // Cancels every job, including prepared moves to Trash; the application exits once they stop.
  void shutdown();
  // Ask every pane to leave a drive before explicit removal.
  void leaveDrive(const luciole::Location& root);
  // Inspect proofs on a worker and retire only history holding this mount. Retained-data cleanup can follow.
  void prepareDriveRemoval(const luciole::Location& root, std::function<void(std::optional<luciole::FileError>)> done);
  bool removalReady(const luciole::Location& root) const;
  // True while any job runs.
  bool busy() const;
  bool historyReplayBusy() const;
  bool canUndo() const;
  bool canRedo() const;
  bool batchRenameHistory() const { return m_batchRenameReceipt != nullptr; }
  std::string_view historyLabel(bool redo) const;
  bool replayHistory(luciole::TabId page, bool redo);
  std::vector<luciole::JobActivity> activities() const;
  std::shared_ptr<const std::string> activityReport(luciole::JobId id) const;
  bool cancelActivity(luciole::JobId id);
  void clearFinishedActivities();

private:
  std::shared_ptr<OperationJob> add(luciole::TabId page, OperationJob::Service service, JobScope scope);
  void changed(OperationJob& job);
  void notify(luciole::TabId page, OperationJob& job);
  bool retained(const OperationJob& job) const;
  void recordHistory(OperationJob& job);
  void notifyActivity();

  const luciole::TabWorkspace& m_workspace;
  luciole::JobRouting m_routing;
  std::map<luciole::JobId, std::shared_ptr<OperationJob>> m_jobs;
  std::map<luciole::TabId, Owner*> m_owners;
  luciole::JobId m_next = 0;
  std::shared_ptr<const luciole::RenameReceipt> m_renameReceipt;
  std::shared_ptr<const luciole::BatchRenameReceipt> m_batchRenameReceipt;
  std::shared_ptr<const luciole::CreateReceipt> m_createReceipt;
  std::shared_ptr<const luciole::MoveReceipt> m_moveReceipt;
  std::shared_ptr<const luciole::CopyReceipt> m_copyReceipt;
  luciole::CreateKind m_createKind = luciole::CreateKind::Folder;
  std::uint64_t m_historyGeneration = 0;
  bool m_historyRedo = false;
  std::deque<luciole::JobActivity> m_finishedActivities;
  std::size_t m_historyProbes = 0;
  std::size_t m_historyRetirements = 0;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};

// One page's view of the hub: the jobs it starts and the ones it owns now.
class PageJobs {
public:
  PageJobs(OperationHub& hub, luciole::TabId page) : m_hub(hub), m_page(page) {}

  template <typename Service> std::shared_ptr<OperationJob> create(JobScope scope = JobScope::Follow) {
    return m_hub.create<Service>(m_page, scope);
  }
  void follow(const OperationJob& job) { m_hub.follow(job); }
  void leaveDrive(const luciole::Location& root) { m_hub.leaveDrive(root); }
  void prepareDriveRemoval(const luciole::Location& root, std::function<void(std::optional<luciole::FileError>)> done) {
    m_hub.prepareDriveRemoval(root, std::move(done));
  }
  bool removalReady(const luciole::Location& root) const { return m_hub.removalReady(root); }
  bool owns(const OperationJob& job) const { return m_hub.owner(job) == m_page; }
  // False once the page left its window (closed tab or pane, closed window): its dialogs close for good.
  bool attached() const { return m_hub.attached(m_page); }
  std::string_view historyLabel(bool redo) const { return m_hub.historyLabel(redo); }
  // An owned job by id, or null.
  OperationJob* find(luciole::JobId id) const {
    auto* job = m_hub.find(id);
    return job && owns(*job) ? job : nullptr;
  }
  template <typename Predicate> OperationJob* newest(Predicate accept) const {
    return m_hub.newest(m_page, std::move(accept));
  }
  template <typename Predicate> OperationJob* oldest(Predicate accept) const {
    return m_hub.oldest(m_page, std::move(accept));
  }
  // True while an owned job runs or waits to start.
  bool busy() const {
    return m_hub.historyReplayBusy() || newest([](const OperationJob& job) { return job.pending(); }) != nullptr;
  }

private:
  OperationHub& m_hub;
  luciole::TabId m_page;
};
