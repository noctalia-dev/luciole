#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "operations/archive_service.h"

#include <atomic>
#include <cstring>

namespace luciole::archive_work {
  struct Progress {
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::size_t> items{0};
    std::atomic<bool> passwordNeeded{false};
    // Accessed by a worker, then its completion callback, never concurrently.
    std::string sourceStamp;
    ArchiveFormat passwordFormat = ArchiveFormat::Zip;
  };
  struct Passwords {
    std::vector<std::string> values;
    ~Passwords() {
      for (auto& value : values) {
        ::explicit_bzero(value.data(), value.size());
      }
    }
    Passwords() = default;
    Passwords(const Passwords&) = delete;
    Passwords& operator=(const Passwords&) = delete;
  };
  struct Prepared {
    std::string stage;
    std::shared_ptr<ArchiveCatalog> catalog;
    std::optional<Location> output;
    std::uint64_t expandedBytes = 0;
    bool archive = false;
  };
  // Entry icons are left empty; see archive_icons.h.
  Prepared inspect(
      const Location& source, ArchiveLimits limits, const CancelToken& cancel, Progress& progress,
      const Passwords& passwords = {}
  );
  Prepared compress(
      const std::vector<Location>& sources, const Location& destination, ArchiveFormat format,
      ArchiveCompressionLevel level, ArchiveLimits limits, const CancelToken& cancel, Progress& progress,
      const Passwords& passwords = {}
  );
  Prepared extract(
      const Location& source, const Location& destination, const std::vector<std::string>& selected,
      const std::shared_ptr<const ArchiveCatalog>& expected, bool open, ArchiveLimits limits, const CancelToken& cancel,
      Progress& progress, const Passwords& passwords = {}
  );
  std::optional<FileError> cleanup(const std::vector<std::string>& paths);
} // namespace luciole::archive_work
