#include "operations/create_receipt.h"

#include "fs/background_job.h"
#include "fs/filename.h"
#include "fs/posix_fd.h"
#include "operations/create_service.h"
#include "operations/creation_stage.h"
#include "operations/history_mount.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

struct luciole::CreationIdentity {
  Fd parent, item;
  struct stat stamp;
  bool removed;
};

bool luciole::CreateReceipt::usesMount(const HistoryMount& mount) const {
  return mount.contains(m_identity->parent.get()) || mount.contains(m_identity->item.get());
}

namespace {
  std::atomic<std::size_t> pendingCreateCleanup = 0;
  using luciole::FileErrorKind;
  struct stat namedStamp(int parent, const std::string& name) {
    struct stat stamp{};
    if (::fstatat(parent, name.c_str(), &stamp, AT_SYMLINK_NOFOLLOW) != 0) {
      luciole::throwErrno(errno);
    }
    return stamp;
  }
  luciole::Fd currentParent(const luciole::Location& destination, int held) {
    const auto parent = destination.parent();
    if (!parent || !parent->nativePath()) {
      luciole::throwFileError(FileErrorKind::InvalidArgument, "Creation history requires a native folder.");
    }
    auto current = luciole::checkedFd(::open(parent->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!luciole::sameIdentity(luciole::statFd(current.get()), luciole::statFd(held))) {
      luciole::throwFileError(
          FileErrorKind::Changed, "The containing folder changed. Creation history is no longer valid."
      );
    }
    return current;
  }
  void requireRevision(int item, const struct stat& expected) {
    if (!luciole::sameFileRevision(luciole::statFd(item), expected)) {
      luciole::throwFileError(FileErrorKind::Changed, "The created item changed. Creation history is no longer valid.");
    }
  }
  void checkCancelled(const luciole::CancelToken& token) {
    if (token.cancelled()) {
      luciole::throwFileError(FileErrorKind::Cancelled, "Creation reversal cancelled.");
    }
  }
  std::shared_ptr<const luciole::CreateReceipt>
  captureInverse(luciole::CreateKind kind, const luciole::Location& destination, int parent, int item, bool removed) {
    try {
      return luciole::CreateReceipt::capture(kind, destination, parent, item, removed);
    } catch (const luciole::FileErrorException&) {
      // Completed effects remain successful when optional history capture fails.
      return {};
    }
  }
  std::optional<luciole::FileError> cleanup(luciole::CreationStage& stage) {
    if (stage.clear()) {
      return {};
    }
    return luciole::FileError{
        .kind = FileErrorKind::Other,
        .message = "The operation completed, but its private folder could not be removed: "
            + luciole::filenameDisplayText(stage.recoveryPath())
    };
  }
  luciole::CreateReplayResult undo(
      luciole::CreateKind kind, const luciole::Location& destination, const luciole::CreationIdentity& proof,
      const luciole::CancelToken& token
  ) {
    const auto parent = currentParent(destination, proof.parent.get());
    const auto name = *destination.basename();
    requireRevision(proof.item.get(), proof.stamp);
    if (!luciole::sameFileRevision(namedStamp(parent.get(), name), proof.stamp)) {
      luciole::throwFileError(
          FileErrorKind::Changed, "The created item was replaced. Creation history is no longer valid."
      );
    }
    checkCancelled(token);
    luciole::CreationStage stage(parent.get(), false);
    stage.adopt(luciole::checkedFd(::fcntl(proof.item.get(), F_DUPFD_CLOEXEC, 0)));
    (void)currentParent(destination, parent.get());
    requireRevision(proof.item.get(), proof.stamp);
    checkCancelled(token);
    if (::syscall(SYS_renameat2, parent.get(), name.c_str(), stage.fd(), "item", RENAME_NOREPLACE) != 0) {
      luciole::throwErrno(errno);
    }
    try {
      const auto moved = luciole::statFd(proof.item.get());
      auto expected = proof.stamp;
      expected.st_ctim = moved.st_ctim;
      if (!luciole::sameFileRevision(expected, moved)
          || !luciole::sameFileRevision(moved, namedStamp(stage.fd(), "item"))) {
        luciole::throwFileError(FileErrorKind::Changed, "The item changed during creation reversal.");
      }
      checkCancelled(token);
      stage.removeItem();
    } catch (const std::exception&) {
      auto error = luciole::currentFileError();
      bool restored = false;
      try {
        // Restore even an intervening entry, without deleting it or replacing a new occupant.
        stage.adopt(luciole::checkedFd(::openat(stage.fd(), "item", O_PATH | O_NOFOLLOW | O_CLOEXEC)));
        (void)currentParent(destination, parent.get());
        stage.publish(name);
        restored = true;
      } catch (const std::exception&) {
        error.message +=
            " Recovery could not be completed; inspect " + luciole::filenameDisplayText(stage.recoveryPath());
      }
      return {.receipt = {}, .error = std::move(error), .applied = false, .removed = !restored, .retireHistory = true};
    }
    auto inverse = captureInverse(kind, destination, parent.get(), proof.item.get(), true);
    auto error = cleanup(stage);
    return {
        .receipt = error ? nullptr : std::move(inverse),
        .error = std::move(error),
        .applied = true,
        .removed = true,
        .retireHistory = false
    };
  }
  std::string symlinkText(int item) {
    std::array<char, 8192> buffer{};
    const auto count = ::readlinkat(item, "", buffer.data(), buffer.size());
    if (count < 0) {
      luciole::throwErrno(errno);
    }
    if (static_cast<std::size_t>(count) == buffer.size()) {
      luciole::throwFileError(FileErrorKind::NameTooLong, "The retained link target exceeds the history limit.");
    }
    return {buffer.data(), static_cast<std::size_t>(count)};
  }
  luciole::CreateReplayResult redo(
      luciole::CreateKind kind, const luciole::Location& destination, const luciole::CreationIdentity& proof,
      const luciole::CancelToken& token
  ) {
    using K = luciole::CreateKind;
    const auto parent = currentParent(destination, proof.parent.get());
    const auto name = *destination.basename();
    requireRevision(proof.item.get(), proof.stamp);
    struct stat occupant{};
    if (::fstatat(parent.get(), name.c_str(), &occupant, AT_SYMLINK_NOFOLLOW) == 0) {
      luciole::throwErrno(EEXIST);
    }
    if (errno != ENOENT) {
      luciole::throwErrno(errno);
    }
    checkCancelled(token);
    if (kind == K::Folder) {
      if (::mkdirat(parent.get(), name.c_str(), proof.stamp.st_mode & 07777) != 0) {
        luciole::throwErrno(errno);
      }
      try {
        const auto item =
            luciole::checkedFd(::openat(parent.get(), name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (::chmod(("/proc/self/fd/" + std::to_string(item.get())).c_str(), proof.stamp.st_mode & 07777) != 0) {
          luciole::throwErrno(errno);
        }
        return {
            .receipt = captureInverse(kind, destination, parent.get(), item.get(), false),
            .error = {},
            .applied = true,
            .removed = false,
            .retireHistory = false
        };
      } catch (const std::exception&) {
        auto error = luciole::currentFileError();
        error.message = "The folder was created, but its mode/history could not be restored: " + error.message;
        return {.receipt = {}, .error = std::move(error), .applied = true, .removed = false, .retireHistory = false};
      }
    }
    luciole::CreationStage stage(parent.get());
    bool linked = false;
    try {
      if (kind == K::SymbolicLink) {
        if (::symlinkat(symlinkText(proof.item.get()).c_str(), stage.fd(), "item") != 0) {
          luciole::throwErrno(errno);
        }
        stage.adopt(luciole::checkedFd(::openat(stage.fd(), "item", O_PATH | O_NOFOLLOW | O_CLOEXEC)));
      } else if (kind == K::HardLink) {
        const auto held = "/proc/self/fd/" + std::to_string(proof.item.get());
        if (::linkat(AT_FDCWD, held.c_str(), stage.fd(), "item", AT_SYMLINK_FOLLOW) != 0) {
          luciole::throwErrno(errno);
        }
        linked = true;
        stage.adopt(luciole::checkedFd(::openat(stage.fd(), "item", O_PATH | O_NOFOLLOW | O_CLOEXEC)));
      } else {
        stage.adopt(
            luciole::checkedFd(::openat(stage.fd(), "item", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600))
        );
        luciole::copyCreationData(
            proof.item.get(), stage.item(), static_cast<std::uint64_t>(proof.stamp.st_size), token
        );
      }
      if ((kind == K::EmptyFile || kind == K::Document) && ::fchmod(stage.item(), proof.stamp.st_mode & 07777) != 0) {
        luciole::throwErrno(errno);
      }
      auto expected = proof.stamp;
      if (linked) {
        const auto current = luciole::statFd(proof.item.get());
        expected.st_ctim = current.st_ctim;
        ++expected.st_nlink;
        requireRevision(proof.item.get(), expected);
        if (!luciole::sameIdentity(current, luciole::statFd(stage.item()))) {
          luciole::throwFileError(FileErrorKind::Changed, "The retained link source changed.");
        }
      } else {
        requireRevision(proof.item.get(), expected);
      }
      (void)currentParent(destination, parent.get());
      checkCancelled(token);
      stage.publish(name);
    } catch (const std::exception&) {
      auto error = luciole::currentFileError();
      if (!stage.clear()) {
        error.message +=
            " Recovery could not be completed; inspect " + luciole::filenameDisplayText(stage.recoveryPath());
      }
      return {.receipt = {}, .error = std::move(error), .applied = false, .removed = false, .retireHistory = linked};
    }
    auto inverse = captureInverse(kind, destination, parent.get(), stage.item(), false);
    auto error = cleanup(stage);
    return {
        .receipt = error ? nullptr : std::move(inverse),
        .error = std::move(error),
        .applied = true,
        .removed = false,
        .retireHistory = false
    };
  }
} // namespace

bool luciole::CreateReceipt::cleanupPending() { return pendingCreateCleanup != 0; }

luciole::CreateReceipt::CreateReceipt(
    CreateKind kind, Location destination, std::shared_ptr<const CreationIdentity> identity
)
    : m_kind(kind), m_destination(std::move(destination)), m_identity(std::move(identity)) {}

std::shared_ptr<const luciole::CreateReceipt>
luciole::CreateReceipt::capture(CreateKind kind, const Location& destination, int parent, int item, bool removed) {
  (void)currentParent(destination, parent);
  const auto stamp = statFd(item);
  if (!removed && !sameFileRevision(stamp, namedStamp(parent, *destination.basename()))) {
    throwFileError(FileErrorKind::Changed, "The created item changed before history was captured.");
  }
  if ((kind == CreateKind::Folder && !S_ISDIR(stamp.st_mode))
      || ((kind == CreateKind::EmptyFile || kind == CreateKind::Document || kind == CreateKind::HardLink)
          && !S_ISREG(stamp.st_mode))
      || (kind == CreateKind::SymbolicLink && !S_ISLNK(stamp.st_mode))) {
    throwFileError(FileErrorKind::Changed, "The created item's type changed.");
  }
  auto identity = std::make_unique<CreationIdentity>(
      checkedFd(::fcntl(parent, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(item, F_DUPFD_CLOEXEC, 0)), stamp, removed
  );
  std::shared_ptr<const CreationIdentity> proof(identity.release(), [](const CreationIdentity* retired) {
    // Final close can free a large unlinked document; keep that filesystem work off the UI thread.
    ++pendingCreateCleanup;
    detail::runInThread(
        [owned = std::unique_ptr<const CreationIdentity>(retired)] mutable { owned.reset(); },
        [] { --pendingCreateCleanup; }
    );
  });
  return std::make_shared<const CreateReceipt>(kind, destination, std::move(proof));
}

luciole::CreateReplayResult luciole::CreateReceipt::reverse(const CancelToken& token) const {
  if (!m_identity) {
    throwFileError(FileErrorKind::InvalidArgument, "Invalid creation history entry.");
  }
  return m_identity->removed ? redo(m_kind, m_destination, *m_identity, token)
                             : undo(m_kind, m_destination, *m_identity, token);
}
