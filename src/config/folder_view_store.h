#pragma once

#include "config/diagnostic.h"
#include "fs/atomic_file.h"
#include "model/directory_options.h"

#include <filesystem>
#include <string>
#include <vector>

namespace luciole {
  struct FolderViewEntry {
    std::string uri;
    FolderView view;
    bool operator==(const FolderViewEntry&) const = default;
  };

  // Per-folder sort and view mode in folder_views.toml, least recently changed first. Loading skips invalid
  // entries and reports them; an unreadable or unparsable file loads as empty and the next save replaces it.
  // Not thread-safe: use one instance from one thread at a time.
  class FolderViewStore {
  public:
    static constexpr std::size_t limit = 4096;
    explicit FolderViewStore(std::filesystem::path path);
    static std::filesystem::path pathFromEnvironment();
    void load();
    // Keeps the last `limit` entries.
    AtomicWriteState save(std::vector<FolderViewEntry> entries);
    const std::vector<FolderViewEntry>& entries() const { return m_entries; }
    const ConfigIssues& issues() const { return m_issues; }

  private:
    std::filesystem::path m_path;
    std::vector<FolderViewEntry> m_entries;
    ConfigIssues m_issues;
  };
} // namespace luciole
