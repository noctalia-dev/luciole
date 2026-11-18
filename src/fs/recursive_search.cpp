#include "fs/recursive_search.h"

#include "fs/file_visibility.h"
#include "fs/gio_interop.h"
#include "fs/name_matching.h"

#include <deque>
#include <unordered_set>
#include <utility>

namespace {
  constexpr const char* kAttributes =
      "standard::name,standard::display-name,standard::type,standard::is-hidden,standard::is-symlink,"
      "standard::icon,standard::size,time::modified,etag::value,id::file,id::filesystem,"
      "time::access,time::created,owner::user,owner::group,unix::mode,"
      "access::can-read,access::can-write,access::can-execute,access::can-rename,access::can-trash,access::can-delete,"
      "trash::orig-path,trash::deletion-date,standard::content-type,standard::fast-content-type";
  constexpr unsigned int kMaxDepth = 256;
  constexpr std::size_t kMaxPending = 65536;

  void closeEnumerator(GFileEnumerator* enumerator) {
    if (enumerator != nullptr && !g_file_enumerator_is_closed(enumerator)) {
      g_file_enumerator_close_async(
          enumerator, G_PRIORITY_DEFAULT, nullptr,
          [](GObject* source, GAsyncResult* result, gpointer) {
            g_file_enumerator_close_finish(G_FILE_ENUMERATOR(source), result, nullptr);
          },
          nullptr
      );
    }
  }
} // namespace

struct luciole::RecursiveSearch::State : std::enable_shared_from_this<State> {
  struct Folder {
    Location location;
    unsigned int depth = 0;
    // Shared by every result found in this folder; set when the folder becomes current.
    std::shared_ptr<const Location> shared = nullptr;
  };
  struct Scan {
    GioPtr<GCancellable> cancellable{g_cancellable_new()};
    GioPtr<GFileEnumerator> enumerator;
    std::deque<Folder> pending;
    std::optional<Folder> current;
    std::optional<Location> root;
    std::unordered_set<std::string> visited;
    std::string query;
    std::vector<DirectoryEntry> entries;
    bool showHidden = false;
    bool showBackups = true;
    ~Scan() { closeEnumerator(enumerator.get()); }
  };
  struct Request {
    std::shared_ptr<State> owner;
    std::shared_ptr<Scan> scan;
  };

  DirectorySnapshot snapshot;
  std::size_t skipped = 0;
  std::uint64_t nextId = std::uint64_t{1} << 63;
  std::function<void()> changed;
  std::shared_ptr<Scan> active;

  bool current(const std::shared_ptr<Scan>& scan) const {
    return active == scan && !g_cancellable_is_cancelled(scan->cancellable.get());
  }
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void cancel() {
    if (active) {
      g_cancellable_cancel(active->cancellable.get());
      active.reset();
    }
  }
  void next(const std::shared_ptr<Scan>& scan) {
    if (!current(scan)) {
      return;
    }
    closeEnumerator(scan->enumerator.get());
    scan->enumerator.reset();
    if (scan->pending.empty()) {
      scan->visited.clear();
      scan->current.reset();
      snapshot.status = DirectoryStatus::Ready;
      notify();
      return;
    }
    scan->current = std::move(scan->pending.front());
    scan->pending.pop_front();
    scan->current->shared = std::make_shared<const Location>(scan->current->location);
    const auto file = gioFile(scan->current->location);
    auto* request = new Request{shared_from_this(), scan};
    g_file_enumerate_children_async(
        file.get(), kAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children_finish(G_FILE(source), result, &error));
          auto& owner = *request->owner;
          if (!owner.current(request->scan)) {
            closeEnumerator(enumerator.get());
            g_clear_error(&error);
            return;
          }
          if (error != nullptr) {
            owner.failed(request->scan, *error);
            g_error_free(error);
            return;
          }
          request->scan->enumerator = std::move(enumerator);
          owner.batch(request->scan);
        },
        request
    );
  }
  void failed(const std::shared_ptr<Scan>& scan, const GError& error) {
    if (scan->current->depth == 0) {
      closeEnumerator(scan->enumerator.get());
      scan->enumerator.reset();
      scan->pending.clear();
      snapshot.error = fileError(error);
      snapshot.status = DirectoryStatus::Error;
      notify();
    } else {
      ++skipped;
      notify();
      next(scan);
    }
  }
  void batch(const std::shared_ptr<Scan>& scan) {
    if (!current(scan)) {
      return;
    }
    auto* request = new Request{shared_from_this(), scan};
    g_file_enumerator_next_files_async(
        scan->enumerator.get(), 64, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          GList* infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), result, &error);
          auto& owner = *request->owner;
          if (owner.current(request->scan)) {
            if (error != nullptr) {
              owner.failed(request->scan, *error);
            } else if (infos == nullptr) {
              owner.next(request->scan);
            } else {
              for (GList* item = infos; item != nullptr; item = item->next) {
                owner.consume(request->scan, G_FILE_INFO(item->data));
              }
              if (!request->scan->entries.empty()) {
                owner.snapshot.entries = DirectoryEntries::concatenate(
                    {owner.snapshot.entries, DirectoryEntries(std::move(request->scan->entries))}
                );
                request->scan->entries.clear();
              }
              owner.notify();
              owner.batch(request->scan);
            }
          }
          g_list_free_full(infos, g_object_unref);
          g_clear_error(&error);
        },
        request
    );
  }
  void consume(const std::shared_ptr<Scan>& scan, GFileInfo* info) {
    const char* name = g_file_info_get_name(info);
    if (name == nullptr) {
      return;
    }
    const bool hidden = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) || name[0] == '.';
    if (hidden && !scan->showHidden) {
      return;
    }
    const auto parent = gioFile(scan->current->location);
    const GioPtr<GFile> child(g_file_get_child(parent.get(), name));
    const GString uri(g_file_get_uri(child.get()));
    const auto location = Location::fromUri(uri.get());
    const auto type = fileType(g_file_info_get_file_type(info));
    if (!scan->showBackups && isEditorBackup(name, type)) {
      return;
    }
    const bool symlink = readIsSymlink(info);
    if (type == FileType::Directory && !symlink) {
      const char* identity = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE);
      const char* filesystem = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
      const auto key =
          identity != nullptr && filesystem != nullptr ? std::string(filesystem) + '\n' + identity : location.uri();
      if (scan->visited.insert(key).second) {
        if (scan->current->depth >= kMaxDepth || scan->pending.size() >= kMaxPending) {
          ++skipped;
        } else {
          scan->pending.push_back({location, scan->current->depth + 1});
        }
      }
    }
    const char* display = g_file_info_get_display_name(info);
    const GString fallback(display == nullptr ? g_filename_display_name(name) : nullptr);
    std::string displayName = display != nullptr ? display : fallback.get();
    if (!foldedDisplayText(displayName).contains(scan->query)) {
      return;
    }
    const GString relative(g_file_get_relative_path(gioFile(*scan->root).get(), parent.get()));
    const GString parentName(g_filename_display_name(relative && *relative.get() != '\0' ? relative.get() : "."));
    DirectoryEntry entry{
        .id = nextId++,
        .place = EntryPlace::inFolder(scan->current->shared),
        .name = name,
        .type = type,
        .hidden = hidden,
        .symlink = symlink,
        .size = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) && g_file_info_get_size(info) >= 0
            ? std::optional<std::uint64_t>(g_file_info_get_size(info))
            : std::nullopt,
        .modifiedSeconds = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)
            ? std::optional<std::uint64_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
            : std::nullopt,
        .access = readFileAccess(info),
        .icon = std::make_shared<const FileIconSource>(readFileIcon(info, location)),
        .contentType = readContentType(info),
        .details = readEntryMetadata(info)
    };
    entry.setDisplayName(std::move(displayName));
    entry.setParentDisplayName(parentName.get());
    entry.setIdentity(readFileIdentity(info));
    if (const char* etag = infoString(info, G_FILE_ATTRIBUTE_ETAG_VALUE)) {
      entry.setEtag(etag);
    }
    if (location.isTrash()) {
      entry.setTrash(readTrashInfo(info));
    }
    scan->entries.push_back(std::move(entry));
  }
};

luciole::RecursiveSearch::RecursiveSearch() : m_state(std::make_shared<State>()) {}
luciole::RecursiveSearch::~RecursiveSearch() {
  m_state->changed = {};
  m_state->cancel();
}
void luciole::RecursiveSearch::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
const luciole::DirectorySnapshot& luciole::RecursiveSearch::snapshot() const { return m_state->snapshot; }
std::size_t luciole::RecursiveSearch::skippedFolders() const { return m_state->skipped; }
void luciole::RecursiveSearch::start(
    const Location& root, const std::string& query, bool showHidden, bool showBackups
) {
  const auto destination = root; // NOLINT(performance-unnecessary-copy-initialization)
  const auto state = m_state;
  state->cancel();
  auto scan = std::make_shared<State::Scan>();
  scan->query = foldedDisplayText(query);
  scan->root = destination;
  scan->showHidden = showHidden;
  scan->showBackups = showBackups;
  scan->pending.push_back({destination, 0});
  state->active = scan;
  state->snapshot = {
      .generation = state->snapshot.generation + 1,
      .location = destination,
      .status = DirectoryStatus::Loading,
      .entries = {},
      .error = std::nullopt,
      .monitorError = std::nullopt,
      .incrementalResults = true
  };
  state->skipped = 0;
  state->notify();
  state->next(scan);
}
void luciole::RecursiveSearch::cancel() {
  m_state->cancel();
  m_state->snapshot.status = DirectoryStatus::Cancelled;
  m_state->notify();
}
