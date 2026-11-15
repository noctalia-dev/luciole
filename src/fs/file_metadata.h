#pragma once

#include "fs/interned_text.h"

#include <cstdint>
#include <optional>
#include <string>

namespace luciole {
  enum class FileType : std::uint8_t { Unknown, Regular, Directory, Symlink, Special, Shortcut, Mountable };

  // Shared presentation for list, tooltip, preview and Properties. Does not inspect file contents.
  std::string fileTypeDescription(FileType type, bool symlink, InternedText contentType);
  // MIME inheritance includes application-based text formats; image/media renderers take precedence.
  bool contentTypeIsText(const std::string& contentType);

  // Provider-reported access facts, not promises that an operation will succeed.
  // nullopt is unknown; false is a reported negative, not a missing attribute.
  struct FileIdentity {
    std::string filesystem;
    std::string file;
    bool operator==(const FileIdentity&) const = default;
  };

  // A provider's change time (ctime), with subsecond precision; detects edits that restore the modification time.
  struct ChangeRevision {
    std::uint64_t seconds = 0;
    std::uint32_t nanoseconds = 0;
    bool operator==(const ChangeRevision&) const = default;
    // "<seconds>:<nanoseconds>", for cache keys and persisted records.
    std::string text() const;
  };

  struct EntryMetadata {
    std::optional<std::uint64_t> accessedSeconds, createdSeconds;
    InternedText owner = nullptr, group = nullptr;
    std::optional<std::uint32_t> mode;
    bool operator==(const EntryMetadata&) const = default;
  };
  struct FileAccess {
    std::optional<bool> read;
    std::optional<bool> write;
    std::optional<bool> execute;
    std::optional<bool> rename;
    std::optional<bool> trash;
    std::optional<bool> remove;
  };

  struct FilesystemInfo {
    std::optional<bool> readOnly;
    std::optional<bool> remote;
    std::optional<std::string> type;
    std::optional<std::uint64_t> freeBytes = std::nullopt;
  };

  struct DirectoryMetadata {
    FileAccess access;
    FilesystemInfo filesystem;
  };
} // namespace luciole
