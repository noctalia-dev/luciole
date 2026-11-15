#pragma once

#include "fs/directory_entries.h"
#include "fs/file_error.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  enum class DirectoryStatus : std::uint8_t { Idle, Loading, Ready, Error, Cancelled };

  struct DirectorySnapshot {
    std::uint64_t generation = 0;
    std::optional<Location> location;
    DirectoryStatus status = DirectoryStatus::Idle;
    DirectoryEntries entries;
    std::optional<FileError> error;
    std::optional<FileError> monitorError;
    DirectoryMetadata metadata{};
    bool metadataLoading = false;
    std::optional<FileError> accessError = std::nullopt;
    std::optional<FileError> filesystemError = std::nullopt;
    bool incrementalResults = false; // Fresh current-scan batches, rather than retained refresh entries.
    // Some local entries still carry a name-based content type; sniffed types follow as further publications.
    bool contentTypesPending = false;
  };

  // Main/default-context service. Observers run there and must not throw.
  // IDs last for this session; they are neither inodes nor persistent bookmarks.
  class DirectorySession {
  public:
    // Optional consumer-owned IDs allow several immutable sessions to share one projection.
    using IdResolver = std::function<std::uint64_t(const DirectoryEntry&)>;
    explicit DirectorySession(std::size_t entryLimit = 0, IdResolver resolveId = {});
    ~DirectorySession();
    DirectorySession(const DirectorySession&) = delete;
    DirectorySession& operator=(const DirectorySession&) = delete;
    DirectorySession(DirectorySession&&) = delete;
    DirectorySession& operator=(DirectorySession&&) = delete;

    void open(const Location& location);
    void refresh();
    void cancel();
    void setChangedCallback(std::function<void()> callback);
    const DirectorySnapshot& snapshot() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
