#include "operations/permission_target.h"

#include "fs/gio_interop.h"
#include "operations/permission_accounts.h"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
  [[noreturn]] void changed(const char* message) { luciole::throwFileError(luciole::FileErrorKind::Changed, message); }
  luciole::FileType typeFromMode(mode_t mode) {
    return S_ISDIR(mode) ? luciole::FileType::Directory
        : S_ISLNK(mode)  ? luciole::FileType::Symlink
        : S_ISREG(mode)  ? luciole::FileType::Regular
                         : luciole::FileType::Special;
  }
} // namespace

bool luciole::samePermissions(const struct stat& a, const struct stat& b) {
  return a.st_uid == b.st_uid && a.st_gid == b.st_gid && (a.st_mode & 07777) == (b.st_mode & 07777);
}

luciole::PermissionTarget::PermissionTarget(DirectoryEntry entry)
    : entry(std::move(entry)), path(*this->entry.location().nativePath()),
      fd(checkedFd(::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC))), baseline(statFd(fd.get())) {}

luciole::PermissionTarget::PermissionTarget(
    DirectoryEntry entry, int descriptor, std::vector<std::shared_ptr<PermissionTarget>> ancestors
)
    : entry(std::move(entry)), path(*this->entry.location().nativePath()), fd(checkedFd(descriptor)),
      baseline(statFd(fd.get())), identified(true), ancestors(std::move(ancestors)) {
  this->entry.type = typeFromMode(baseline.st_mode);
  this->entry.symlink = S_ISLNK(baseline.st_mode);
  this->entry.setIdentity(std::nullopt); // Identified directly by the held native descriptor.
}

bool luciole::PermissionTarget::current() const {
  struct stat value{};
  return ::lstat(path.c_str(), &value) == 0 && sameIdentity(value, baseline);
}

void luciole::PermissionTarget::validate() const {
  for (const auto& ancestor : ancestors) {
    if (!ancestor->current() || !samePermissions(statFd(ancestor->fd.get()), ancestor->baseline)) {
      changed("A containing folder changed. Reopen Properties.");
    }
  }
  if (!identified) {
    throwFileError(FileErrorKind::Unsupported, "The captured item has no provider identity; editing is unavailable.");
  }
  if (!current()) {
    changed("The item moved or was replaced. Reopen Properties.");
  }
  if (!samePermissions(statFd(fd.get()), baseline)) {
    changed("Permissions or ownership changed elsewhere. Reopen Properties.");
  }
  if (aclBaseline && aclBaseline->supported && !sameAcl(inspectAcl(fd.get()), *aclBaseline)) {
    changed("Access control changed elsewhere. Reopen Properties.");
  }
}

luciole::PermissionInfo luciole::PermissionTarget::inspect(bool names) const {
  const auto value = statFd(fd.get());
  const bool root = ::geteuid() == 0, owned = value.st_uid == ::geteuid();
  struct statvfs filesystem{};
  const bool readOnly = ::statvfs(("/proc/self/fd/" + std::to_string(fd.get())).c_str(), &filesystem) == 0
      && (filesystem.f_flag & ST_RDONLY) != 0;
  const bool valid = current(), editable = identified && valid && !readOnly;
  PermissionInfo info{
      .mode = value.st_mode & 07777,
      .owner = value.st_uid,
      .group = value.st_gid,
      .ownerName = {},
      .groupName = {},
      .canMode = editable && (root || owned) && !S_ISLNK(value.st_mode),
      .canOwner = editable && root,
      .canGroup = editable && (root || owned),
      .symlink = S_ISLNK(value.st_mode),
      .multipleLinks = S_ISREG(value.st_mode) && value.st_nlink > 1,
      .current = valid,
      .directory = S_ISDIR(value.st_mode),
      .acl = aclView,
      .reason = {}
  };
  // Display names are optional; unavailable account services must not hide known IDs/mode.
  try {
    if (names) {
      info.ownerName = accountName(info.owner, AccountKind::User);
    }
  } catch (const std::exception&) {
  }
  try {
    if (names) {
      info.groupName = accountName(info.group, AccountKind::Group);
    }
  } catch (const std::exception&) {
  }
  if (!valid) {
    info.reason = "The item moved or was replaced. Reopen Properties.";
  } else if (!identified) {
    info.reason = "This provider does not identify the captured item; editing is unavailable.";
  } else if (readOnly) {
    info.reason = "This filesystem is read-only.";
  } else if (!root && !owned) {
    info.reason = "Only the owner or an administrator can change these permissions.";
  } else if (info.symlink) {
    info.reason = "Link permissions cannot be edited. Ownership applies to the link itself.";
  }
  return info;
}

void luciole::PermissionTarget::loadAcl(bool adopt) {
  auto value = inspectAcl(fd.get());
  const auto basic = inspect(false);
  value.editable = value.supported
      && basic.canMode
      && samePermissions(statFd(fd.get()), baseline)
      && (adopt || (aclBaseline && sameAcl(value, *aclBaseline)));
  if (value.supported && !value.editable) {
    value.message = basic.reason.empty() ? "Access control changed. Reopen Properties." : basic.reason;
  }
  if (adopt) {
    aclBaseline = value;
  }
  aclView = std::make_shared<const AclInfo>(std::move(value));
}

std::shared_ptr<luciole::PermissionTarget>
luciole::preparePermissionTarget(const DirectoryEntry& entry, bool captureCurrentLocation) {
  const auto location = entry.location();
  if (!location.nativePath() || location.isTrash()) {
    throwFileError(FileErrorKind::Unsupported, "Permission editing is available for native items outside Trash.");
  }
  auto target = std::make_shared<PermissionTarget>(entry);
  const GioPtr<GFileInfo> info(g_file_query_info(
      gioFile(location).get(), "id::file,id::filesystem,unix::device,unix::inode", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      nullptr, nullptr
  ));
  const auto identity = entry.identity();
  if (!info || !target->current() || (identity && readFileIdentity(info.get()) != identity)) {
    changed("The item changed. Reopen Properties.");
  }
  if (g_file_info_has_attribute(info.get(), "unix::device")
      && g_file_info_has_attribute(info.get(), "unix::inode")
      && (g_file_info_get_attribute_uint32(info.get(), "unix::device") != target->baseline.st_dev
          || g_file_info_get_attribute_uint64(info.get(), "unix::inode") != target->baseline.st_ino)) {
    changed("The item changed. Reopen Properties.");
  }
  target->identified = identity.has_value() || (captureCurrentLocation && readFileIdentity(info.get()).has_value());
  return target;
}
