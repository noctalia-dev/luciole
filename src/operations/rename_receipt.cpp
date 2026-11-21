#include "operations/rename_receipt.h"

#include "fs/file_error.h"
#include "fs/posix_fd.h"
#include "operations/history_mount.h"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <utility>

struct luciole::RenameIdentity {
  Fd parent, item;
  struct stat stamp;
};

bool luciole::RenameReceipt::usesMount(const HistoryMount& mount) const {
  return mount.contains(m_identity->parent.get()) || mount.contains(m_identity->item.get());
}

namespace {
  std::pair<std::string, std::string> names(const luciole::Location& source, const luciole::Location& destination) {
    if (!source.isNative()
        || !destination.isNative()
        || source.parent() != destination.parent()
        || source == destination
        || source.isTrash()
        || destination.isTrash()) {
      luciole::throwFileError(luciole::FileErrorKind::InvalidArgument, "Invalid rename history entry.");
    }
    const auto before = std::filesystem::path(*source.nativePath()).filename().native();
    const auto after = std::filesystem::path(*destination.nativePath()).filename().native();
    if (before.empty() || before == "." || before == ".." || after.empty() || after == "." || after == "..") {
      luciole::throwFileError(luciole::FileErrorKind::InvalidArgument, "Invalid rename history name.");
    }
    return {before, after};
  }
  struct stat namedStamp(int parent, const std::string& name) {
    struct stat stamp{};
    if (::fstatat(parent, name.c_str(), &stamp, AT_SYMLINK_NOFOLLOW) < 0) {
      luciole::throwErrno(errno);
    }
    return stamp;
  }
} // namespace

luciole::RenameReceipt::RenameReceipt(
    Location source, Location destination, std::shared_ptr<const RenameIdentity> identity
)
    : m_source(std::move(source)), m_destination(std::move(destination)), m_identity(std::move(identity)) {}

std::shared_ptr<const luciole::RenameReceipt>
luciole::RenameReceipt::capture(const Location& source, const Location& destination, int parent, int item) {
  const auto [before, after] = names(source, destination);
  const auto stamp = statFd(item);
  if (!sameFileRevision(stamp, namedStamp(parent, after))) {
    throwFileError(FileErrorKind::Changed, "The renamed item changed before history was captured.");
  }
  const auto currentParent =
      checkedFd(::open(source.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!sameIdentity(statFd(parent), statFd(currentParent.get()))) {
    throwFileError(FileErrorKind::Changed, "The containing folder changed before history was captured.");
  }
  auto identity = std::make_shared<RenameIdentity>(
      checkedFd(::fcntl(parent, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(item, F_DUPFD_CLOEXEC, 0)), stamp
  );
  return std::make_shared<RenameReceipt>(source, destination, std::move(identity));
}

std::shared_ptr<const luciole::RenameReceipt> luciole::RenameReceipt::reverse(const CancelToken& token) const {
  if (!m_identity) {
    throwFileError(FileErrorKind::InvalidArgument, "Invalid rename history entry.");
  }
  const auto [before, after] = names(m_source, m_destination);
  const auto parent =
      checkedFd(::open(m_destination.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!sameIdentity(statFd(parent.get()), statFd(m_identity->parent.get()))) {
    throwFileError(FileErrorKind::Changed, "The containing folder changed. Rename history is no longer valid.");
  }
  if (!sameFileRevision(m_identity->stamp, statFd(m_identity->item.get()))
      || !sameFileRevision(m_identity->stamp, namedStamp(parent.get(), after))) {
    throwFileError(FileErrorKind::Changed, "The renamed item changed. Rename history is no longer valid.");
  }
  if (token.cancelled()) {
    throwFileError(FileErrorKind::Cancelled, "Rename reversal cancelled.");
  }
  if (luciole::renameNoReplace(parent.get(), after.c_str(), parent.get(), before.c_str()) != 0) {
    throwErrno(errno);
  }
  try {
    return capture(m_destination, m_source, parent.get(), m_identity->item.get());
  } catch (const FileErrorException&) {
    // The rename already committed. Failure to capture history must not report it as an unapplied operation.
    return {};
  }
}
