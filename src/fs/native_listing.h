#pragma once

#include "fs/directory_entries.h"

#include <cstddef>
#include <cstdint>
#include <gio/gio.h>
#include <optional>
#include <span>
#include <string>
#include <sys/statfs.h>
#include <sys/types.h>
#include <vector>

namespace luciole {
  struct NativeListing {
    std::vector<DirectoryEntry> entries; // IDs are 0; the session assigns them on the main thread.
    bool unsupported = false;            // Use GIO's enumeration instead; it also reports the folder's errors.
    bool limited = false;                // entryLimit was reached.
    bool network = false;                // True when the folder sits on a mounted network filesystem.
    // Entries whose content type comes from the name although GIO would sniff the contents; see
    // sniffContentType().
    std::vector<std::uint32_t> uncertain;
    std::optional<BrowseCatalogue> catalogue;
  };

  // Lists a local folder on a worker. The eager path stats each entry; a large browse catalogue reads names and
  // dirent kinds, with statx only for missing kinds and mount-root lost+found hiddenness.
  // Every fact equals what GIO's local backend reports for the session's attributes with NOFOLLOW_SYMLINKS, except
  // access read/write/execute, which are deferred to entryAccess(), and the content type of `uncertain` entries.
  // Network filesystems use the browse catalogue above 64 entries when dirent kinds are present, and otherwise report
  // unsupported with `network = true` so GIO enumerates them without deferred main-thread access checks. Unreadable
  // entries, non-network FUSE filesystems, and other failures report unsupported. A cancelled listing is empty.
  NativeListing
  listNativeFolder(const std::string& path, std::size_t entryLimit, GCancellable* cancellable, bool catalogue = false);
  struct CatalogueValidation {
    bool parentChanged = false;
    bool unavailable = false;
    std::vector<std::size_t> changed;
  };
  // Recheck dirent continuity after a row batch without treating d_ino as a provider identity. This also works
  // for child mount points, whose actual inode can differ from their parent directory's d_ino.
  CatalogueValidation
  validateCatalogueRows(const BrowseCatalogue& catalogue, std::span<const std::size_t> rows, GCancellable* cancellable);
  // Whether reading the file or folder at path may go over a network: network filesystems and FUSE mounts without a
  // local disk, such as rclone and sshfs. True when the filesystem cannot be determined. Blocking: worker threads only.
  bool networkFilesystem(const std::string& path);
  // The same decision from a path's statfs result and the st_dev of its stat result.
  bool networkFilesystem(const struct statfs& filesystem, dev_t device);
  // GIO's content type of a local file, sniffing its contents when the name is not decisive; nullopt when GIO
  // reports none. Blocking: worker threads only.
  std::optional<std::string> sniffContentType(const std::string& path);
  // Whether the link at path leads to a folder; false for a broken or unreadable link. It follows the link, so an
  // unresponsive mount can block it without end and cancellation cannot interrupt it. Worker threads only.
  bool linksToDirectory(const std::string& path);
} // namespace luciole
