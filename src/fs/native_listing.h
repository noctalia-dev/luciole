#pragma once

#include "fs/directory_entries.h"

#include <cstddef>
#include <cstdint>
#include <gio/gio.h>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  struct NativeListing {
    std::vector<DirectoryEntry> entries; // IDs are 0; the session assigns them on the main thread.
    bool unsupported = false;            // Use GIO's enumeration instead; it also reports the folder's errors.
    bool limited = false;                // entryLimit was reached.
    // Entries whose content type comes from the name although GIO would sniff the contents; see
    // sniffContentType().
    std::vector<std::uint32_t> uncertain;
  };

  // Lists a local folder with readdir and one statx per entry. Worker threads only.
  // Every fact equals what GIO's local backend reports for the session's attributes with NOFOLLOW_SYMLINKS, except
  // access read/write/execute, which are deferred to entryAccess(), and the content type of `uncertain` entries.
  // Network and FUSE filesystems, unreadable entries and any other failure report unsupported. A cancelled listing
  // is empty.
  NativeListing listNativeFolder(const std::string& path, std::size_t entryLimit, GCancellable* cancellable);
  // GIO's content type of a local file, sniffing its contents when the name is not decisive; nullopt when GIO
  // reports none. Blocking: worker threads only.
  std::optional<std::string> sniffContentType(const std::string& path);
} // namespace luciole
