#pragma once

#include "fs/directory_entries.h"

namespace luciole {
  // The entry's access facts. Read, write and execute that a local listing deferred are checked now with access(2),
  // the call GIO's local backend makes; a missing native path keeps them unknown.
  FileAccess entryAccess(const DirectoryEntry& entry);
} // namespace luciole
