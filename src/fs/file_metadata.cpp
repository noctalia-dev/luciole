#include "fs/file_metadata.h"

#include "fs/gio_interop.h"

namespace {
  std::optional<bool> boolean(GFileInfo* info, const char* attribute) {
    if (g_file_info_get_attribute_type(info, attribute) != G_FILE_ATTRIBUTE_TYPE_BOOLEAN) {
      return std::nullopt;
    }
    return g_file_info_get_attribute_boolean(info, attribute) != FALSE;
  }
} // namespace

std::optional<std::string> luciole::readContentType(GFileInfo* info) {
  for (const auto* attribute : {G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE}) {
    if (g_file_info_get_attribute_type(info, attribute) != G_FILE_ATTRIBUTE_TYPE_STRING) {
      continue;
    }
    const char* value = g_file_info_get_attribute_string(info, attribute);
    if (value != nullptr && *value != '\0' && g_utf8_validate(value, -1, nullptr)) {
      return value;
    }
  }
  return std::nullopt;
}

std::string luciole::fileTypeDescription(FileType type, bool symlink, const std::optional<std::string>& contentType) {
  if (symlink || type == FileType::Symlink) {
    return "Link";
  }
  switch (type) {
  case FileType::Directory:
    return "Folder";
  case FileType::Special:
    return "Special";
  case FileType::Shortcut:
    return "Shortcut";
  case FileType::Mountable:
    return "Mount";
  case FileType::Regular:
    if (contentType && !g_content_type_is_unknown(contentType->c_str())) {
      const GString description(g_content_type_get_description(contentType->c_str()));
      return description && *description ? description.get() : *contentType;
    }
    return "File";
  default:
    return "Unknown";
  }
}

bool luciole::contentTypeIsText(const std::string& contentType) {
  if (contentType.starts_with("image/") || contentType.starts_with("video/") || contentType.starts_with("audio/")) {
    return false;
  }
  return !contentType.empty()
      && (contentType.starts_with("text/")
          || g_content_type_is_a(contentType.c_str(), "text/plain")
          // Retain the existing Nix-provider spelling even when absent from the installed MIME database.
          || contentType == "application/x-nix");
}

luciole::FileAccess luciole::readFileAccess(GFileInfo* info) {
  return {
      .read = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ),
      .write = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE),
      .execute = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE),
      .rename = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME),
      .trash = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH),
      .remove = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE)
  };
}

luciole::FilesystemInfo luciole::readFilesystemInfo(GFileInfo* info) {
  FilesystemInfo result{
      .readOnly = boolean(info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY),
      .remote = boolean(info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE),
      .type = std::nullopt
  };
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE) == G_FILE_ATTRIBUTE_TYPE_STRING) {
    result.type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);
  }
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE) == G_FILE_ATTRIBUTE_TYPE_UINT64) {
    result.freeBytes = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
  }
  return result;
}

std::optional<luciole::FileIdentity> luciole::readFileIdentity(GFileInfo* info) {
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM) != G_FILE_ATTRIBUTE_TYPE_STRING
      || g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_ID_FILE) != G_FILE_ATTRIBUTE_TYPE_STRING) {
    return std::nullopt;
  }
  const char* filesystem = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
  const char* file = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE);
  if (!filesystem || !*filesystem || !file || !*file) {
    return std::nullopt;
  }
  return FileIdentity{filesystem, file};
}

luciole::EntryMetadata luciole::readEntryMetadata(GFileInfo* info) {
  EntryMetadata result;
  const auto number = [&](const char* key) -> std::optional<std::uint64_t> {
    return g_file_info_get_attribute_type(info, key) == G_FILE_ATTRIBUTE_TYPE_UINT64
        ? std::optional(g_file_info_get_attribute_uint64(info, key))
        : std::nullopt;
  };
  const auto text = [&](const char* key) -> std::optional<std::string> {
    if (g_file_info_get_attribute_type(info, key) != G_FILE_ATTRIBUTE_TYPE_STRING) {
      return {};
    }
    const char* value = g_file_info_get_attribute_string(info, key);
    if (!value || !*value || !g_utf8_validate(value, -1, nullptr)) {
      return {};
    }
    return value;
  };
  result.accessedSeconds = number(G_FILE_ATTRIBUTE_TIME_ACCESS);
  result.createdSeconds = number(G_FILE_ATTRIBUTE_TIME_CREATED);
  result.owner = text(G_FILE_ATTRIBUTE_OWNER_USER);
  result.group = text(G_FILE_ATTRIBUTE_OWNER_GROUP);
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_UNIX_MODE) == G_FILE_ATTRIBUTE_TYPE_UINT32) {
    result.mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 07777U;
  }
  return result;
}
