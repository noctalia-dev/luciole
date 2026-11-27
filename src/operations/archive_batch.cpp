#include "operations/archive_batch.h"

#include "fs/background_job.h"
#include "operations/archive_input.h"
#include "operations/archive_support.h"

#include <algorithm>
#include <format>
#include <utility>

namespace {
  namespace fs = std::filesystem;
  using namespace luciole;
  fs::path resolved(const Location& location) {
    std::error_code code;
    auto path = fs::weakly_canonical(*location.nativePath(), code);
    if (code) {
      throwFileError(errnoError(code.value()).kind, "Could not inspect archive paths: " + code.message());
    }
    return path;
  }
  bool contains(const fs::path& parent, const fs::path& path) {
    return std::ranges::mismatch(parent, path).in1 == parent.end();
  }
  std::optional<FileError> protectedSource(const fs::path& destination, const std::vector<fs::path>& sources) {
    if (std::ranges::any_of(sources, [&](const auto& source) { return contains(destination, source); })) {
      return FileError{FileErrorKind::InvalidArgument, "The output folder would replace a selected source archive."};
    }
    return {};
  }
  std::string uniqueName(const fs::path& parent, const std::string& name, std::set<fs::path>& reserved) {
    auto candidate = name;
    for (std::size_t number = 1; !reserved.insert(parent / candidate).second; ++number) {
      candidate = std::format("{} ({})", name, number);
    }
    return candidate;
  }
  TransferResult outcome(const ArchiveBatch::Task& task, TransferOutcome value, std::optional<FileError> error) {
    TransferResult result(task.source, value, std::move(error));
    result.destination = task.destination;
    return result;
  }
  ArchiveBatch::Plan prepare(ArchiveBatch::Plan plan, const CancelToken& cancel) {
    archive_work::Progress progress;
    auto work = archive_work::workFor({}, cancel, progress);
    for (auto& task : plan.tasks) {
      work.check();
      plan.sources.push_back(fs::path(*task.source.nativePath()).lexically_normal());
      try {
        plan.sources.push_back(resolved(task.source));
        archive_work::Input input(task.source, work);
        task.stamp = input.fingerprint();
        for (const auto& path : input.sourcePaths()) {
          plan.sources.push_back(resolved(Location::fromPath(path.native())));
        }
      } catch (const FileErrorException& failure) {
        task.error = failure.error();
      }
    }
    for (auto& task : plan.tasks) {
      work.check();
      try {
        task.parent = resolved(*task.destination.parent());
        task.name = uniqueName(task.parent, task.name, plan.reserved);
        task.destination = task.destination.parent()->child(task.name);
        if (const auto error = protectedSource(resolved(task.destination), plan.sources)) {
          task.error = error;
        }
      } catch (const FileErrorException& failure) {
        task.error = failure.error();
      }
    }
    return plan;
  }
} // namespace

luciole::ArchiveBatch::ArchiveBatch(std::unique_ptr<ArchiveService> child, std::function<void()> callback)
    : worker(std::move(child)), changed(std::move(callback)) {}
luciole::ArchiveBatch::~ArchiveBatch() {
  worker->setChangedCallback({});
  worker->shutdown();
}
void luciole::ArchiveBatch::notify() {
  const auto callback = changed;
  if (callback) {
    callback();
  }
}
void luciole::ArchiveBatch::start(std::vector<Location> sources, const std::optional<Location>& parent) {
  plan.tasks.reserve(sources.size());
  for (auto& source : sources) {
    auto destination = parent.value_or(*source.parent()).child(archiveFolderName(source));
    auto name = *destination.basename();
    plan.tasks.push_back({std::move(source), std::move(destination), {}, std::move(name), {}, {}});
  }
  const std::weak_ptr<ArchiveBatch> weak = shared_from_this();
  worker->setChangedCallback([weak] {
    if (const auto owner = weak.lock()) {
      owner->childChanged();
    }
  });
  runJob<Plan>(
      [tasks = plan, cancel = m_cancel] { return prepare(tasks, cancel); },
      [owner = shared_from_this()](JobResult<Plan> result) {
        owner->m_preparing = false;
        if (result) {
          owner->plan = std::move(*result);
        } else {
          for (auto& task : owner->plan.tasks) {
            task.error = result.error();
          }
        }
        owner->schedule();
      }
  );
  notify();
}
void luciole::ArchiveBatch::schedule() {
  if (m_next) {
    return;
  }
  m_next = g_idle_source_new();
  g_source_set_callback(
      m_next,
      [](gpointer data) -> gboolean {
        const auto owner = *static_cast<std::shared_ptr<ArchiveBatch>*>(data);
        g_source_unref(std::exchange(owner->m_next, nullptr));
        owner->advance();
        return G_SOURCE_REMOVE;
      },
      new std::shared_ptr<ArchiveBatch>(shared_from_this()),
      [](gpointer data) { delete static_cast<std::shared_ptr<ArchiveBatch>*>(data); }
  );
  g_source_attach(m_next, g_main_context_default());
}
void luciole::ArchiveBatch::record(TransferResult result) {
  if (result.error && result.outcome != TransferOutcome::Skipped && !error) {
    error = result.error;
  }
  results.push_back(std::move(result));
}
void luciole::ArchiveBatch::advance() {
  if (m_stopping) {
    while (index < plan.tasks.size()) {
      const auto& task = plan.tasks[index++];
      record(
          outcome(task, TransferOutcome::Cancelled, FileError{FileErrorKind::Cancelled, "Archive batch cancelled."})
      );
    }
  }
  if (index == plan.tasks.size()) {
    running = false;
    notify();
    return;
  }
  auto& task = plan.tasks[index];
  if (m_published.contains(task.parent / task.name)) {
    task.name = uniqueName(task.parent, archiveFolderName(task.source), plan.reserved);
    task.destination = task.destination.parent()->child(task.name);
    if (!task.error) {
      task.error = protectedSource(task.parent / task.name, plan.sources);
    }
  }
  if (task.error) {
    record(outcome(task, TransferOutcome::Failed, task.error));
    ++index;
    schedule();
    notify();
    return;
  }
  m_active = true;
  if (!worker->extract(task.source, task.destination, {}, nullptr, task.stamp)) {
    m_active = false;
    record(
        outcome(task, TransferOutcome::Failed, FileError{FileErrorKind::Other, "Could not start archive extraction."})
    );
    ++index;
    schedule();
    notify();
  }
}
void luciole::ArchiveBatch::childChanged() {
  if (m_active && !worker->busy()) {
    const auto& task = plan.tasks[index];
    TransferResult result =
        worker->results().empty() ? outcome(task, TransferOutcome::Failed, worker->error()) : worker->results().front();
    result.source = task.source;
    if (worker->error() && !result.error && result.outcome != TransferOutcome::Skipped) {
      result.error = worker->error();
      if (result.outcome == TransferOutcome::Moved && result.error->kind != FileErrorKind::Cancelled) {
        result.outcome = TransferOutcome::Partial;
      }
    }
    if (worker->error() && worker->error()->kind == FileErrorKind::Cancelled) {
      if (result.outcome == TransferOutcome::Failed) {
        result.outcome = TransferOutcome::Cancelled;
      }
      result.error = worker->error();
      m_stopping = true;
    }
    if (result.destination
        && (result.outcome == TransferOutcome::Moved || result.outcome == TransferOutcome::Partial)) {
      const auto published = task.parent / *result.destination->basename();
      m_published.insert(published);
      plan.reserved.insert(published);
    }
    record(std::move(result));
    m_active = false;
    ++index;
    schedule();
  }
  notify();
}
void luciole::ArchiveBatch::cancel(bool shutdown) {
  if (!running) {
    if (shutdown) {
      worker->shutdown();
    }
    return;
  }
  m_stopping = true;
  m_cancel.cancel();
  if (shutdown) {
    worker->shutdown();
  } else {
    worker->cancel();
  }
  if (!m_preparing && !m_active) {
    schedule();
  }
}
luciole::ArchivePhase luciole::ArchiveBatch::phase() const {
  return !running ? ArchivePhase::Idle : m_active ? worker->phase() : ArchivePhase::Reading;
}
std::optional<luciole::Location> luciole::ArchiveBatch::source() const {
  return index < plan.tasks.size() ? std::optional(plan.tasks[index].source) : std::nullopt;
}
