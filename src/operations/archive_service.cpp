#include "operations/archive_service.h"

#include "fs/background_job.h"
#include "fs/system_mounts.h"
#include "operations/archive_icons.h"
#include "operations/archive_work.h"

#include <filesystem>
#include <glib.h>
#include <utility>

struct luciole::ArchiveService::State : std::enable_shared_from_this<State> {
  enum class Operation { Inspect, Compress, Extract, Open };
  struct Request {
    Operation operation;
    std::vector<Location> sources;
    std::optional<Location> destination;
    ArchiveFormat format;
    std::vector<std::string> selected;
    std::shared_ptr<const ArchiveCatalog> expected;
    std::shared_ptr<archive_work::Passwords> passwords;
  };
  std::optional<Request> request;
  std::optional<ArchivePasswordRequest> password;
  std::uint64_t nextPassword = 0;
  ArchiveLimits limits;
  archive_work::Progress progress;
  ArchivePhase phase = ArchivePhase::Idle;
  std::function<void()> changed;
  CancelToken cancel;
  std::shared_ptr<const ArchiveCatalog> catalog;
  std::optional<Location> opened;
  std::vector<std::string> openStages;
  std::uint64_t openBytes = 0;
  std::string stage;
  std::optional<FileError> error;
  bool closing = false;
  GSource* timer = nullptr;
  TransferService transfers;
  std::shared_ptr<State> transferLifetime;
  std::vector<TransferResult> results;
  explicit State(ArchiveLimits value) : limits(value) {}
  ~State() {
    if (timer) {
      g_source_destroy(timer);
      g_source_unref(timer);
    }
  }
  static FileError previewFull() {
    return {FileErrorKind::NoSpace, "Temporary archive preview storage is full. Extract the member instead."};
  }
  void notify() {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void stopTimer() {
    if (timer) {
      g_source_destroy(timer);
      g_source_unref(std::exchange(timer, nullptr));
    }
  }
  void startTimer() {
    stopTimer();
    timer = g_timeout_source_new(100);
    g_source_set_callback(
        timer,
        [](gpointer data) -> gboolean {
          static_cast<State*>(data)->notify();
          return G_SOURCE_CONTINUE;
        },
        this, nullptr
    );
    g_source_attach(timer, g_main_context_default());
  }
  void clean(std::vector<std::string> paths) {
    stopTimer();
    if (closing) {
      for (auto& path : openStages) {
        paths.push_back(std::move(path));
      }
      openStages.clear();
      openBytes = 0;
    }
    if (paths.empty()) {
      phase = ArchivePhase::Idle;
      notify();
      return;
    }
    phase = ArchivePhase::Cleaning;
    runJob<std::optional<FileError>>(
        [paths = std::move(paths)] { return archive_work::cleanup(paths); },
        [owner = shared_from_this()](JobResult<std::optional<FileError>> result) {
          if (!result) {
            owner->error = std::move(result.error());
          } else if (*result) {
            owner->error = std::move(**result);
          }
          if (owner->closing && !owner->openStages.empty()) {
            owner->clean({});
          } else {
            owner->phase = ArchivePhase::Idle;
            owner->notify();
          }
        }
    );
    notify();
  }
  void transferChanged() {
    if (transfers.busy()) {
      notify();
      return;
    }
    results = transfers.results();
    for (const auto& result : results) {
      if (result.error) {
        error = result.error;
        break;
      }
      if (result.outcome == TransferOutcome::Cancelled) {
        error = FileError{FileErrorKind::Cancelled, "Archive operation cancelled."};
      } else if (result.outcome == TransferOutcome::Skipped) {
        error = FileError{FileErrorKind::Exists, "Archive operation skipped."};
      }
    }
    std::vector<std::string> paths;
    if (!stage.empty()) {
      paths.push_back(std::exchange(stage, {}));
    }
    clean(std::move(paths));
    transferLifetime.reset();
  }
  // Publishes a successful job's output on the main thread.
  void finish(Operation operation, const std::optional<Location>& destination, archive_work::Prepared prepared) {
    if (operation == Operation::Inspect) {
      catalog = std::move(prepared.catalog);
      phase = ArchivePhase::Idle;
      notify();
      return;
    }
    if (operation == Operation::Open) {
      if (prepared.expandedBytes > 512U * 1024U * 1024U - openBytes) {
        error = previewFull();
        clean({prepared.stage});
        return;
      }
      openStages.push_back(prepared.stage);
      openBytes += prepared.expandedBytes;
      opened = prepared.output;
      phase = ArchivePhase::Idle;
      notify();
      return;
    }
    stage = std::move(prepared.stage);
    const std::filesystem::path target(*destination->nativePath());
    phase = ArchivePhase::Transferring;
    transferLifetime = shared_from_this();
    if (!transfers.start(
            {{*prepared.output}, Location::fromPath(target.parent_path().native()), TransferAction::Move, false}
        )) {
      error = FileError{FileErrorKind::Other, "Could not start the archive publication."};
      clean({std::exchange(stage, {})});
      transferLifetime.reset();
    }
    notify();
  }
  bool start(
      Operation operation, std::vector<Location> sources, const std::optional<Location>& destination,
      ArchiveFormat format, std::vector<std::string> selected, std::shared_ptr<const ArchiveCatalog> known = nullptr,
      std::shared_ptr<archive_work::Passwords> passwords = nullptr
  ) {
    if (phase != ArchivePhase::Idle || closing || sources.empty()) {
      return false;
    }
    if (operation == Operation::Open && (openStages.size() >= 32 || openBytes >= 512U * 1024U * 1024U)) {
      error = previewFull();
      notify();
      return false;
    }
    error.reset();
    opened.reset();
    results.clear();
    progress.bytes.store(0);
    progress.items.store(0);
    cancel = CancelToken{};
    if (!known) {
      known = catalog;
    }
    auto expected = (known && known->source == sources.front()) ? std::move(known) : nullptr;
    progress.sourceStamp.clear();
    password.reset();
    if (operation == Operation::Inspect) {
      catalog.reset();
    }
    request = Request{
        operation,
        std::move(sources),
        destination,
        format,
        std::move(selected),
        std::move(expected),
        passwords ? std::move(passwords) : std::make_shared<archive_work::Passwords>()
    };
    launch();
    return true;
  }
  void launch() {
    const auto& task = *request;
    const auto operation = task.operation;
    const auto destination = task.destination;
    progress.bytes.store(0);
    progress.items.store(0);
    progress.passwordNeeded.store(false);
    progress.passwordFormat = ArchiveFormat::Zip;
    phase = operation == Operation::Inspect ? ArchivePhase::Reading
        : operation == Operation::Compress  ? ArchivePhase::Compressing
                                            : ArchivePhase::Extracting;
    // progress outlives the work: the done callback holds the state until after the work returns.
    runJob<archive_work::Prepared>(
        [limits = limits, progress = &progress, token = cancel, task] {
          const auto sourceMounts = openSystemMounts(task.sources, token, false);
          const auto nestedSourceMounts =
              task.operation == Operation::Compress ? openSystemMountsInside(task.sources, token) : std::vector<Fd>{};
          const auto destinationParent =
              task.operation != Operation::Open && task.destination ? task.destination->parent() : std::nullopt;
          const auto destinationMount = destinationParent ? openSystemMount(*destinationParent, token, true) : Fd{};
          if (task.operation == Operation::Inspect) {
            auto prepared = archive_work::inspect(task.sources.front(), limits, token, *progress, *task.passwords);
            assignArchiveIcons(*prepared.catalog);
            return prepared;
          }
          if (task.operation == Operation::Compress) {
            return archive_work::compress(
                task.sources, *task.destination, task.format, limits, token, *progress, *task.passwords
            );
          }
          return archive_work::extract(
              task.sources.front(), *task.destination, task.selected, task.expected, task.operation == Operation::Open,
              limits, token, *progress, *task.passwords
          );
        },
        [owner = shared_from_this(), token = cancel, operation,
         destination](JobResult<archive_work::Prepared> outcome) {
          owner->stopTimer();
          owner->error = outcome ? std::nullopt : std::optional(outcome.error());
          if (!owner->closing && !token.cancelled() && !outcome && owner->progress.passwordNeeded.load()) {
            owner->password = ArchivePasswordRequest{
                ++owner->nextPassword, owner->request->sources.front(), !owner->request->passwords->values.empty(),
                owner->progress.passwordFormat
            };
            owner->error.reset();
            owner->phase = ArchivePhase::WaitingPassword;
            owner->notify();
            return;
          }
          owner->request.reset();
          if (owner->closing || token.cancelled()) {
            if (!owner->error) {
              owner->error = FileError{FileErrorKind::Cancelled, "Archive operation cancelled."};
            }
            std::vector<std::string> paths;
            if (outcome && !outcome->stage.empty()) {
              paths.push_back(std::move(outcome->stage));
            }
            owner->clean(std::move(paths));
            return;
          }
          if (owner->error) {
            owner->clean({});
            return;
          }
          owner->finish(operation, destination, std::move(*outcome));
        }
    );
    startTimer();
    notify();
  }
};
luciole::ArchiveService::ArchiveService(ArchiveLimits limits) : m_state(std::make_shared<State>(limits)) {
  const std::weak_ptr<State> weak = m_state;
  m_state->transfers.setChangedCallback([weak] {
    if (const auto owner = weak.lock()) {
      owner->transferChanged();
    }
  });
}
luciole::ArchiveService::~ArchiveService() {
  m_state->changed = {};
  shutdown();
}
bool luciole::ArchiveService::inspect(Location source) {
  return m_state->start(State::Operation::Inspect, {std::move(source)}, std::nullopt, ArchiveFormat::Zip, {});
}
bool luciole::ArchiveService::compress(
    std::vector<Location> sources, Location destination, ArchiveFormat format, std::string password
) {
  auto passwords = std::make_shared<archive_work::Passwords>();
  if (!password.empty()) {
    passwords->values.push_back(std::move(password));
    if (archivePasswordProblem(passwords->values.back())
        || format != ArchiveFormat::Zip
        || !archiveEncryptionAvailable()) {
      return false;
    }
  }
  return m_state->start(
      State::Operation::Compress, std::move(sources), std::move(destination), format, {}, nullptr, std::move(passwords)
  );
}
bool luciole::ArchiveService::extract(
    Location source, Location destination, std::vector<std::string> selected,
    std::shared_ptr<const ArchiveCatalog> expected
) {
  return m_state->start(
      State::Operation::Extract, {std::move(source)}, std::move(destination), ArchiveFormat::Zip, std::move(selected),
      std::move(expected)
  );
}
bool luciole::ArchiveService::openMember(const std::string& path) {
  if (!m_state->catalog) {
    return false;
  }
  return m_state->start(
      State::Operation::Open, {m_state->catalog->source}, m_state->catalog->source, ArchiveFormat::Zip, {path}
  );
}
void luciole::ArchiveService::cancel() {
  m_state->cancel.cancel();
  m_state->transfers.cancel();
  if (m_state->phase == ArchivePhase::WaitingPassword) {
    m_state->request.reset();
    m_state->password.reset();
    m_state->error = FileError{FileErrorKind::Cancelled, "Archive operation cancelled."};
    m_state->clean({});
  }
}
void luciole::ArchiveService::shutdown() {
  if (m_state->closing) {
    return;
  }
  m_state->closing = true;
  cancel();
  if (!busy()) {
    m_state->clean({});
  }
}
bool luciole::ArchiveService::busy() const { return m_state->phase != ArchivePhase::Idle; }
luciole::ArchivePhase luciole::ArchiveService::phase() const { return m_state->phase; }
std::uint64_t luciole::ArchiveService::bytes() const {
  return m_state->phase == ArchivePhase::Transferring ? m_state->transfers.bytes() : m_state->progress.bytes.load();
}
std::size_t luciole::ArchiveService::items() const { return m_state->progress.items.load(); }
const std::optional<luciole::FileError>& luciole::ArchiveService::error() const { return m_state->error; }
const std::shared_ptr<const luciole::ArchiveCatalog>& luciole::ArchiveService::catalog() const {
  return m_state->catalog;
}
std::optional<luciole::Location> luciole::ArchiveService::takeOpenedFile() {
  return std::exchange(m_state->opened, std::nullopt);
}
const std::vector<luciole::TransferResult>& luciole::ArchiveService::results() const { return m_state->results; }
std::optional<luciole::TransferConflict> luciole::ArchiveService::conflict() const {
  return m_state->transfers.conflict();
}
bool luciole::ArchiveService::resolve(std::uint64_t id, TransferConflictChoice choice) {
  return m_state->transfers.resolve(id, choice);
}
void luciole::ArchiveService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}

std::optional<luciole::ArchivePasswordRequest> luciole::ArchiveService::passwordRequest() const {
  return m_state->password;
}
bool luciole::ArchiveService::providePassword(std::uint64_t id, std::string password) {
  archive_work::Passwords incoming;
  incoming.values.push_back(std::move(password));
  auto& value = incoming.values.back();
  if (!m_state->password
      || m_state->password->id != id
      || m_state->phase != ArchivePhase::WaitingPassword
      || value.empty()
      || archivePasswordProblem(value, m_state->password->format)) {
    return false;
  }
  auto& passwords = m_state->request->passwords->values;
  if (passwords.size() >= 8) {
    ::explicit_bzero(passwords.front().data(), passwords.front().size());
    passwords.erase(passwords.begin());
  }
  passwords.push_back(std::move(value));
  m_state->password.reset();
  m_state->launch();
  return true;
}
