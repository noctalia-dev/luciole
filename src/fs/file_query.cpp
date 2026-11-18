#include "fs/file_query.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/native_listing.h"

#include <format>
#include <memory>
#include <unordered_set>
#include <utility>

void luciole::queryEntryIdentities(
    DirectoryEntries entries, std::vector<std::uint64_t> ids, const CancelToken& token,
    std::move_only_function<void(std::expected<EntryIdentities, FileError>)> done
) {
  runJob<EntryIdentities>(
      token,
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
          const auto file = gioFile(entries.location(i));
          const GioPtr<GFileInfo> info(g_file_query_info(
              file.get(), G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM,
              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token), nullptr
          ));
          if (info) {
            if (auto identity = readFileIdentity(info.get())) {
              result.emplace(brief.id, std::move(*identity));
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
  GError* rawError = nullptr;
  const GioPtr<GFileInfo> info(g_file_query_info(
      gioFile(location).get(),
      G_FILE_ATTRIBUTE_STANDARD_TYPE
      "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE
      "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE
      "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_ACCESS "," G_FILE_ATTRIBUTE_TIME_CREATED
      "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_OWNER_GROUP "," G_FILE_ATTRIBUTE_UNIX_MODE
      "," G_FILE_ATTRIBUTE_ETAG_VALUE "," G_FILE_ATTRIBUTE_TIME_CHANGED
      ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "," G_FILE_ATTRIBUTE_ID_FILE
      "," G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token), &rawError
  ));
  const GErrorPtr error(rawError);
  if (!info) {
    throw FileErrorException(error ? fileError(*error) : FileError{FileErrorKind::Other, "Metadata unavailable"});
  }
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
  runJob<FileDetails>(
      token, [location, token] { return readFileDetails(location, token); },
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
  runJob<FileIconSource>(
      token,
      [location, token] {
        GError* rawError = nullptr;
        const GioPtr<GFileInfo> info(g_file_query_info(
            gioFile(location).get(),
            G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_ICON,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token), &rawError
        ));
        const GErrorPtr error(rawError);
        if (!info) {
          throw FileErrorException(error ? fileError(*error) : FileError{FileErrorKind::Other, "Icon unavailable"});
        }
        return readFileIcon(info.get(), location);
      },
      [token, done = std::move(done)](auto reply) mutable {
        if (!token.cancelled()) {
          done(std::move(reply));
        }
      }
  );
}

void luciole::queryFileType(
    const Location& location, const CancelToken& token,
    std::move_only_function<void(std::expected<FileType, FileError>)> done
) {
  using Done = std::move_only_function<void(std::expected<FileType, FileError>)>;
  const auto file = gioFile(location);
  g_file_query_info_async(
      file.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, gioCancellable(token),
      [](GObject* source, GAsyncResult* result, gpointer raw) {
        const std::unique_ptr<Done> done(static_cast<Done*>(raw));
        GError* rawError = nullptr;
        const GioPtr<GFileInfo> info(g_file_query_info_finish(G_FILE(source), result, &rawError));
        const GErrorPtr error(rawError);
        if (error && g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
          return;
        }
        if (error) {
          (*done)(std::unexpected(fileError(*error)));
        } else {
          (*done)(fileType(g_file_info_get_file_type(info.get())));
        }
      },
      new Done(std::move(done))
  );
}

std::optional<luciole::FileError> luciole::enumerateChildren(
    const Location& parent, const CancelToken& token, const std::function<bool(const ChildInfo&)>& visit
) {
  auto* cancellable = gioCancellable(token);
  GError* raw = nullptr;
  const GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
      gioFile(parent).get(),
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
                                     "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
      G_FILE_QUERY_INFO_NONE, cancellable, &raw
  ));
  while (enumerator && !g_cancellable_is_cancelled(cancellable)) {
    const GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), cancellable, &raw));
    if (!info) {
      break;
    }
    const char* name = g_file_info_get_name(info.get());
    const char* display = g_file_info_get_display_name(info.get());
    const ChildInfo child{
        .name = name,
        .displayName = display ? display : name,
        .type = fileType(g_file_info_get_file_type(info.get())),
        .hidden = g_file_info_get_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN)
            || (name && name[0] == '.'),
    };
    if (!visit(child)) {
      break;
    }
  }
  const GErrorPtr error(raw);
  if (enumerator) {
    g_file_enumerator_close(enumerator.get(), cancellable, nullptr);
  }
  if (error && !g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return fileError(*error);
  }
  return std::nullopt;
}
