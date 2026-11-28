#include "operations/transfer_estimate.h"

#include "fs/directory_entries.h"
#include "fs/file_error.h"
#include "fs/folder_totals.h"
#include "fs/folder_totals_walk.h"
#include "fs/gio_interop.h"
#include "fs/native_listing.h"
#include "fs/transfer_types.h"

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

  // The provider transfer refuses a tree past this many entries, so measuring further would describe a failed copy.
  constexpr std::size_t kProviderItems = 100000;

  // Phones and cameras have no native path, so they are measured through GIO. This is one extra listing per folder
  // before the copy, which lists them again, so on a device it costs a pass over USB before the first byte moves.
  class GioWalk {
  public:
    GioWalk(const luciole::CancelToken& token, const std::function<void(const luciole::SourceEstimate&)>& publish)
        : m_token(token), m_publish(publish) {}

    luciole::SourceEstimate run(const luciole::Location& source) {
      const luciole::GioPtr<GFileInfo> info(g_file_query_info(
          luciole::gioFile(source).get(), kAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
          luciole::gioCancellable(m_token), nullptr
      ));
      if (!info) {
        m_estimate.partial = true;
      } else {
        add(source, info.get(), 0);
      }
      return m_estimate;
    }

  private:
    static constexpr const char* kAttributes = "standard::name,standard::type,standard::size,standard::is-symlink";

    void add(const luciole::Location& location, GFileInfo* info, std::size_t depth) {
      if (m_stopped || m_token.cancelled() || ++m_items > kProviderItems || depth > kLimits.depth) {
        // Cancelled or past the limits: the rest stays unmeasured.
        m_estimate.partial = true;
        m_stopped = true;
        return;
      }
      const auto type = g_file_info_get_file_type(info);
      if (g_file_info_get_is_symlink(info) || (type != G_FILE_TYPE_REGULAR && type != G_FILE_TYPE_DIRECTORY)) {
        // Provider copies count only regular files as done, so anything else would hold the bar short of full.
        m_estimate.partial = true;
        return;
      }
      if (type == G_FILE_TYPE_REGULAR) {
        ++m_estimate.files;
        m_estimate.bytes += static_cast<std::uint64_t>(std::max<goffset>(0, g_file_info_get_size(info)));
        return;
      }
      const luciole::GioPtr<GFileEnumerator> children(g_file_enumerate_children(
          luciole::gioFile(location).get(), kAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
          luciole::gioCancellable(m_token), nullptr
      ));
      if (!children) {
        m_estimate.partial = true;
        return;
      }
      while (!m_stopped) {
        GError* error = nullptr;
        const luciole::GioPtr<GFileInfo> child(
            g_file_enumerator_next_file(children.get(), luciole::gioCancellable(m_token), &error)
        );
        if (error) {
          // An unreadable folder leaves an unknown remainder while the rest is still measured.
          g_error_free(error);
          m_estimate.partial = true;
          break;
        }
        if (!child) {
          break;
        }
        add(location.child(g_file_info_get_name(child.get())), child.get(), depth + 1);
      }
      report(m_publish, m_estimate);
    }

    const luciole::CancelToken& m_token;
    const std::function<void(const luciole::SourceEstimate&)>& m_publish;
    luciole::SourceEstimate m_estimate;
    std::size_t m_items = 0;
    bool m_stopped = false;
  };
} // namespace

luciole::SourceEstimate luciole::estimateGioSource(
    const Location& source, const CancelToken& token, const std::function<void(const SourceEstimate&)>& publish
) {
  return GioWalk(token, publish).run(source);
}

bool luciole::measurableSource(const Location& source) {
  return source.nativePath().has_value() || transferDeviceSource(source);
}

std::optional<luciole::SourceEstimate> luciole::estimateSource(
    const Location& source, const CancelToken& token, const std::function<void(const SourceEstimate&)>& publish
) {
  // GVfs gives a phone a FUSE path when gvfsd-fuse runs, which the native walk refuses as a network filesystem.
  if (transferDeviceSource(source)) {
    return estimateGioSource(source, token, publish);
  }
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
