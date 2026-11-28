#include "operations/transfer_estimate.h"

#include "fs/directory_entries.h"
#include "fs/file_error.h"
#include "fs/folder_totals.h"
#include "fs/folder_totals_walk.h"
#include "fs/native_listing.h"

#include <filesystem>
#include <sys/stat.h>
#include <vector>

namespace {
  // Bounds the extra traversal one estimate may add to a transfer.
  constexpr luciole::FolderTotalsLimits kLimits{.items = 200000, .depth = 128};

  // Running totals are optional presentation.
  void report(const std::function<void(const luciole::SourceEstimate&)>& sink, const luciole::SourceEstimate& value) {
    if (sink) {
      sink(value);
    }
  }

  // The folder walk only needs the entry's place and name.
  luciole::DirectoryEntry rootEntry(const luciole::Location& location, const std::string& path) {
    luciole::DirectoryEntry entry;
    entry.place = luciole::EntryPlace(location);
    entry.name = std::filesystem::path(path).filename().native();
    return entry;
  }
  // Items a reader counts as files: regular files, links and special nodes, not folders.
  std::size_t countedFiles(const luciole::FolderTotalsSnapshot& snapshot) {
    return snapshot.files + snapshot.links + snapshot.other;
  }
} // namespace

std::optional<luciole::SourceEstimate> luciole::estimateSource(
    const Location& source, const CancelToken& token, const std::function<void(const SourceEstimate&)>& publish
) {
  const auto path = source.nativePath();
  if (!path) {
    return {};
  }
  if (networkFilesystem(*path)) {
    return {};
  }
  struct stat value{};
  if (::lstat(path->c_str(), &value) != 0) {
    return SourceEstimate{.partial = true};
  }
  if (!S_ISDIR(value.st_mode)) {
    // Only regular files move payload bytes while a link is recreated and a special file never opened.
    SourceEstimate estimate{.files = 1};
    if (S_ISREG(value.st_mode) && value.st_size >= 0) {
      estimate.bytes = static_cast<std::uint64_t>(value.st_size);
    }
    // The copier rejects special nodes, so their count can never land.
    estimate.partial = !S_ISREG(value.st_mode) && !S_ISLNK(value.st_mode);
    return estimate;
  }
  SourceEstimate estimate;
  std::vector<DirectoryEntry> roots{rootEntry(source, *path)};
  try {
    // The shared folder walk also counts link target lengths, which a transfer never writes, so a
    // tree with many links measures slightly more than the bytes that arrive.
    const auto walked = walkFolderTotals(
        std::move(roots), token,
        [&](const luciole::FolderTotalsSnapshot& snapshot) {
          auto running = estimate;
          running.files += countedFiles(snapshot);
          running.bytes += snapshot.bytes;
          // Special nodes count toward the total but never move, so it is only a lower bound.
          running.partial |= snapshot.partial() || snapshot.other != 0;
          report(publish, running);
        },
        kLimits
    );
    estimate.files += countedFiles(walked);
    estimate.bytes += walked.bytes;
    estimate.partial |= walked.partial() || walked.other != 0;
  } catch (const FileErrorException&) {
    // A changed or unreadable tree leaves an unknown remainder while the transfer still runs.
    estimate.partial = true;
  }
  return estimate;
}
