#include "fs/file_query.h"

#include "fs/gio_interop.h"
#include "fs/native_listing.h"
#include "fs/network_info.h"
#include "fs/network_job.h"

#include <format>
#include <memory>
#include <unordered_set>
#include <utility>

void luciole::queryEntryIdentities(
    DirectoryEntries entries, std::vector<std::uint64_t> ids, const CancelToken& token,
    std::move_only_function<void(std::expected<EntryIdentities, FileError>)> done
) {
  const auto lane = entries.empty() ? IoLane::Native : ioLane(entries.location(0));
  runIoJob<EntryIdentities>(
      lane, token,
      [entries = std::move(entries), ids = std::move(ids), token] {
        EntryIdentities result;
        std::unordered_set<std::uint64_t> wanted(ids.begin(), ids.end());
        std::unordered_map<const BrowseCatalogue*, std::vector<std::size_t>> queried;
        std::unordered_map<std::string_view, const DirectoryEntry*> bulk;
        NativeListing listing;
        if (wanted.size() > 512 && entries.catalogue()) {
          listing = listNativeFolder(*entries.catalogue()->folder->nativePath(), 0, gioCancellable(token));
          if (!listing.unsupported) {
            for (const auto& entry : listing.entries) {
              bulk.emplace(entry.name, &entry);
            }
          }
        }
        for (std::size_t i = 0; i < entries.size() && !token.cancelled() && !wanted.empty(); ++i) {
          const auto brief = entries.brief(i);
          if (!wanted.erase(brief.id)) {
            continue;
          }
          if (const auto* known = entries.known(i)) {
            if (auto identity = known->identity()) {
              result.emplace(brief.id, std::move(*identity));
            }
            continue;
          }
          const auto row = entries.catalogueRow(i);
          if (!row) {
            continue;
          }
          queried[row->catalogue].push_back(row->index);
          if (const auto found = bulk.find(brief.name); found != bulk.end()) {
            if (auto identity = found->second->identity()) {
              result.emplace(brief.id, std::move(*identity));
            }
            continue;
          }
          try {
            const auto info = isolatedInfo(
                entries.location(i), G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, token
            );
            if (auto identity = readFileIdentity(info.get())) {
              result.emplace(brief.id, std::move(*identity));
            }
          } catch (const FileErrorException& error) {
            if (error.error().kind != FileErrorKind::Missing && error.error().kind != FileErrorKind::Denied) {
              throw;
            }
          }
        }
        for (const auto& [catalogue, rows] : queried) {
          const auto validation = validateCatalogueRows(*catalogue, rows, gioCancellable(token));
          if (validation.parentChanged || validation.unavailable) {
            for (const auto index : rows) {
              result.erase(catalogue->brief(index).id);
            }
          } else {
            for (const auto index : validation.changed) {
              result.erase(catalogue->brief(index).id);
            }
          }
        }
        return result;
      },
      std::move(done)
  );
}

luciole::FileDetails luciole::readFileDetails(const Location& location, const CancelToken& token) {
  const auto info = isolatedInfo(
      location,
      G_FILE_ATTRIBUTE_STANDARD_TYPE
      "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE
      "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE
      "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_ACCESS "," G_FILE_ATTRIBUTE_TIME_CREATED
      "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_OWNER_GROUP "," G_FILE_ATTRIBUTE_UNIX_MODE
      "," G_FILE_ATTRIBUTE_ETAG_VALUE "," G_FILE_ATTRIBUTE_TIME_CHANGED
      ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "," G_FILE_ATTRIBUTE_ID_FILE
      "," G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, token
  );
  FileDetails details;
  if (g_file_info_get_attribute_type(info.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE) == G_FILE_ATTRIBUTE_TYPE_UINT32) {
    details.type = fileType(g_file_info_get_file_type(info.get()));
  }
  details.metadata = readEntryMetadata(info.get());
  if (const char* etag = g_file_info_get_etag(info.get())) {
    details.etag = etag;
  }
  details.contentRevision = readChangeRevision(info.get());
  details.identity = readFileIdentity(info.get());
  if (g_file_info_get_attribute_type(info.get(), G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME)
      == G_FILE_ATTRIBUTE_TYPE_STRING) {
    details.displayName = g_file_info_get_display_name(info.get());
  }
  details.typeDescription = fileTypeDescription(details.type, readIsSymlink(info.get()), readContentType(info.get()));
  if (g_file_info_get_attribute_type(info.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE) == G_FILE_ATTRIBUTE_TYPE_UINT64) {
    details.size = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE);
  }
  if (g_file_info_get_attribute_type(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED) == G_FILE_ATTRIBUTE_TYPE_UINT64) {
    details.modifiedSeconds = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED);
  }
  if (g_file_info_get_attribute_type(info.get(), G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)
      == G_FILE_ATTRIBUTE_TYPE_BYTE_STRING) {
    const char* target = g_file_info_get_symlink_target(info.get());
    const GString display(g_filename_display_name(target));
    details.linkTarget = display.get();
  }
  return details;
}

void luciole::queryFileDetails(
    const Location& location, const CancelToken& token,
    std::move_only_function<void(std::expected<FileDetails, FileError>)> done
) {
  runIoJob<FileDetails>(
      ioLane(location), token, [location, token] { return readFileDetails(location, token); },
      [token, done = std::move(done)](auto reply) mutable {
        if (!token.cancelled()) {
          done(std::move(reply));
        }
      }
  );
}

void luciole::queryFileIcon(
    const Location& location, const CancelToken& token,
    std::move_only_function<void(std::expected<FileIconSource, FileError>)> done
) {
  runIoJob<FileIconSource>(
      ioLane(location), token,
      [location, token] {
        const auto info = isolatedInfo(
            location,
            G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_ICON,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, token
        );
        return readFileIcon(info.get(), location);
      },
      [token, done = std::move(done)](auto reply) mutable {
        if (!token.cancelled()) {
          done(std::move(reply));
        }
      }
  );
}

luciole::FileType luciole::readFileType(const Location& location, const CancelToken& token, FollowSymlinks follow) {
  const auto info = isolatedInfo(
      location, G_FILE_ATTRIBUTE_STANDARD_TYPE,
      follow == FollowSymlinks::Yes ? G_FILE_QUERY_INFO_NONE : G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, token
  );
  return fileType(g_file_info_get_file_type(info.get()));
}

void luciole::queryFileType(
    const Location& location, const CancelToken& token,
    std::move_only_function<void(std::expected<FileType, FileError>)> done
) {
  runIoJob<FileType>(
      ioLane(location), token, [location, token] { return readFileType(location, token); },
      [token, done = std::move(done)](JobResult<FileType> result) mutable {
        if (!token.cancelled()) {
          done(std::move(result));
        }
      }
  );
}

std::optional<luciole::FileError> luciole::enumerateChildren(
    const Location& parent, const CancelToken& token, const std::function<bool(const ChildInfo&)>& visit
) {
  try {
    isolatedChildren(
        parent,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
                                       "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NONE, token, [&](GFileInfo* info) {
          const char* name = g_file_info_get_name(info);
          const char* display = g_file_info_get_display_name(info);
          return visit(
              {.name = name,
               .displayName = display ? display : name,
               .type = fileType(g_file_info_get_file_type(info)),
               .hidden = g_file_info_get_is_hidden(info) != FALSE || (name && name[0] == '.')}
          );
        }
    );
  } catch (...) {
    auto error = currentFileError();
    if (error.kind != FileErrorKind::Cancelled) {
      return error;
    }
  }
  return std::nullopt;
}
