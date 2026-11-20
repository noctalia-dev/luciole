#pragma once

#include <string>
#include <vector>

namespace luciole {
  // One real disk partition backing a mountpoint: parsed from /proc/self/mounts
  // with the filesystem label resolved through /dev/disk/by-label. GIO's volume
  // monitor hides internal and system mounts, so the drive list would otherwise
  // miss them even while mounted.
  struct DiskPartition {
    std::string device;     // e.g. /dev/nvme0n1p4
    std::string mountpoint; // e.g. /mnt/Games
    std::string fstype;     // e.g. ext4
    std::string name;       // label, else the mountpoint or device basename
  };

  // Parses mounts-file text (the /proc/self/mounts format: device, mountpoint,
  // fstype, options, ...), keeping real disk partitions except the root
  // filesystem. Labels resolve through `byLabelDir` (/dev/disk/by-label).
  std::vector<DiskPartition> listMountedPartitions(const std::string& mountsText, const std::string& byLabelDir);

  // Whether the block device backs removable media, via sysfs. Best effort:
  // false when the sysfs entry is missing or unreadable.
  bool diskRemovable(const std::string& device);
} // namespace luciole
