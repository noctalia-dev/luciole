#include "fs/disk_partitions.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <filesystem>
#include <print>

namespace {
  namespace fs = std::filesystem;

  void parsingAndNaming(const fs::path& labels) {
    fs::create_symlink("/dev/nvme0n1p4", labels / "Games");
    fs::create_symlink("/dev/nvme1n1p2", labels / "Home");
    fs::create_symlink("/dev/mapper/vg-data", labels / "Data Disk");

    const std::string mounts = "/dev/nvme1n1p1 / ext4 rw,relatime 0 0\n"
                               "/dev/nvme0n1p4 /mnt/Games ext4 rw,relatime 0 0\n"
                               "/dev/nvme1n1p2 /home ext4 rw,relatime 0 0\n"
                               "/dev/nvme1n1p3 /boot vfat rw,relatime 0 0\n"
                               "/dev/nvme1n1p4 /boot/efi vfat rw,relatime 0 0\n"
                               "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n"
                               "overlay /run/docker overlay rw 0 0\n"
                               "/dev/sr0 /media/cdrom iso9660 ro 0 0\n"
                               "/dev/mapper/vg-data /mnt/My\\040Data ext4 rw,relatime 0 0\n"
                               "/dev/nvme0n1p4 /mnt/Games ext4 rw,relatime 0 0\n" // Duplicate mountpoint.
                               "proc /proc proc rw 0 0\n";
    const auto partitions = luciole::listMountedPartitions(mounts, labels.native());
    TEST_CHECK(partitions.size() == 4);

    // Labels win over mountpoint names.
    TEST_CHECK(partitions[0].device == "/dev/nvme0n1p4" && partitions[0].name == "Games");
    TEST_CHECK(partitions[0].mountpoint == "/mnt/Games" && partitions[0].fstype == "ext4");
    TEST_CHECK(partitions[1].name == "Home");
    // Unlabeled falls back to the mountpoint basename. Boot partitions are hidden.
    TEST_CHECK(partitions[2].device == "/dev/sr0" && partitions[2].name == "cdrom");
    // Escaped spaces resolve on both sides before matching the label.
    TEST_CHECK(partitions[3].device == "/dev/mapper/vg-data" && partitions[3].name == "Data Disk");
    TEST_CHECK(partitions[3].mountpoint == "/mnt/My Data");
  }

  void filtering() {
    const auto partitions = luciole::listMountedPartitions(
        "UUID=abc / ext4 rw 0 0\n"
        "/dev/sda1 / ext4 rw 0 0\n"          // Root never gets a second row.
        "/dev/sda2 swap swap defaults 0 0\n" // Not a mountpoint with a filesystem.
        "tmpfs /tmp tmpfs rw 0 0\n",
        "/nonexistent-labels"
    );
    TEST_CHECK(partitions.empty());
  }

  void removableIsBestEffort() {
    // Whole disks map to themselves; unknown devices have no sysfs entry.
    TEST_CHECK(!luciole::diskRemovable("/dev/sda"));
    TEST_CHECK(!luciole::diskRemovable("/dev/nonexistent99"));
  }
} // namespace

int main() {
  const luciole::GString temporary(g_dir_make_tmp("luciole-disk-partitions-XXXXXX", nullptr));
  TEST_CHECK(temporary);
  const fs::path root(temporary.get());
  const auto labels = root / "by-label";
  fs::create_directory(labels);
  parsingAndNaming(labels);
  filtering();
  removableIsBestEffort();
  fs::remove_all(root);
  std::println("disk partition checks passed");
}
