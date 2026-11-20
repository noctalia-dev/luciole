#include "fs/disk_partitions.h"

#include <algorithm>
#include <array>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  struct DirCloser {
    void operator()(DIR* dir) const noexcept {
      if (dir != nullptr) {
        (void)closedir(dir);
      }
    }
  };

  // Real on-disk filesystems worth a Drives row. Virtual, pseudo and stacked
  // filesystems (tmpfs, overlay, squashfs, zfs, ...) never qualify.
  bool realFilesystem(std::string_view fstype) {
    for (const auto* known :
         {"ext2", "ext3", "ext4", "vfat", "exfat", "ntfs", "ntfs3", "btrfs", "xfs", "f2fs", "hfsplus", "udf",
          "iso9660"}) {
      if (fstype == known) {
        return true;
      }
    }
    return false;
  }

  // /proc/self/mounts escapes spaces, tabs, newlines and backslashes in paths.
  std::string unescape(std::string_view field) {
    std::string result;
    result.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
      if (field[i] == '\\') {
        int value = 0;
        std::size_t digits = 0;
        while (digits < 3 && i + 1 + digits < field.size()) {
          const char digit = field[i + 1 + digits];
          if (digit < '0' || digit > '7') {
            break;
          }
          value = value * 8 + (digit - '0');
          ++digits;
        }
        if (digits == 3) {
          result.push_back(static_cast<char>(value));
          i += 3;
          continue;
        }
      }
      result.push_back(field[i]);
    }
    return result;
  }

  std::string_view basenameOf(std::string_view path) {
    if (const auto slash = path.find_last_of('/'); slash != std::string_view::npos) {
      return path.substr(slash + 1);
    }
    return path;
  }

  std::string deviceBasename(const std::string& device) { return std::string(basenameOf(device)); }

  // Label for `device` (e.g. /dev/nvme0n1p4) from the by-label directory, whose
  // entries are labels symlinking at the device nodes.
  std::string partitionLabel(const std::string& byLabelDir, const std::string& device) {
    const std::string wanted = deviceBasename(device);
    std::unique_ptr<DIR, DirCloser> dir(opendir(byLabelDir.c_str()));
    if (!dir) {
      return {};
    }
    std::array<char, 4096> target{};
    while (const auto* item = readdir(dir.get())) {
      const std::string_view name(item->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      const std::string link = byLabelDir + '/' + std::string(name);
      const auto length = readlink(link.c_str(), target.data(), target.size() - 1);
      if (length <= 0) {
        continue;
      }
      if (basenameOf(std::string_view(target.data(), static_cast<std::size_t>(length))) == wanted) {
        return std::string(name);
      }
    }
    return {};
  }

  std::string displayName(const std::string& byLabelDir, const std::string& device, const std::string& mountpoint) {
    if (auto label = partitionLabel(byLabelDir, device); !label.empty()) {
      return label;
    }
    if (const auto base = basenameOf(mountpoint); !base.empty()) {
      return std::string(base);
    }
    return deviceBasename(device);
  }

  // Parent disk of a partition node for the sysfs removable check:
  // sda1 -> sda, nvme0n1p4 -> nvme0n1, mmcblk0p2 -> mmcblk0.
  std::string parentDisk(const std::string& device) {
    auto disk = deviceBasename(device);
    while (!disk.empty() && disk.back() >= '0' && disk.back() <= '9') {
      disk.pop_back();
    }
    if (!disk.empty() && disk.back() == 'p') {
      disk.pop_back();
    }
    return disk;
  }
} // namespace

std::vector<luciole::DiskPartition>
luciole::listMountedPartitions(const std::string& mountsText, const std::string& byLabelDir) {
  std::vector<DiskPartition> result;
  std::size_t start = 0;
  while (start <= mountsText.size()) {
    const auto end = mountsText.find('\n', start);
    const std::string_view line(
        mountsText.data() + start, end == std::string::npos ? mountsText.size() - start : end - start
    );
    start = end == std::string::npos ? mountsText.size() + 1 : end + 1;
    // device, mountpoint, fstype, options, ... (no literal spaces: escaped).
    std::string_view fields[4];
    std::size_t count = 0, field = 0;
    for (std::size_t i = 0; i <= line.size() && count < 4; ++i) {
      if (i == line.size() || line[i] == ' ' || line[i] == '\t') {
        fields[count++] = line.substr(field, i - field);
        while (i + 1 < line.size() && (line[i + 1] == ' ' || line[i + 1] == '\t')) {
          ++i;
        }
        field = i + 1;
      }
    }
    if (count < 4) {
      continue;
    }
    const std::string device = unescape(fields[0]);
    const std::string mountpoint = unescape(fields[1]);
    const std::string fstype(fields[2]);
    if (!device.starts_with("/dev/") || !realFilesystem(fstype) || mountpoint == "/" || mountpoint.empty()) {
      continue;
    }
    // The boot partitions carry no user data; nobody wants them in Drives.
    if (mountpoint == "/boot" || mountpoint == "/boot/efi") {
      continue;
    }
    if (std::ranges::any_of(result, [&](const DiskPartition& known) { return known.mountpoint == mountpoint; })) {
      continue;
    }
    result.push_back({
        .device = device,
        .mountpoint = mountpoint,
        .fstype = fstype,
        .name = displayName(byLabelDir, device, mountpoint),
    });
  }
  return result;
}

bool luciole::diskRemovable(const std::string& device) {
  const auto disk = parentDisk(device);
  if (disk.empty() || disk == deviceBasename(device)) {
    return false;
  }
  const std::string path = "/sys/block/" + disk + "/removable";
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  char value = 0;
  const auto count = ::read(fd, &value, 1);
  ::close(fd);
  return count == 1 && value == '1';
}
