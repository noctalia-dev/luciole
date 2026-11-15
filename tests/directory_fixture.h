#pragma once

#include "fs/directory_session.h"

#include <utility>
#include <vector>

namespace test {
  // Edit fixture data before publishing a new immutable snapshot.
  template <class Edit> void editEntries(luciole::DirectorySnapshot& snapshot, Edit edit) {
    std::vector<luciole::DirectoryEntry> entries(snapshot.entries.begin(), snapshot.entries.end());
    edit(entries);
    snapshot.entries = std::move(entries);
  }
} // namespace test
