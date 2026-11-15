#include "operations/trash_item.h"

#include "fs/gio_interop.h"

#include <set>
#include <string_view>

namespace {
  constexpr const char* attributes =
      "standard::name,standard::display-name,standard::type,standard::is-symlink,standard::size,"
      "time::modified,id::file,id::filesystem,etag::value,trash::orig-path,trash::deletion-date";

  luciole::TrashItemInfo readInfo(GFileInfo* info) {
    const auto attribute = [info](const char* name, bool bytes = false) -> std::optional<std::string> {
      if (g_file_info_get_attribute_type(info, name)
          != (bytes ? G_FILE_ATTRIBUTE_TYPE_BYTE_STRING : G_FILE_ATTRIBUTE_TYPE_STRING)) {
        return std::nullopt;
      }
      const char* value =
          bytes ? g_file_info_get_attribute_byte_string(info, name) : g_file_info_get_attribute_string(info, name);
      return value && *value ? std::optional<std::string>(value) : std::nullopt;
    };
    const auto number = [info](const char* name) -> std::optional<std::uint64_t> {
      return g_file_info_has_attribute(info, name) ? std::optional(g_file_info_get_attribute_uint64(info, name))
                                                   : std::nullopt;
    };
    return {
        .id = luciole::readFileIdentity(info),
        .type = luciole::fileType(g_file_info_get_file_type(info)),
        .symlink = g_file_info_get_is_symlink(info) != FALSE,
        .size = number(G_FILE_ATTRIBUTE_STANDARD_SIZE),
        .modified = number(G_FILE_ATTRIBUTE_TIME_MODIFIED),
        .etag = attribute(G_FILE_ATTRIBUTE_ETAG_VALUE),
        .original = attribute(G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, true),
        .deleted = attribute(G_FILE_ATTRIBUTE_TRASH_DELETION_DATE)
    };
  }

  [[noreturn]] void throwGio(const luciole::GErrorPtr& error, const char* fallback) {
    if (error) {
      throw luciole::FileErrorException(luciole::fileError(*error));
    }
    luciole::throwFileError(luciole::FileErrorKind::Other, fallback);
  }
} // namespace

luciole::TrashItemIdentity luciole::TrashItemInfo::identity() const {
  if (!id || type == FileType::Unknown) {
    throwFileError(FileErrorKind::Unsupported, "The trash service cannot identify these items safely.");
  }
  return {.id = *id, .type = type, .symlink = symlink, .etag = etag, .original = original, .deleted = deleted};
}

bool luciole::isTopLevelTrashItem(const Location& location) {
  return location.isTrash() && location.parent() == Location::trash();
}

luciole::TrashItemIdentity luciole::readTrashItemIdentity(const DirectoryEntry& entry) {
  auto identity = entry.identity();
  if (!identity || entry.type == FileType::Unknown) {
    throwFileError(FileErrorKind::Unsupported, "The trash service cannot identify these items safely.");
  }
  return {
      .id = std::move(*identity),
      .type = entry.type,
      .symlink = entry.symlink,
      .etag = entry.etag(),
      .original = entry.trashInfo().original ? entry.trashInfo().original->nativePath() : std::nullopt,
      .deleted = entry.trashInfo().deletionDate
  };
}

luciole::TrashItemInfo luciole::queryTrashItem(const Location& location, const CancelToken& token, bool follow) {
  GError* raw = nullptr;
  const GioPtr<GFileInfo> info(g_file_query_info(
      gioFile(location).get(), attributes, follow ? G_FILE_QUERY_INFO_NONE : G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      gioCancellable(token), &raw
  ));
  const GErrorPtr error(raw);
  if (!info) {
    throwGio(error, "Could not read the item.");
  }
  return readInfo(info.get());
}

std::vector<luciole::TrashItem> luciole::listTrashItems(const CancelToken& token) {
  const auto trash = Location::trash();
  const auto root = gioFile(trash);
  GError* raw = nullptr;
  const GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
      root.get(), attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token), &raw
  ));
  const GErrorPtr error(raw);
  if (!enumerator) {
    throwGio(error, "Could not read Trash.");
  }
  std::vector<TrashItem> items;
  std::set<std::string> names;
  for (;;) {
    raw = nullptr;
    const GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), gioCancellable(token), &raw));
    const GErrorPtr nextError(raw);
    if (nextError) {
      throw FileErrorException(fileError(*nextError));
    }
    if (!info) {
      break;
    }
    const char* name = g_file_info_get_name(info.get());
    if (name == nullptr
        || *name == '\0'
        || std::string_view(name).contains('/')
        || std::string_view(name) == "."
        || std::string_view(name) == ".."
        || !names.insert(name).second) {
      throwFileError(FileErrorKind::Other, "The trash service returned an invalid item.");
    }
    auto location = trash.child(name);
    if (!location.isTrash() || location.parent() != trash) {
      throwFileError(FileErrorKind::Other, "The trash service returned an item outside Trash.");
    }
    const char* display = g_file_info_get_display_name(info.get());
    const GString valid(g_utf8_make_valid(display != nullptr ? display : name, -1));
    items.push_back({std::move(location), valid.get(), readInfo(info.get()).identity()});
  }
  raw = nullptr;
  const bool closed = g_file_enumerator_close(enumerator.get(), gioCancellable(token), &raw);
  const GErrorPtr closeError(raw);
  if (!closed) {
    throwGio(closeError, "Could not finish reading Trash.");
  }
  return items;
}
