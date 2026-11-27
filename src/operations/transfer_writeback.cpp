#include "operations/transfer_writeback.h"

#include "fs/gio_ptr.h"
#include "fs/system_mounts.h"
#include "operations/transfer_copy.h"

#include <algorithm>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <linux/magic.h>
#include <set>
#include <sstream>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <utility>

namespace fs = std::filesystem;

namespace {
  std::string attribute(const fs::path& path) {
    std::ifstream file(path);
    std::string value;
    file >> value;
    return value;
  }

  std::string subsystem(const fs::path& device) {
    std::error_code error;
    auto target = fs::read_symlink(device / "subsystem", error);
    if (error) {
      return {};
    }
    if (!target.has_filename()) {
      target = target.parent_path();
    }
    return target.filename().native();
  }

  bool usbRootHub(const fs::path& device) {
    const auto devnum = attribute(device / "devnum");
    return devnum == "1" || (devnum.empty() && device.filename().native().starts_with("usb"));
  }

  bool removableBlock(const fs::path& link, std::set<fs::path>& visited) {
    std::error_code error;
    auto device = fs::canonical(link, error);
    if (error || !visited.insert(device).second) {
      return false;
    }
    if (fs::exists(device / "partition", error)) {
      device = device.parent_path();
      if (!visited.insert(device).second) {
        return false;
      }
    }
    for (const auto& slave : fs::directory_iterator(device / "slaves", error)) {
      if (removableBlock(slave.path(), visited)) {
        return true;
      }
    }
    for (auto parent = device; parent != parent.root_path() && parent.filename() != "devices";
         parent = parent.parent_path()) {
      const auto removable = attribute(parent / "removable");
      const auto cardType = attribute(parent / "type");
      if (removable == "1" || removable == "removable" || cardType == "SD" || cardType == "SDcombo") {
        return true;
      }
      // USB storage with RMB=0 on ports lacking ACPI metadata reports "unknown"; root hubs also do, so exclude them.
      if (removable == "unknown" && subsystem(parent) == "usb" && !usbRootHub(parent)) {
        return true;
      }
    }
    return false;
  }

  std::string readMountinfo(const std::function<std::string()>& reader) {
    if (reader) {
      return reader();
    }
    std::ifstream mounts("/proc/self/mountinfo");
    std::ostringstream contents;
    contents << mounts.rdbuf();
    return std::move(contents).str();
  }

  std::optional<luciole::transfer::MountInfo>
  resolveMount(int fd, std::optional<std::string>& snapshot, const std::function<std::string()>& reader) {
    struct statx mount{};
    if (::statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_MNT_ID, &mount) != 0
        || !(mount.stx_mask & STATX_MNT_ID)) {
      return {};
    }
    if (snapshot) {
      if (auto entry = luciole::transfer::mountInfo(*snapshot, mount.stx_mnt_id)) {
        return entry;
      }
    }
    snapshot = readMountinfo(reader);
    return luciole::transfer::mountInfo(*snapshot, mount.stx_mnt_id);
  }

  bool fuseMountType(std::string_view type) { return type == "fuse" || type == "fuseblk" || type.starts_with("fuse."); }

  bool isFuseStatfs(int directory) {
    struct statfs fs{};
    return ::fstatfs(directory, &fs) == 0 && fs.f_type == FUSE_SUPER_MAGIC;
  }

  std::size_t statfsPaceLimit(int directory) {
    struct statfs fs{};
    if (::fstatfs(directory, &fs) != 0) {
      return 0;
    }
    if (fs.f_type == static_cast<decltype(fs.f_type)>(CIFS_SUPER_MAGIC)
        || fs.f_type == static_cast<decltype(fs.f_type)>(SMB2_SUPER_MAGIC)) {
      return luciole::transfer::kSmbPaceLimit;
    }
    if (
        fs.f_type == NFS_SUPER_MAGIC
#ifdef AFS_SUPER_MAGIC
        || fs.f_type == AFS_SUPER_MAGIC
#endif
#ifdef SMB_SUPER_MAGIC
        || fs.f_type == SMB_SUPER_MAGIC
#endif
#ifdef NCP_SUPER_MAGIC
        || fs.f_type == NCP_SUPER_MAGIC
#endif
#ifdef CODA_SUPER_MAGIC
        || fs.f_type == CODA_SUPER_MAGIC
#endif
#ifdef V9FS_MAGIC
        || fs.f_type == V9FS_MAGIC
#endif
#ifdef CEPH_SUPER_MAGIC
        || fs.f_type == CEPH_SUPER_MAGIC
#endif
    ) {
      return luciole::transfer::kDefaultPaceLimit;
    }
    return 0;
  }
} // namespace

namespace luciole::transfer {
  std::optional<MountInfo> mountInfo(std::string_view mountinfo, std::uint64_t mountId) {
    const auto prefix = std::to_string(mountId) + " ";
    while (!mountinfo.empty()) {
      const auto end = mountinfo.find('\n');
      const auto line = mountinfo.substr(0, end);
      if (line.starts_with(prefix)) {
        const auto separator = line.find(" - ");
        if (separator == line.npos) {
          return {};
        }
        std::istringstream fields(std::string(line.substr(separator + 3)));
        std::string type, source;
        if (!(fields >> type >> source)) {
          return {};
        }
        std::istringstream ids(std::string(line.substr(prefix.size(), separator - prefix.size())));
        unsigned parent = 0, majorId = 0, minorId = 0;
        char colon = 0;
        if (!(ids >> parent >> majorId >> colon >> minorId) || colon != ':') {
          return {};
        }
        const GString decoded(g_strcompress(source.c_str()));
        return MountInfo{makedev(majorId, minorId), std::move(type), decoded.get()};
      }
      mountinfo.remove_prefix(end == mountinfo.npos ? mountinfo.size() : end + 1);
    }
    return {};
  }

  std::size_t mountPaceLimit(std::string_view type) {
    if (type == "cifs" || type == "smb3") {
      return kSmbPaceLimit;
    }
    return networkFilesystemType(type) || removableFilesystemType(type) || type == "fuse.gvfsd-fuse" ? kDefaultPaceLimit
                                                                                                     : 0;
  }

  bool removableBlockDevice(const fs::path& device) {
    std::set<fs::path> visited;
    return !device.empty() && removableBlock(device, visited);
  }

  void Writeback::pin(Volume& volume, int directory) {
    if (!volume.directory.valid()) {
      volume.directory = checkedFd(::fcntl(directory, F_DUPFD_CLOEXEC, 0));
    }
  }

  Writeback::Volume& Writeback::observe(int directory, dev_t device, bool requireSync) {
    if (const auto alias = m_aliases.find(device); alias != m_aliases.end()) {
      device = alias->second;
    }
    auto it = m_volumes.find(device);
    if (it == m_volumes.end()) {
      dev_t backing = device, filesystem = device;
      std::optional<MountInfo> mount;
      if (major(device) == 0) {
        mount = resolveMount(directory, m_mountinfo, ops.mountinfo);
        if (mount && mount->source.starts_with("/dev/")) {
          filesystem = mount->filesystem;
          struct stat block{};
          if (::stat(mount->source.c_str(), &block) == 0 && S_ISBLK(block.st_mode)) {
            backing = block.st_rdev;
          }
        }
      }
      if (device != filesystem) {
        m_aliases.emplace(device, filesystem);
      }
      auto [entry, inserted] = m_volumes.try_emplace(filesystem);
      if (inserted) {
        entry->second.removable = ops.removable ? ops.removable(directory, device)
                                                : major(backing) != 0
                && removableBlockDevice(std::format("/sys/dev/block/{}:{}", major(backing), minor(backing)));
        if (ops.paceLimit) {
          entry->second.paceLimit = ops.paceLimit(directory, device);
        } else if (entry->second.removable) {
          entry->second.paceLimit = kDefaultPaceLimit;
        } else if (mount) {
          entry->second.paceLimit = mountPaceLimit(mount->type);
        } else if (major(device) == 0) {
          entry->second.paceLimit = statfsPaceLimit(directory);
        }
        // Linux FUSE writeback clears page writeback on queueing, so sync_file_range() cannot wait for daemon writes.
        if (entry->second.paceLimit > 0
            && ((mount && fuseMountType(mount->type)) || (!mount && isFuseStatfs(directory)))) {
          entry->second.rangeSyncUnsupported = true;
        }
      }
      it = entry;
    }
    auto& volume = it->second;
    if (volume.removable || requireSync) {
      pin(volume, directory);
    }
    if (volume.failure) {
      throw FileErrorException(*volume.failure);
    }
    return volume;
  }

  void Writeback::published(Volume& volume, TransferResult& result, bool copiedMove) {
    if (volume.removable || copiedMove) {
      volume.published.push_back(&result);
    }
  }

  void Writeback::markDirty(Volume& volume, TransferResult& result) {
    if (volume.removable && !volume.failure) {
      volume.published.push_back(&result);
    }
  }

  void Writeback::markDirty(dev_t device, TransferResult& result) {
    if (const auto alias = m_aliases.find(device); alias != m_aliases.end()) {
      device = alias->second;
    }
    if (const auto it = m_volumes.find(device); it != m_volumes.end()) {
      markDirty(it->second, result);
    }
  }

  void Writeback::flush(Volume& volume, bool final) {
    if (!volume.failure) {
      if (volume.removable && phase) {
        phase(final ? TransferFlush::Final : TransferFlush::Device);
      }
      try {
        if (ops.sync) {
          ops.sync(volume.directory.get());
        } else {
          synchronize(volume.directory.get());
        }
      } catch (...) {
        volume.failure = currentFileError();
      }
      if (volume.removable && phase && !final) {
        phase(TransferFlush::None);
      }
    }
    if (volume.failure) {
      m_failed = true;
      for (auto* result : volume.published) {
        result->outcome = TransferOutcome::Partial;
        result->retryable = false;
        if (result->error) {
          result->error->kind = volume.failure->kind;
          result->error->message += " " + volume.failure->message;
        } else {
          result->error = volume.failure;
        }
      }
    }
    volume.published.clear();
    if (volume.failure) {
      throw FileErrorException(*volume.failure);
    }
  }

  void Writeback::flushPublished(dev_t device) {
    if (const auto alias = m_aliases.find(device); alias != m_aliases.end()) {
      device = alias->second;
    }
    if (const auto it = m_volumes.find(device); it != m_volumes.end() && !it->second.published.empty()) {
      flush(it->second);
    }
  }

  void Writeback::flushAllPublished() {
    for (auto& [device, volume] : m_volumes) {
      if (!volume.published.empty()) {
        flush(volume);
      }
    }
  }

  bool Writeback::finish() {
    for (auto& [device, volume] : m_volumes) {
      if (!volume.published.empty()) {
        try {
          flush(volume, true);
        } catch (const FileErrorException&) {
          // Continue flushing publications on other filesystems.
        }
      }
    }
    if (phase) {
      phase(TransferFlush::None);
    }
    m_volumes.clear();
    m_mountinfo.reset();
    m_aliases.clear();
    return std::exchange(m_failed, false);
  }
} // namespace luciole::transfer
