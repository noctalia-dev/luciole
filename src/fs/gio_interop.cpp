#include "fs/gio_interop.h"

#include <format>

luciole::FileError luciole::fileError(const GError& error) {
  using Kind = FileErrorKind;
  auto kind = Kind::Other;
  if (error.domain == G_IO_ERROR) {
    switch (error.code) {
    case G_IO_ERROR_NOT_FOUND:
      kind = Kind::Missing;
      break;
    case G_IO_ERROR_PERMISSION_DENIED:
      kind = Kind::Denied;
      break;
    case G_IO_ERROR_NOT_DIRECTORY:
      kind = Kind::NotDirectory;
      break;
    case G_IO_ERROR_IS_DIRECTORY:
      kind = Kind::IsDirectory;
      break;
    case G_IO_ERROR_EXISTS:
      kind = Kind::Exists;
      break;
    case G_IO_ERROR_NOT_EMPTY:
      kind = Kind::NotEmpty;
      break;
    case G_IO_ERROR_FILENAME_TOO_LONG:
      kind = Kind::NameTooLong;
      break;
    case G_IO_ERROR_NO_SPACE:
      kind = Kind::NoSpace;
      break;
    case G_IO_ERROR_READ_ONLY:
      kind = Kind::ReadOnly;
      break;
    case G_IO_ERROR_INVALID_ARGUMENT:
      kind = Kind::InvalidArgument;
      break;
    case G_IO_ERROR_WOULD_RECURSE:
      kind = Kind::WouldRecurse;
      break;
    case G_IO_ERROR_WOULD_MERGE:
      kind = Kind::WouldMerge;
      break;
    case G_IO_ERROR_NOT_MOUNTED:
      kind = Kind::NotMounted;
      break;
    case G_IO_ERROR_HOST_NOT_FOUND:
    case G_IO_ERROR_HOST_UNREACHABLE:
    case G_IO_ERROR_NETWORK_UNREACHABLE:
    case G_IO_ERROR_CONNECTION_REFUSED:
    case G_IO_ERROR_CONNECTION_CLOSED:
    case G_IO_ERROR_NOT_CONNECTED:
    case G_IO_ERROR_TIMED_OUT:
      kind = Kind::Unavailable;
      break;
    case G_IO_ERROR_NOT_SUPPORTED:
      kind = Kind::Unsupported;
      break;
    case G_IO_ERROR_CANCELLED:
      kind = Kind::Cancelled;
      break;
    default:
      break;
    }
  }
  return {.kind = kind, .message = error.message};
}

luciole::FileError luciole::directoryOpenError(const Location& location, FileError error) {
  if (error.kind != FileErrorKind::Unsupported || location.isNative() || location.isTrash()) {
    return error;
  }
  const GString scheme(g_uri_parse_scheme(location.uri().c_str()));
  if (!scheme) {
    return error;
  }
  const auto* schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
  for (const auto* current = schemes; current && *current; ++current) {
    if (g_ascii_strcasecmp(*current, scheme.get()) == 0) {
      return error;
    }
  }
  error.message = std::format(
      "Desktop support for {}:// is unavailable. Install or enable its GIO/GVfs backend and session services, then "
      "restart Luciole.",
      scheme.get()
  );
  return error;
}

luciole::FileType luciole::fileType(GFileType type) {
  switch (type) {
  case G_FILE_TYPE_REGULAR:
    return FileType::Regular;
  case G_FILE_TYPE_DIRECTORY:
    return FileType::Directory;
  case G_FILE_TYPE_SYMBOLIC_LINK:
    return FileType::Symlink;
  case G_FILE_TYPE_SPECIAL:
    return FileType::Special;
  case G_FILE_TYPE_SHORTCUT:
    return FileType::Shortcut;
  case G_FILE_TYPE_MOUNTABLE:
    return FileType::Mountable;
  case G_FILE_TYPE_UNKNOWN:
    break;
  }
  return FileType::Unknown;
}

namespace {
  // GIO resolves an attribute name on every call; this checks the type and reads the value with one lookup.
  // Strings are returned directly, other types as a pointer to their storage.
  gpointer typedAttribute(GFileInfo* info, const char* attribute, GFileAttributeType expected) {
    GFileAttributeType type = G_FILE_ATTRIBUTE_TYPE_INVALID;
    gpointer value = nullptr;
    if (g_file_info_get_attribute_data(info, attribute, &type, &value, nullptr) == FALSE || type != expected) {
      return nullptr;
    }
    return value;
  }
} // namespace

const char* luciole::infoString(GFileInfo* info, const char* attribute) {
  return static_cast<const char*>(typedAttribute(info, attribute, G_FILE_ATTRIBUTE_TYPE_STRING));
}

std::optional<bool> luciole::infoBoolean(GFileInfo* info, const char* attribute) {
  const auto* value = static_cast<const gboolean*>(typedAttribute(info, attribute, G_FILE_ATTRIBUTE_TYPE_BOOLEAN));
  return value != nullptr ? std::optional(*value != FALSE) : std::nullopt;
}

std::optional<std::uint32_t> luciole::infoUint32(GFileInfo* info, const char* attribute) {
  const auto* value = static_cast<const guint32*>(typedAttribute(info, attribute, G_FILE_ATTRIBUTE_TYPE_UINT32));
  return value != nullptr ? std::optional<std::uint32_t>(*value) : std::nullopt;
}

std::optional<std::uint64_t> luciole::infoUint64(GFileInfo* info, const char* attribute) {
  const auto* value = static_cast<const guint64*>(typedAttribute(info, attribute, G_FILE_ATTRIBUTE_TYPE_UINT64));
  return value != nullptr ? std::optional<std::uint64_t>(*value) : std::nullopt;
}

bool luciole::readIsSymlink(GFileInfo* info) {
  return infoUint32(info, G_FILE_ATTRIBUTE_STANDARD_TYPE) == std::uint32_t{G_FILE_TYPE_SYMBOLIC_LINK}
  || infoBoolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK).value_or(false);
}
