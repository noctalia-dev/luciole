#include "operations/provider_item.h"

#include "fs/filename.h"

#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole;
  constexpr const char* attributes = "standard::type,standard::is-symlink,standard::size,id::file,id::filesystem,"
                                     "etag::value,time::modified,time::modified-usec,time::changed,time::changed-usec";
  void check(const CancelToken& token) {
    if (token.cancelled()) {
      throwFileError(FileErrorKind::Cancelled, "Operation cancelled.");
    }
  }
  [[noreturn]] void fail(GError* raw) {
    const GErrorPtr error(raw);
    throw FileErrorException(error ? fileError(*error) : FileError{FileErrorKind::Other, "Provider operation failed."});
  }
  GioPtr<GFileInfo> query(const Location& location, const CancelToken& token, bool follow) {
    check(token);
    GError* error = nullptr;
    GioPtr<GFileInfo> info(g_file_query_info(
        gioFile(location).get(), attributes, follow ? G_FILE_QUERY_INFO_NONE : G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        gioCancellable(token), &error
    ));
    if (!info) {
      fail(error);
    }
    if (!readFileIdentity(info.get())) {
      throwFileError(FileErrorKind::Unsupported, "The provider cannot identify this item safely.");
    }
    return info;
  }
  void move(const Location& source, const Location& destination, const CancelToken& token) {
    check(token);
    GError* error = nullptr;
    if (!g_file_move(
            gioFile(source).get(), gioFile(destination).get(),
            static_cast<GFileCopyFlags>(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
            gioCancellable(token), nullptr, nullptr, &error
        )) {
      fail(error);
    }
  }
  void erase(const ProviderItem& item, const CancelToken& token) {
    item.requireCurrent(token, false);
    check(token);
    GError* error = nullptr;
    // Finish this request before observing cancellation so an applied deletion is recorded accurately.
    if (!g_file_delete(gioFile(item.location()).get(), nullptr, &error)) {
      fail(error);
    }
  }
  void nameCheck(const std::string& name) {
    if (const auto problem = filenameProblem(name)) {
      throwFileError(FileErrorKind::InvalidArgument, *problem);
    }
  }
} // namespace

luciole::ProviderItem::ProviderItem(Location location, const CancelToken& token, bool follow)
    : m_location(std::move(location)), m_info(query(m_location, token, follow)), m_follow(follow) {}
bool luciole::ProviderItem::directory() const {
  return g_file_info_get_file_type(m_info.get()) == G_FILE_TYPE_DIRECTORY && (m_follow || !readIsSymlink(m_info.get()));
}
void luciole::ProviderItem::requireCurrent(const CancelToken& token, bool content) const {
  const auto now = query(m_location, token, m_follow);
  if (readFileIdentity(m_info.get()) != readFileIdentity(now.get())
      || g_file_info_get_file_type(m_info.get()) != g_file_info_get_file_type(now.get())
      || readIsSymlink(m_info.get()) != readIsSymlink(now.get())) {
    throwFileError(FileErrorKind::Changed, "The item was moved or replaced. Reopen this action.");
  }
  if (!content) {
    return;
  }
  for (const char* attribute :
       {G_FILE_ATTRIBUTE_ETAG_VALUE, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, G_FILE_ATTRIBUTE_TIME_CHANGED, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC}) {
    if (g_file_info_has_attribute(m_info.get(), attribute)) {
      const GString before(g_file_info_get_attribute_as_string(m_info.get(), attribute));
      const GString after(g_file_info_get_attribute_as_string(now.get(), attribute));
      if (!after || std::string_view(before.get()) != after.get()) {
        throwFileError(FileErrorKind::Changed, "The item changed. Reopen this action.");
      }
    }
  }
}

luciole::Location luciole::createProviderItem(
    const ProviderItem& parent, const std::string& name, bool folder, const CancelToken& token,
    const std::function<void(GOutputStream*)>& write
) {
  nameCheck(name);
  parent.requireCurrent(token, false);
  if (!parent.directory()) {
    throwFileError(FileErrorKind::NotDirectory, "The destination is not a folder.");
  }
  const auto destination = parent.location().child(name);
  GError* error = nullptr;
  if (folder) {
    check(token);
    if (!g_file_make_directory(gioFile(destination).get(), gioCancellable(token), &error)) {
      fail(error);
    }
    return destination;
  }
  const GString uuid(g_uuid_string_random());
  const auto temporary = parent.location().child(".luciole-create-" + std::string(uuid.get()));
  const GioPtr<GFileOutputStream> stream(
      g_file_create(gioFile(temporary).get(), G_FILE_CREATE_PRIVATE, gioCancellable(token), &error)
  );
  if (!stream) {
    fail(error);
  }
  std::optional<ProviderItem> owned;
  try {
    owned.emplace(temporary, CancelToken{});
    write(G_OUTPUT_STREAM(stream.get()));
    if (!g_output_stream_close(G_OUTPUT_STREAM(stream.get()), gioCancellable(token), &error)) {
      fail(error);
    }
    parent.requireCurrent(token, false);
    owned->requireCurrent(token, false);
    move(temporary, destination, token);
    return destination;
  } catch (...) {
    const auto original = currentFileError();
    (void)g_output_stream_close(G_OUTPUT_STREAM(stream.get()), nullptr, nullptr);
    try {
      if (!owned) {
        throwFileError(FileErrorKind::Changed, "The temporary file could not be identified.");
      }
      parent.requireCurrent(CancelToken{}, false);
      erase(*owned, CancelToken{});
    } catch (...) {
      throwFileError(FileErrorKind::Other, original.message + " Temporary data remains at " + temporary.uri());
    }
    throw FileErrorException(original);
  }
}

luciole::Location
luciole::renameProviderItem(const ProviderItem& source, const std::string& name, const CancelToken& token) {
  nameCheck(name);
  const auto parentLocation = source.location().parent();
  if (!parentLocation) {
    throwFileError(FileErrorKind::InvalidArgument, "The filesystem root cannot be renamed.");
  }
  const ProviderItem parent(*parentLocation, token, true);
  const auto destination = parentLocation->child(name);
  source.requireCurrent(token);
  parent.requireCurrent(token, false);
  if (destination != source.location()) {
    move(source.location(), destination, token);
  }
  return destination;
}

void luciole::deleteProviderTree(const ProviderItem& root, const CancelToken& token, bool& changed) {
  struct Entry {
    ProviderItem item;
    std::optional<std::size_t> parent;
  };
  // The explicit stack avoids C++ recursion; snapshots contain no file payloads.
  std::vector<Entry> tree;
  root.requireCurrent(token);
  tree.push_back({ProviderItem(root.location(), token), std::nullopt});
  std::set<std::pair<std::string, std::string>> folders;
  for (std::size_t index = 0; index < tree.size(); ++index) {
    check(token);
    if (!tree[index].item.directory()) {
      continue;
    }
    tree[index].item.requireCurrent(token);
    const auto identity = *readFileIdentity(query(tree[index].item.location(), token, false).get());
    if (!folders.emplace(identity.filesystem, identity.file).second) {
      throwFileError(FileErrorKind::WouldRecurse, "The folder contains a filesystem cycle.");
    }
    const auto location = tree[index].item.location();
    GError* error = nullptr;
    const GioPtr<GFileEnumerator> reader(g_file_enumerate_children(
        gioFile(location).get(), "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, gioCancellable(token), &error
    ));
    if (!reader) {
      fail(error);
    }
    while (true) {
      const GioPtr<GFileInfo> info(g_file_enumerator_next_file(reader.get(), gioCancellable(token), &error));
      if (!info) {
        if (error) {
          fail(error);
        }
        break;
      }
      const char* name = g_file_info_get_name(info.get());
      if (!name
          || !*name
          || std::string_view(name) == "."
          || std::string_view(name) == ".."
          || std::string_view(name).contains('/')) {
        throwFileError(FileErrorKind::InvalidArgument, "The provider returned an invalid filename.");
      }
      tree.push_back({ProviderItem(location.child(name), token), index});
    }
    if (!g_file_enumerator_close(reader.get(), gioCancellable(token), &error)) {
      fail(error);
    }
  }
  for (const auto& entry : tree) {
    entry.item.requireCurrent(token);
  }
  for (std::size_t index = tree.size(); index-- > 0;) {
    for (auto parent = tree[index].parent; parent; parent = tree[*parent].parent) {
      tree[*parent].item.requireCurrent(token, false);
    }
    if (!tree[index].item.directory()) {
      tree[index].item.requireCurrent(token);
    }
    erase(tree[index].item, token);
    changed = true;
  }
}
