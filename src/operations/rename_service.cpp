#include "operations/rename_service.h"

#include "fs/background_job.h"
#include "fs/filename.h"
#include "fs/posix_fd.h"
#include "fs/system_mounts.h"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  struct Target {
    std::string parentPath, name;
    luciole::Fd parent, item;
    explicit Target(const std::string& path)
        : parentPath(std::filesystem::path(path).parent_path().native()),
          name(std::filesystem::path(path).filename().native()),
          parent(luciole::checkedFd(::open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))),
          item(luciole::checkedFd(::openat(parent.get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC))) {}
  };
  struct Reply {
    std::shared_ptr<Target> target;
    std::optional<luciole::Location> destination;
    std::shared_ptr<const luciole::RenameReceipt> receipt;
  };
} // namespace

struct luciole::RenameService::State {
  RenameSnapshot snapshot;
  std::shared_ptr<Target> target;
  CancelToken token;
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
          if (result && result->destination) {
            owner->snapshot.destination = std::move(result->destination);
            owner->snapshot.status = RenameStatus::Succeeded;
            owner->snapshot.receipt = std::move(result->receipt);
            owner->target.reset();
          } else if (token.cancelled()) {
            owner->snapshot.status = RenameStatus::Cancelled;
            owner->target.reset();
          } else if (!result) {
            owner->snapshot.status = RenameStatus::Error;
          } else {
            owner->target = std::move(result->target);
            owner->snapshot.status = RenameStatus::Ready;
          }
          owner->notify();
        }
    );
    owner->notify();
  }
};
luciole::RenameService::RenameService() : m_state(std::make_shared<State>()) {}
luciole::RenameService::~RenameService() {
  m_state->changed = {};
  cancel();
}
void luciole::RenameService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::RenameService::busy() const { return m_state->active; }
bool luciole::RenameService::canRename() const { return !busy() && m_state->target != nullptr; }
const luciole::RenameSnapshot& luciole::RenameService::snapshot() const { return m_state->snapshot; }
std::shared_ptr<const luciole::RenameReceipt> luciole::RenameService::takeReceipt() {
  return std::move(m_state->snapshot.receipt);
}
void luciole::RenameService::cancel() {
  m_state->token.cancel();
  m_state->target.reset();
  if (!busy() && m_state->snapshot.status != RenameStatus::Succeeded) {
    m_state->snapshot.status = RenameStatus::Cancelled;
  }
}
bool luciole::RenameService::prepare(const Location& source) {
  if (busy()) {
    return false;
  }
  m_state->target.reset();
  m_state->snapshot = {
      .status = RenameStatus::Preparing, .source = source, .destination = std::nullopt, .error = {}, .receipt = {}
  };
  State::launch(m_state, [source](const CancelToken& token) {
    const auto path = source.nativePath();
    if (!path) {
      throwFileError(FileErrorKind::Unsupported, "Renaming remote items is not supported yet.");
    }
    const auto mount = openSystemMount(source, token, false);
    const auto name = std::filesystem::path(*path).filename().native();
    if (name.empty() || name == "." || name == "..") {
      throwFileError(FileErrorKind::InvalidArgument, "The filesystem root cannot be renamed.");
    }
    return Reply{.target = std::make_shared<Target>(*path), .destination = std::nullopt, .receipt = {}};
  });
  return true;
}
bool luciole::RenameService::rename(std::string name) {
  if (!canRename()) {
    return false;
  }
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = RenameStatus::Error;
    m_state->snapshot.error = FileError{.kind = FileErrorKind::InvalidArgument, .message = *error};
    m_state->notify();
    return false;
  }
  m_state->snapshot.status = RenameStatus::Renaming;
  m_state->snapshot.error.reset();
  State::launch(m_state, [target = m_state->target, name = std::move(name)](const CancelToken& token) {
    const auto mount = openSystemMount(Location::fromPath(target->parentPath), token);
    auto destination = Location::fromPath((std::filesystem::path(target->parentPath) / name).native());
    const auto parent = checkedFd(::open(target->parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!sameIdentity(statFd(parent.get()), statFd(target->parent.get()))) {
      throwFileError(FileErrorKind::Changed, "The containing folder changed. Reopen Rename.");
    }
    struct stat current{};
    if (::fstatat(target->parent.get(), target->name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
      throwErrno(errno);
    }
    if (!sameIdentity(statFd(target->item.get()), current)) {
      throwFileError(FileErrorKind::Changed, "The item was replaced or moved. Reopen Rename.");
    }
    const auto limit = ::fpathconf(target->parent.get(), _PC_NAME_MAX);
    if (limit >= 0 && name.size() > static_cast<std::size_t>(limit)) {
      throwErrno(ENAMETOOLONG);
    }
    if (token.cancelled()) {
      return Reply{};
    }
    if (name != target->name
        && luciole::renameNoReplace(target->parent.get(), target->name.c_str(), target->parent.get(), name.c_str())
            != 0) {
      throwErrno(errno);
    }
    std::shared_ptr<const RenameReceipt> receipt;
    if (name != target->name) {
      try {
        receipt = RenameReceipt::capture(
            Location::fromPath((std::filesystem::path(target->parentPath) / target->name).native()), destination,
            target->parent.get(), target->item.get()
        );
      } catch (const FileErrorException&) {
        // Successful mutation remains successful even if the optional history cannot be captured.
      }
    }
    return Reply{.target = {}, .destination = std::move(destination), .receipt = std::move(receipt)};
  });
  return true;
}

bool luciole::RenameService::replay(std::shared_ptr<const RenameReceipt> receipt) {
  if (busy() || !receipt) {
    return false;
  }
  m_state->target.reset();
  m_state->snapshot = {
      .status = RenameStatus::Renaming, .source = receipt->destination(), .destination = {}, .error = {}, .receipt = {}
  };
  State::launch(m_state, [receipt = std::move(receipt)](const CancelToken& token) {
    auto inverse = receipt->reverse(token);
    return Reply{.target = {}, .destination = receipt->source(), .receipt = std::move(inverse)};
  });
  return true;
}
