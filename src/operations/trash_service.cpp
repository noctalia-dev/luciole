#include "operations/trash_service.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "fs/system_mounts.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  struct Parent {
    std::string path;
    luciole::Fd directory;
    explicit Parent(std::string path)
        : path(std::move(path)),
          directory(luciole::checkedFd(::open(this->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))) {}
  };
  struct Target {
    luciole::Location location;
    std::string name;
    luciole::Fd item;
    std::shared_ptr<Parent> parent;
    Target(luciole::Location location, std::string name, std::shared_ptr<Parent> parent)
        : location(std::move(location)), name(std::move(name)),
          item(
              luciole::checkedFd(::openat(parent->directory.get(), this->name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC))
          ),
          parent(std::move(parent)) {}
  };
  struct Prepared {
    std::map<std::string, std::shared_ptr<Parent>> parents;
    std::vector<std::unique_ptr<Target>> targets;
  };
  struct Reply {
    std::shared_ptr<Prepared> prepared;
    std::vector<luciole::TrashResult> results;
    bool finished = false;
  };
} // namespace

struct luciole::TrashService::State {
  TrashSnapshot snapshot;
  std::shared_ptr<Prepared> prepared;
  CancelToken token;
  std::atomic<std::size_t> completed{0};
  bool active = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void launch(const std::shared_ptr<State>& owner, std::move_only_function<Reply(const CancelToken&)> work) {
    owner->active = true;
    owner->token = CancelToken{};
    runJob<Reply>(
        owner->token, [token = owner->token, work = std::move(work)] mutable { return work(token); },
        [owner, token = owner->token](JobResult<Reply> result) {
          owner->active = false;
          owner->snapshot.error.reset();
          if (!result && result.error().kind != FileErrorKind::Cancelled) {
            owner->snapshot.error = std::move(result.error());
          }
          if (result && result->finished) {
            owner->snapshot.results = std::move(result->results);
            owner->snapshot.status = TrashStatus::Finished;
            owner->prepared.reset();
          } else if (token.cancelled()) {
            owner->snapshot.status = TrashStatus::Cancelled;
            owner->prepared.reset();
          } else if (!result) {
            owner->snapshot.status = TrashStatus::Error;
            owner->prepared.reset();
          } else {
            owner->prepared = std::move(result->prepared);
            owner->snapshot.status = TrashStatus::Ready;
          }
          owner->notify();
        }
    );
    owner->notify();
  }
};

luciole::TrashService::TrashService() : m_state(std::make_shared<State>()) {}
luciole::TrashService::~TrashService() {
  m_state->changed = {};
  cancel();
}
void luciole::TrashService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::TrashService::busy() const { return m_state->active; }
bool luciole::TrashService::canTrash() const { return !busy() && m_state->prepared != nullptr; }
std::size_t luciole::TrashService::completed() const { return m_state->completed.load(); }
const luciole::TrashSnapshot& luciole::TrashService::snapshot() const { return m_state->snapshot; }
void luciole::TrashService::cancel() {
  m_state->token.cancel();
  m_state->prepared.reset();
  if (!busy() && m_state->snapshot.status != TrashStatus::Finished) {
    m_state->snapshot.status = TrashStatus::Cancelled;
  }
}
bool luciole::TrashService::prepare(const Location& parent, std::vector<Location> sources) {
  return prepareSources(parent, std::move(sources));
}
bool luciole::TrashService::prepare(std::vector<Location> sources) {
  return prepareSources(std::nullopt, std::move(sources));
}
bool luciole::TrashService::prepareSources(const std::optional<Location>& parent, std::vector<Location> sources) {
  if (busy() || sources.empty() || sources.size() > 4096) {
    return false;
  }
  m_state->prepared.reset();
  m_state->completed.store(0);
  m_state->snapshot = {.status = TrashStatus::Preparing, .results = {}, .error = {}};
  for (const auto& source : sources) {
    m_state->snapshot.results.push_back({source, TrashOutcome::Cancelled, {}});
  }
  State::launch(m_state, [parent, sources = std::move(sources)](const CancelToken& token) {
    const auto expectedParent = parent ? parent->nativePath() : std::nullopt;
    if (parent && !expectedParent) {
      throwFileError(FileErrorKind::Unsupported, "Moving remote items to Trash is not supported yet.");
    }
    const auto sourceMounts = openSystemMounts(sources, token, false);
    const auto parentMount = parent ? openSystemMount(*parent, token, true) : Fd{};
    auto prepared = std::make_shared<Prepared>();
    std::set<std::string, std::less<>> paths;
    for (const auto& source : sources) {
      if (token.cancelled()) {
        return Reply{};
      }
      const auto native = source.isNative() ? source.nativePath() : std::nullopt;
      if (!native || (expectedParent && std::filesystem::path(*native).parent_path().native() != *expectedParent)) {
        throwFileError(
            FileErrorKind::InvalidArgument, "Trash requires native items from the expected containing folders."
        );
      }
      const auto name = std::filesystem::path(*native).filename().native();
      if (native->size() >= PATH_MAX || name.empty() || name == "." || name == ".." || !paths.insert(*native).second) {
        throwFileError(FileErrorKind::InvalidArgument, "The Trash selection contains an invalid or repeated item.");
      }
    }
    for (const auto& path : paths) {
      for (auto separator = path.find('/', 1); separator != std::string::npos;
           separator = path.find('/', separator + 1)) {
        if (paths.contains(std::string_view(path).substr(0, separator))) {
          throwFileError(FileErrorKind::InvalidArgument, "The Trash selection contains overlapping items.");
        }
      }
    }
    for (const auto& source : sources) {
      if (token.cancelled()) {
        return Reply{};
      }
      const std::filesystem::path path(*source.nativePath());
      const auto folder = path.parent_path().native();
      auto& directory = prepared->parents[folder];
      if (!directory) {
        directory = std::make_shared<Parent>(folder);
      }
      prepared->targets.push_back(std::make_unique<Target>(source, path.filename().native(), directory));
    }
    return Reply{.prepared = std::move(prepared), .results = {}, .finished = false};
  });
  return true;
}
bool luciole::TrashService::trash() {
  if (!canTrash()) {
    return false;
  }
  m_state->snapshot.status = TrashStatus::Trashing;
  State::launch(m_state, [owner = m_state, prepared = m_state->prepared](const CancelToken& token) {
    std::vector<Location> parentLocations;
    parentLocations.reserve(prepared->parents.size());
    for (const auto& [path, _] : prepared->parents) {
      parentLocations.push_back(Location::fromPath(path));
    }
    std::vector<Location> targetLocations;
    targetLocations.reserve(prepared->targets.size());
    for (const auto& target : prepared->targets) {
      targetLocations.push_back(target->location);
    }
    const auto parentMounts = openSystemMounts(parentLocations, token, true);
    const auto targetMounts = openSystemMounts(targetLocations, token, false);
    Reply reply{.prepared = {}, .results = {}, .finished = true};
    for (const auto& target : prepared->targets) {
      reply.results.push_back({target->location, TrashOutcome::Cancelled, {}});
    }
    for (std::size_t index = 0; index < prepared->targets.size(); ++index) {
      if (token.cancelled()) {
        break;
      }
      const auto& target = *prepared->targets[index];
      auto& result = reply.results[index];
      try {
        const auto currentParent = checkedFd(::open(target.parent->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!sameIdentity(statFd(target.parent->directory.get()), statFd(currentParent.get()))) {
          throwFileError(FileErrorKind::Changed, "The containing folder changed. Select the items again.");
        }
        struct stat current{};
        if (::fstatat(target.parent->directory.get(), target.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
          throwErrno(errno);
        }
        if (!sameIdentity(statFd(target.item.get()), current)) {
          throwFileError(FileErrorKind::Changed, "The item was replaced or moved. Select it again.");
        }
        if (token.cancelled()) {
          break;
        }
        GError* rawError = nullptr;
        if (g_file_trash(gioFile(target.location).get(), gioCancellable(token), &rawError)) {
          result.outcome = TrashOutcome::Trashed;
        } else {
          const GErrorPtr error(rawError);
          result.error = error
              ? fileError(*error)
              : FileError{.kind = FileErrorKind::Other, .message = "Could not move the item to Trash."};
          result.outcome =
              result.error->kind == FileErrorKind::Cancelled ? TrashOutcome::Cancelled : TrashOutcome::Failed;
        }
      } catch (const std::exception&) {
        result.outcome = TrashOutcome::Failed;
        result.error = currentFileError();
      }
      owner->completed.fetch_add(1);
    }
    return reply;
  });
  return true;
}
