#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/file_icon_source.h"
#include "fs/file_metadata.h"
#include "fs/gio_ptr.h"
#include "fs/location.h"
#include "fs/trash_info.h"

#include <gio/gio.h>
#include <optional>
#include <string>
#include <unordered_map>

// GIO bridge for code that drives GIO itself: fs, operations, thumbnails, imaging,
// platform, and clipboard sources. browser, config, ui, and app use the typed fs API.
namespace luciole {
  GioPtr<GFile> gioFile(const Location& location);
  Location locationFromFile(GFile* file);
  GCancellable* gioCancellable(const CancelToken& token);
  CancelToken cancellationToken(GCancellable* cancellable);

  FileError fileError(const GError& error);
  // May synchronously query installed GIO providers; call on a worker.
  FileError directoryOpenError(const Location& location, FileError error);
  FileType fileType(GFileType type);
  // Providers may omit the separate boolean when their type already describes the item.
  bool readIsSymlink(GFileInfo* info);
  // Typed attribute reads; absent or differently typed attributes are null. One name lookup each.
  const char* infoString(GFileInfo* info, const char* attribute);
  std::optional<bool> infoBoolean(GFileInfo* info, const char* attribute);
  std::optional<std::uint32_t> infoUint32(GFileInfo* info, const char* attribute);
  std::optional<std::uint64_t> infoUint64(GFileInfo* info, const char* attribute);

  std::optional<FileIdentity> readFileIdentity(GFileInfo* info);
  EntryMetadata readEntryMetadata(GFileInfo* info);
  FileAccess readFileAccess(GFileInfo* info);
  FilesystemInfo readFilesystemInfo(GFileInfo* info);
  // Prefer the provider's regular MIME type, falling back to its filename-only type when unavailable.
  InternedText readContentType(GFileInfo* info);
  // time::changed with time::changed-nsec, falling back to time::changed-usec; nullopt without time::changed.
  std::optional<ChangeRevision> readChangeRevision(GFileInfo* info);
  FileIconSource readIconSource(GIcon* icon);
  FileIconSource readFileIcon(GFileInfo* info, const Location& location);
  TrashInfo readTrashInfo(GFileInfo* info);

  // Deduplicates the icon sources of one listing. Not thread-safe.
  class FileIconPool {
  public:
    // For infos that carry the provider's standard::icon; `folder` contains the entry.
    SharedFileIcon get(GFileInfo* info, const Location& folder);
    // Children of a local folder listed without standard::icon. GIO's local backend derives that icon from
    // the content type and the user's special folders, building two icon objects per file; this derives the
    // same icon once per distinct value. nullptr clears the folder.
    void setLocalFolder(const char* path);
    // name is the raw filename; contentType is GIO's regular content type, null when unknown; `folder` contains it.
    SharedFileIcon getLocal(const char* name, FileType type, const char* contentType, const Location& folder);

  private:
    struct SpecialIcon {
      const char* name;
      bool fallbacks;
    };
    std::unordered_map<std::string, SpecialIcon> m_special;
    std::unordered_map<std::string, SharedFileIcon> m_icons;
    std::string m_key;
  };
  struct DirectoryEntry;
  DirectoryEntry readEntry(
      GFileInfo* info, const std::shared_ptr<const Location>& folder, FileIconPool& icons, bool local, bool trashFolder
  );
} // namespace luciole
