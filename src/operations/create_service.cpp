#include "operations/create_service.h"

#include "fs/background_job.h"
#include "fs/filename.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "fs/system_mounts.h"
#include "operations/create_receipt.h"
#include "operations/creation_stage.h"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  struct TemplateSource {
    luciole::DocumentTemplate source;
    luciole::Fd file;
    explicit TemplateSource(luciole::DocumentTemplate source)
        : source(std::move(source)), file(
                                         luciole::checkedFd(
                                             ::open(
                                                 this->source.location.nativePath().value_or("").c_str(),
                                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
                                             )
                                         )
                                     ) {
      requireCurrent();
    }
    void requireCurrent() const {
      const auto current = luciole::checkedFd(
          ::open(source.location.nativePath().value_or("").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
      );
      if (luciole::templateRevision(luciole::statFd(current.get())) != source.revision
          || luciole::templateRevision(luciole::statFd(file.get())) != source.revision) {
        luciole::throwFileError(
            luciole::FileErrorKind::Changed, "The template changed. Close this dialog and choose it again."
        );
      }
    }
  };
  struct Target {
    std::string path;
    luciole::Fd directory;
    std::unique_ptr<TemplateSource> source;
    std::optional<std::string> image;
    explicit Target(std::string path)
        : path(std::move(path)),
          directory(luciole::checkedFd(::open(this->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))) {}
    void requireCurrent() const {
      const auto current = luciole::checkedFd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!luciole::sameIdentity(luciole::statFd(current.get()), luciole::statFd(directory.get()))) {
        luciole::throwFileError(
            luciole::FileErrorKind::Changed, "The destination folder changed. Close this dialog and try again."
        );
      }
    }
  };
  struct LinkSource {
    std::string path, name;
    Target parent;
    luciole::Fd item;
    explicit LinkSource(const luciole::DirectoryEntry& source)
        : path(source.location().nativePath().value_or("")), name(std::filesystem::path(path).filename().native()),
          parent(std::filesystem::path(path).parent_path().native()),
          item(luciole::checkedFd(::openat(parent.directory.get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC))) {
      const luciole::GioPtr<GFileInfo> info(g_file_query_info(
          luciole::gioFile(source.location()).get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
      ));
      if (!info
          || luciole::readFileIdentity(info.get()) != source.identity()
          || luciole::fileType(g_file_info_get_file_type(info.get())) != source.type
          || (g_file_info_get_is_symlink(info.get()) != FALSE) != source.symlink) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "The selected item changed. Choose it again.");
      }
      requireCurrent();
    }
    void requireCurrent() const {
      parent.requireCurrent();
      struct stat current{};
      if (::fstatat(parent.directory.get(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
          || !luciole::sameIdentity(current, luciole::statFd(item.get()))) {
        luciole::throwFileError(
            luciole::FileErrorKind::Changed, "The source was moved or replaced. Close this dialog and choose it again."
        );
      }
    }
  };
  struct Reply {
    std::shared_ptr<Target> target;
    std::optional<luciole::Location> destination;
    // An applied effect whose private-folder cleanup failed.
    std::optional<luciole::FileError> error;
    std::shared_ptr<LinkSource> linkSource;
    std::shared_ptr<const luciole::CreateReceipt> receipt;
    bool removed = false, retireHistory = false;
  };
  constexpr auto crossDeviceLink = "Hard links require a destination on the same filesystem.";
  Reply publish(
      luciole::CreationStage& stage, const Target& target, const std::string& name, luciole::CreateKind kind,
      const luciole::CancelToken& token
  ) {
    target.requireCurrent();
    if (token.cancelled()) {
      return Reply{};
    }
    stage.publish(name);
    Reply reply{
        .target = {},
        .destination = luciole::Location::fromPath((std::filesystem::path(target.path) / name).native()),
        .error = {},
        .linkSource = {},
        .receipt = {}
    };
    try {
      reply.receipt = luciole::CreateReceipt::capture(kind, *reply.destination, target.directory.get(), stage.item());
    } catch (const luciole::FileErrorException&) {
      // Publishing succeeded; failure to capture optional history must not report it as unapplied.
    }
    if (!stage.clear()) {
      reply.receipt.reset();
      reply.error = luciole::FileError{
          .kind = luciole::FileErrorKind::Other,
          .message = "The item was created, but its private folder could not be removed: "
              + luciole::filenameDisplayText(stage.recoveryPath())
      };
    }
    return reply;
  }

} // namespace

struct luciole::CreateService::State {
  CreateSnapshot snapshot;
  std::shared_ptr<Target> target;
  std::shared_ptr<LinkSource> linkSource;
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
          if (result) {
            owner->snapshot.removed = result->removed;
            owner->snapshot.retireHistory = result->retireHistory;
            owner->snapshot.error = std::move(result->error);
          } else if (result.error().kind != FileErrorKind::Cancelled) {
            owner->snapshot.error = std::move(result.error());
          }
          if (result && result->destination) {
            owner->snapshot.destination = std::move(result->destination);
            owner->snapshot.receipt = std::move(result->receipt);
            owner->snapshot.status = owner->snapshot.error ? CreateStatus::Partial : CreateStatus::Succeeded;
            owner->target.reset();
            owner->linkSource.reset();
          } else if (token.cancelled()) {
            owner->snapshot.status = CreateStatus::Cancelled;
            owner->target.reset();
            owner->linkSource.reset();
          } else if (!result || result->error) {
            owner->snapshot.status = CreateStatus::Error;
          } else {
            owner->target = std::move(result->target);
            owner->linkSource = std::move(result->linkSource);
            owner->snapshot.status = CreateStatus::Ready;
          }
          owner->notify();
        }
    );
    owner->notify();
  }
};
luciole::CreateService::CreateService() : m_state(std::make_shared<State>()) {}
luciole::CreateService::~CreateService() {
  m_state->changed = {};
  cancel();
}
void luciole::CreateService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::CreateService::busy() const { return m_state->active; }
bool luciole::CreateService::canCreate() const {
  return !busy() && (m_state->target != nullptr || m_state->linkSource != nullptr);
}
const luciole::CreateSnapshot& luciole::CreateService::snapshot() const { return m_state->snapshot; }
std::shared_ptr<const luciole::CreateReceipt> luciole::CreateService::takeReceipt() {
  return std::move(m_state->snapshot.receipt);
}
void luciole::CreateService::cancel() {
  m_state->token.cancel();
  m_state->target.reset();
  m_state->linkSource.reset();
  if (!busy()
      && m_state->snapshot.status != CreateStatus::Succeeded
      && m_state->snapshot.status != CreateStatus::Partial) {
    m_state->snapshot.status = CreateStatus::Cancelled;
  }
}
bool luciole::CreateService::prepare(const Location& parent, CreateKind kind) {
  if (kind != CreateKind::Folder && kind != CreateKind::EmptyFile) {
    return false;
  }
  return prepareTarget(parent, kind, std::nullopt);
}
bool luciole::CreateService::prepareTemplate(const Location& parent, DocumentTemplate source) {
  return prepareTarget(parent, CreateKind::Document, std::move(source));
}
bool luciole::CreateService::prepareImage(const Location& parent, std::string bytes) {
  return prepareTarget(parent, CreateKind::Image, std::nullopt, std::move(bytes));
}
bool luciole::CreateService::prepareLink(const DirectoryEntry& source) {
  if (busy()
      || !source.location().nativePath()
      || !source.identity()
      || (source.type != FileType::Regular && source.type != FileType::Directory && source.type != FileType::Symlink)) {
    return false;
  }
  m_state->target.reset();
  m_state->linkSource.reset();
  m_state->snapshot = {
      .status = CreateStatus::Preparing,
      .kind = CreateKind::SymbolicLink,
      .parent = std::nullopt,
      .destination = std::nullopt,
      .error = {},
      .receipt = {}
  };
  State::launch(m_state, [source](const CancelToken& token) {
    const auto mount = openSystemMount(source.location(), token, false);
    return Reply{
        .target = {},
        .destination = std::nullopt,
        .error = {},
        .linkSource = std::make_shared<LinkSource>(source),
        .receipt = {}
    };
  });
  return true;
}
bool luciole::CreateService::createLink(const Location& parent, std::string name, CreateKind kind) {
  if (!canCreate() || !m_state->linkSource || (kind != CreateKind::SymbolicLink && kind != CreateKind::HardLink)) {
    return false;
  }
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateStatus::Error;
    m_state->snapshot.error = FileError{.kind = FileErrorKind::InvalidArgument, .message = *error};
    m_state->notify();
    return false;
  }
  m_state->snapshot.kind = kind;
  m_state->snapshot.parent = parent;
  m_state->snapshot.status = CreateStatus::Creating;
  m_state->snapshot.error.reset();
  State::launch(
      m_state, [source = m_state->linkSource, parent, name = std::move(name), kind](const CancelToken& token) {
        const auto path = parent.nativePath();
        if (!path) {
          throwFileError(FileErrorKind::Unsupported, "Choose a native destination folder.");
        }
        const auto destinationMount = openSystemMount(parent, token, true);
        const auto sourceMount = openSystemMount(Location::fromPath(source->path), token, false);
        Target destination(*path);
        destination.requireCurrent();
        source->requireCurrent();
        const auto sourceInfo = statFd(source->item.get());
        if (kind == CreateKind::HardLink && !S_ISREG(sourceInfo.st_mode)) {
          throwFileError(
              FileErrorKind::Unsupported,
              "Hard links are available only for regular files, without following symbolic links."
          );
        }
        if (kind == CreateKind::HardLink && sourceInfo.st_dev != statFd(destination.directory.get()).st_dev) {
          throwFileError(FileErrorKind::Unsupported, crossDeviceLink);
        }
        const auto limit = ::fpathconf(destination.directory.get(), _PC_NAME_MAX);
        if (limit >= 0 && name.size() > static_cast<std::size_t>(limit)) {
          throwErrno(ENAMETOOLONG);
        }
        CreationStage stage(destination.directory.get());
        if (token.cancelled()) {
          return Reply{};
        }
        const auto held = "/proc/self/fd/" + std::to_string(source->item.get());
        const int result = kind == CreateKind::HardLink
            ? ::linkat(AT_FDCWD, held.c_str(), stage.fd(), "item", AT_SYMLINK_FOLLOW)
            : ::symlinkat(source->path.c_str(), stage.fd(), "item");
        if (result != 0) {
          if (errno == EXDEV) {
            throwFileError(FileErrorKind::Unsupported, crossDeviceLink);
          }
          throwErrno(errno);
        }
        stage.adopt(checkedFd(::openat(stage.fd(), "item", O_PATH | O_NOFOLLOW | O_CLOEXEC)));
        return publish(stage, destination, name, kind, token);
      }
  );
  return true;
}
bool luciole::CreateService::prepareTarget(
    const Location& parent, CreateKind kind, std::optional<DocumentTemplate> source, std::optional<std::string> image
) {
  if (busy()) {
    return false;
  }
  m_state->target.reset();
  m_state->linkSource.reset();
  m_state->snapshot = {
      .status = CreateStatus::Preparing,
      .kind = kind,
      .parent = parent,
      .destination = std::nullopt,
      .error = {},
      .receipt = {}
  };
  State::launch(
      m_state, [parent, source = std::move(source), image = std::move(image)](const CancelToken& token) mutable {
        const auto path = parent.nativePath();
        if (!path) {
          throwFileError(FileErrorKind::Unsupported, "Creating remote files or folders is not supported yet.");
        }
        const auto mount = openSystemMount(parent, token, true);
        const auto templateMount = source ? openSystemMount(source->location, token, false) : Fd{};
        auto target = std::make_shared<Target>(*path);
        if (source) {
          target->source = std::make_unique<TemplateSource>(std::move(*source));
        }
        target->image = std::move(image);
        return Reply{
            .target = std::move(target), .destination = std::nullopt, .error = {}, .linkSource = {}, .receipt = {}
        };
      }
  );
  return true;
}
bool luciole::CreateService::create(std::string name) {
  if (!canCreate() || !m_state->target) {
    return false;
  }
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateStatus::Error;
    m_state->snapshot.error = FileError{.kind = FileErrorKind::InvalidArgument, .message = *error};
    m_state->notify();
    return false;
  }
  m_state->snapshot.status = CreateStatus::Creating;
  m_state->snapshot.error.reset();
  State::launch(
      m_state,
      [target = m_state->target, kind = m_state->snapshot.kind, name = std::move(name)](const CancelToken& token) {
        const auto mount = openSystemMount(Location::fromPath(target->path), token, true);
        const auto templateMount =
            target->source ? openSystemMount(target->source->source.location, token, false) : Fd{};
        target->requireCurrent();
        const auto limit = ::fpathconf(target->directory.get(), _PC_NAME_MAX);
        if (limit >= 0 && name.size() > static_cast<std::size_t>(limit)) {
          throwErrno(ENAMETOOLONG);
        }
        if (token.cancelled()) {
          return Reply{};
        }
        if (kind == CreateKind::Folder) {
          if (::mkdirat(target->directory.get(), name.c_str(), 0777) != 0) {
            throwErrno(errno);
          }
          Reply reply{
              .target = {},
              .destination = Location::fromPath((std::filesystem::path(target->path) / name).native()),
              .error = {},
              .linkSource = {},
              .receipt = {}
          };
          try {
            const auto item = checkedFd(
                ::openat(target->directory.get(), name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            );
            reply.receipt = CreateReceipt::capture(kind, *reply.destination, target->directory.get(), item.get());
          } catch (const FileErrorException&) {
            // The folder already exists even when optional history capture fails.
          }
          return reply;
        }
        CreationStage stage(target->directory.get());
        stage.adopt(checkedFd(::openat(stage.fd(), "item", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666)));
        if (kind == CreateKind::Document) {
          auto& source = *target->source;
          source.requireCurrent();
          copyCreationData(source.file.get(), stage.item(), source.source.revision.size, token);
          source.requireCurrent();
        } else if (kind == CreateKind::Image) {
          writeCreationData(*target->image, stage.item(), token);
        }
        return publish(stage, *target, name, kind, token);
      }
  );
  return true;
}

bool luciole::CreateService::replay(std::shared_ptr<const CreateReceipt> receipt) {
  if (busy() || !receipt) {
    return false;
  }
  m_state->target.reset();
  m_state->linkSource.reset();
  m_state->snapshot = {
      .status = CreateStatus::Creating,
      .kind = receipt->kind(),
      .parent = receipt->destination().parent(),
      .destination = receipt->destination(),
      .error = {},
      .receipt = {}
  };
  State::launch(m_state, [receipt = std::move(receipt)](const CancelToken& token) {
    auto result = receipt->reverse(token);
    return Reply{
        .target = {},
        .destination = result.applied || result.removed ? std::optional(receipt->destination()) : std::nullopt,
        .error = std::move(result.error),
        .linkSource = {},
        .receipt = std::move(result.receipt),
        .removed = result.removed,
        .retireHistory = result.retireHistory
    };
  });
  return true;
}
