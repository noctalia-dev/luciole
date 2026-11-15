#include "fs/entry_access.h"

#include <fcntl.h>
#include <unistd.h>

luciole::FileAccess luciole::entryAccess(const DirectoryEntry& entry) {
  auto access = entry.access;
  if (!entry.accessDeferred) {
    return access;
  }
  const auto path = entry.location().nativePath();
  if (!path) {
    return access;
  }
  access.read = ::faccessat(AT_FDCWD, path->c_str(), R_OK, 0) == 0;
  access.write = ::faccessat(AT_FDCWD, path->c_str(), W_OK, 0) == 0;
  access.execute = ::faccessat(AT_FDCWD, path->c_str(), X_OK, 0) == 0;
  return access;
}
