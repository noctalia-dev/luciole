#include "fs/directory_session.h"

#include "fs/gio_interop.h"
#include "fs/heap_trim.h"
#include "fs/native_listing.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
  constexpr const char* kAccessAttributes =
      G_FILE_ATTRIBUTE_ACCESS_CAN_READ "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE
                                       "," G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME "," G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH
                                       "," G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE;
  constexpr const char* kFilesystemAttributes = G_FILE_ATTRIBUTE_FILESYSTEM_READONLY
      "," G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE "," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE;
  constexpr const char* kAttributes = G_FILE_ATTRIBUTE_STANDARD_NAME
      "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
      "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK
      "," G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_ACCESS
      "," G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_OWNER_GROUP
      "," G_FILE_ATTRIBUTE_UNIX_MODE "," G_FILE_ATTRIBUTE_TIME_CHANGED
      ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "," G_FILE_ATTRIBUTE_ETAG_VALUE
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_READ "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME "," G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
      "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE "," G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM;
  // Local folders derive icons from the content type instead; see FileIconPool::getLocal.
  constexpr const char* kIconAttribute = G_FILE_ATTRIBUTE_STANDARD_ICON;
  constexpr const char* kTrashAttributes = G_FILE_ATTRIBUTE_TRASH_ORIG_PATH "," G_FILE_ATTRIBUTE_TRASH_DELETION_DATE;
  constexpr int kBatchSize = 64;
  // Delay between a change notification and its rescan; slow scans extend it up to the maximum.
  constexpr std::chrono::milliseconds kMonitorDelay{80};
  constexpr std::chrono::milliseconds kMaxMonitorDelay{2000};
  // A scan that finishes after further changes is discarded, unless nothing was published for this long.
  constexpr std::chrono::seconds kChurnPublishInterval{1};
  // Publishing at least this many entries, or replacing that many, frees enough heap to return it to the system.
  constexpr std::size_t kHeapTrimEntries = 10'000;
  // The first sniffed content types publish after this long; each later batch waits twice as long as the previous
  // one, up to the maximum, so a large folder republishes only a few times.
  constexpr std::chrono::milliseconds kSniffPublishInterval{100};
  constexpr std::chrono::milliseconds kMaxSniffPublishInterval{1600};
  constexpr std::string_view kEntryLimitMessage = "Tree entry limit reached. Open this folder to see all items.";

  void closeEnumerator(GFileEnumerator* enumerator) {
    if (enumerator == nullptr || g_file_enumerator_is_closed(enumerator)) {
      return;
    }
    g_file_enumerator_close_async(
        enumerator, G_PRIORITY_DEFAULT, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
          g_file_enumerator_close_finish(G_FILE_ENUMERATOR(source), result, nullptr);
        },
        nullptr
    );
  }

  struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
  };

  // Session IDs by child name. Published entries already carry both, so the table indexes them (4 bytes a slot)
  // and only keeps the names monitor events changed since that publication.
  class NameIds {
  public:
    void clear() {
      m_entries = {};
      m_slots.clear();
      m_changes.clear();
    }

    // The first entry of a name wins, like the scan that assigned the IDs.
    void index(const luciole::DirectoryEntries& entries) {
      m_entries = entries;
      m_changes.clear();
      m_slots.assign(entries.empty() ? 0 : std::bit_ceil(entries.size() * 2), kEmpty);
      for (std::uint32_t i = 0; i < entries.size(); ++i) {
        const auto slot = probe(entries[i].name);
        if (m_slots[slot] == kEmpty) {
          m_slots[slot] = i;
        }
      }
    }

    std::optional<std::uint64_t> find(std::string_view name) const {
      if (const auto changed = m_changes.find(name); changed != m_changes.end()) {
        return changed->second;
      }
      if (m_slots.empty()) {
        return std::nullopt;
      }
      const auto slot = m_slots[probe(name)];
      return slot == kEmpty ? std::nullopt : std::optional(m_entries[slot].id);
    }

    // The indexed entries were replaced by copies in the same order with the same names and IDs.
    void rebase(const luciole::DirectoryEntries& entries) {
      if (!m_slots.empty()) {
        m_entries = entries;
      }
    }

    // nullopt forgets the name.
    void set(std::string_view name, std::optional<std::uint64_t> id) {
      if (const auto changed = m_changes.find(name); changed != m_changes.end()) {
        changed->second = id;
      } else {
        m_changes.emplace(std::string(name), id);
      }
    }

    // Every name with an ID.
    void forEach(const std::function<void(std::string_view, std::uint64_t)>& visit) const {
      for (const auto slot : m_slots) {
        if (slot != kEmpty && !m_changes.contains(std::string_view(m_entries[slot].name))) {
          visit(m_entries[slot].name, m_entries[slot].id);
        }
      }
      for (const auto& [name, id] : m_changes) {
        if (id) {
          visit(name, *id);
        }
      }
    }

  private:
    static constexpr std::uint32_t kEmpty = UINT32_MAX;

    // The slot holding `name`, or the empty slot where it would go.
    std::size_t probe(std::string_view name) const {
      const auto mask = m_slots.size() - 1;
      for (auto slot = NameHash{}(name)&mask;; slot = (slot + 1) & mask) {
        if (m_slots[slot] == kEmpty || m_entries[m_slots[slot]].name == name) {
          return slot;
        }
      }
    }

    luciole::DirectoryEntries m_entries;
    std::vector<std::uint32_t> m_slots;
    std::unordered_map<std::string, std::optional<std::uint64_t>, NameHash, std::equal_to<>> m_changes;
  };
} // namespace

struct luciole::DirectorySession::State : std::enable_shared_from_this<State> {
  std::size_t entryLimit = 0;
  FileIconPool icons;
  struct Scan {
    GioPtr<GCancellable> cancellable{g_cancellable_new()};
    GioPtr<GFileEnumerator> enumerator;
    std::vector<DirectoryEntry> entries;
    GioPtr<GFile> parent;
    // Shared by every entry of the scan; each entry's location is this folder's child.
    std::shared_ptr<const Location> folder;
    // Local path of a native listing, and the names whose contents its sniffing pass still has to read.
    std::string path;
    std::shared_ptr<const std::vector<std::string>> sniff = std::make_shared<const std::vector<std::string>>();
    std::size_t sniffed = 0;
    std::chrono::milliseconds sniffInterval = kSniffPublishInterval;
    // Identities of this scan's entries by name; the published entries replace State::identities, and this table
    // is dropped then.
    std::unordered_map<std::string, std::uint64_t> identities;
    bool local = false;
    bool trash = false;
    DirectoryMetadata metadata;
    std::optional<FileError> accessError;
    std::optional<FileError> filesystemError;
    bool enumerating = true;
    bool accessPending = true;
    bool filesystemPending = true;
    ~Scan() { closeEnumerator(enumerator.get()); }
  };

  struct Request {
    std::weak_ptr<State> state;
    std::shared_ptr<Scan> scan;
  };

  DirectorySnapshot snapshot;
  std::function<void()> changed;
  std::shared_ptr<Scan> scan;
  GioPtr<GFileMonitor> monitor;
  NameIds identities;
  // Names in the published entries whose content type is still the name-based guess.
  std::unordered_set<std::string> unsniffed;
  std::uint64_t nextId = 1;
  IdResolver resolveId;
  guint timer = 0;
  bool dirty = false;
  bool entriesAccessInvalidated = false;
  std::chrono::steady_clock::time_point scanStarted;
  std::chrono::steady_clock::time_point lastPublished;
  std::chrono::milliseconds lastScanDuration{0};

  ~State() { stop(); }

  void notify() const {
    // A callback may replace itself, navigate, cancel, or destroy its session.
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }

  void stopScan() {
    if (timer != 0) {
      g_source_remove(std::exchange(timer, 0));
    }
    if (scan) {
      g_cancellable_cancel(scan->cancellable.get());
      scan.reset();
    }
    dirty = false;
  }

  void stop() {
    stopScan();
    if (monitor) {
      g_file_monitor_cancel(monitor.get());
      monitor.reset();
    }
  }

  void invalidateMetadata(bool loading) {
    snapshot.metadata = {};
    snapshot.metadataLoading = loading;
    snapshot.accessError.reset();
    snapshot.filesystemError.reset();
    if (!entriesAccessInvalidated && !snapshot.entries.empty()) {
      std::vector<DirectoryEntry> entries(snapshot.entries.begin(), snapshot.entries.end());
      for (auto& entry : entries) {
        entry.access = {};
      }
      snapshot.entries = std::move(entries);
      identities.rebase(snapshot.entries);
      entriesAccessInvalidated = true;
    }
  }

  void publishMetadata() {
    snapshot.metadata = scan->metadata;
    snapshot.metadataLoading = scan->accessPending || scan->filesystemPending;
    snapshot.accessError = scan->accessError;
    snapshot.filesystemError = scan->filesystemError;
  }

  void metadataCompleted() {
    if (!dirty && !scan->enumerating && snapshot.status == DirectoryStatus::Ready) {
      publishMetadata();
      notify();
    }
  }

  void queryMetadata() {
    const auto file = gioFile(*snapshot.location);
    g_file_query_info_async(
        file.get(), kAccessAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          const GioPtr<GFileInfo> info(g_file_query_info_finish(G_FILE(source), result, &error));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            request->scan->accessPending = false;
            if (info) {
              request->scan->metadata.access = readFileAccess(info.get());
            } else if (error != nullptr) {
              request->scan->accessError = fileError(*error);
            }
            state->metadataCompleted();
          }
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    g_file_query_filesystem_info_async(
        file.get(), kFilesystemAttributes, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          const GioPtr<GFileInfo> info(g_file_query_filesystem_info_finish(G_FILE(source), result, &error));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            request->scan->filesystemPending = false;
            if (info) {
              request->scan->metadata.filesystem = readFilesystemInfo(info.get());
            } else if (error != nullptr) {
              request->scan->filesystemError = fileError(*error);
            }
            state->metadataCompleted();
          }
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
  }

  void schedule() {
    if (timer != 0) {
      return;
    }
    // A folder that changes continuously is rescanned at most every other scan duration, so scanning cannot
    // consume the whole main thread.
    const auto delay = std::clamp(2 * lastScanDuration, kMonitorDelay, kMaxMonitorDelay);
    timer = g_timeout_add_full(
        G_PRIORITY_DEFAULT, static_cast<guint>(delay.count()),
        [](gpointer data) -> gboolean {
          if (const auto state = static_cast<std::weak_ptr<State>*>(data)->lock()) {
            state->timer = 0;
            state->begin();
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
  }

  void monitorChanged(GFile* file, GFile* other, GFileMonitorEvent event) {
    // Identities are keyed by child name; events about the folder itself or other locations name no child.
    const auto folder = gioFile(*snapshot.location);
    const auto childName = [&](GFile* child) {
      return GString(g_file_has_parent(child, folder.get()) != FALSE ? g_file_get_basename(child) : nullptr);
    };
    const auto name = childName(file);
    // Only an explicit same-directory rename proves continuity. Replacements
    // and delete/create sequences get a new identity even at the same name.
    if (event == G_FILE_MONITOR_EVENT_RENAMED && other != nullptr) {
      const auto id = name ? identities.find(name.get()) : std::nullopt;
      if (name) {
        identities.set(name.get(), std::nullopt);
      }
      if (const auto otherName = childName(other)) {
        identities.set(otherName.get(), id);
      }
    } else if (
        name
        && (event == G_FILE_MONITOR_EVENT_DELETED
            || event == G_FILE_MONITOR_EVENT_MOVED_OUT
            || event == G_FILE_MONITOR_EVENT_CREATED
            || event == G_FILE_MONITOR_EVENT_MOVED_IN)
    ) {
      identities.set(name.get(), std::nullopt);
    }
    const bool wasDirty = dirty;
    dirty = true;
    if (!scan || !scan->enumerating) {
      schedule();
      if (!wasDirty) {
        invalidateMetadata(true);
        notify();
      }
    }
  }

  void watch() {
    const auto file = gioFile(*snapshot.location);
    if (!g_file_is_native(file.get())) {
      snapshot.monitorError = FileError{
          .kind = FileErrorKind::Unsupported,
          .message = snapshot.location->isTrash() ? "Refresh to update the Trash view."
                                                  : "Remote change monitoring is not implemented yet."
      };
      return;
    }
    GError* error = nullptr;
    monitor.reset(g_file_monitor_directory(file.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, &error));
    if (error != nullptr) {
      snapshot.monitorError = fileError(*error);
      g_error_free(error);
      return;
    }
    g_signal_connect(
        monitor.get(), "changed",
        G_CALLBACK(+[](GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent event, gpointer data) {
          // Keep State alive if its observer destroys the session.
          const auto state = static_cast<State*>(data)->shared_from_this();
          state->monitorChanged(file, other, event);
        }),
        this
    );
  }

  void begin() {
    stopScan();
    scanStarted = std::chrono::steady_clock::now();
    ++snapshot.generation;
    snapshot.status = DirectoryStatus::Loading;
    snapshot.contentTypesPending = false;
    snapshot.error.reset();
    invalidateMetadata(true);
    scan = std::make_shared<Scan>();
    scan->parent = gioFile(*snapshot.location);
    scan->folder = std::make_shared<const Location>(*snapshot.location);
    scan->local =
        g_file_is_native(scan->parent.get()) != FALSE && g_file_has_uri_scheme(scan->parent.get(), "file") != FALSE;
    scan->trash = snapshot.location->isTrash();
    const GString localPath(scan->local ? g_file_get_path(scan->parent.get()) : nullptr);
    scan->local = scan->local && localPath;
    icons.setLocalFolder(localPath.get());
    queryMetadata();
    if (scan->local) {
      listNatively(localPath.get());
    } else {
      enumerate();
    }
    notify();
  }

  // Local folders: readdir and statx on a worker, published in one step.
  void listNatively(std::string path) {
    scan->path = path;
    struct Job {
      std::string path;
      std::size_t limit = 0;
      NativeListing listing;
    };
    auto* task = g_task_new(
        nullptr, scan->cancellable.get(),
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          auto& job = *static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            state->listed(std::move(job.listing));
          }
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    g_task_set_task_data(task, new Job{.path = std::move(path), .limit = entryLimit, .listing = {}}, [](gpointer data) {
      delete static_cast<Job*>(data);
    });
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
      auto& job = *static_cast<Job*>(data);
      job.listing = listNativeFolder(job.path, job.limit, cancellable);
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }

  void listed(NativeListing listing) {
    if (listing.unsupported) {
      enumerate();
      return;
    }
    // A folder lists each name once, so IDs come straight from the published table without a scan table.
    for (auto& entry : listing.entries) {
      if (resolveId) {
        entry.id = resolveId(entry);
      } else if (const auto previous = identities.find(entry.name)) {
        entry.id = *previous;
      } else {
        entry.id = nextId++;
      }
    }
    if (!listing.uncertain.empty()) {
      keepSniffedTypes(listing);
    }
    scan->entries = std::move(listing.entries);
    const auto listedScan = scan;
    if (listing.limited) {
      complete(FileError{FileErrorKind::Unsupported, std::string(kEntryLimitMessage)});
    } else {
      complete(std::nullopt);
    }
    if (scan && scan == listedScan && !snapshot.error && !scan->sniff->empty()) {
      unsniffed = {scan->sniff->begin(), scan->sniff->end()};
      sniffContents();
    }
  }

  // Uncertain entries reuse the published type of an unchanged file; the others wait for sniffContents().
  void keepSniffedTypes(NativeListing& listing) {
    std::unordered_map<std::string_view, const DirectoryEntry*> previous;
    previous.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
      previous.emplace(entry.name, &entry);
    }
    auto names = std::make_shared<std::vector<std::string>>();
    for (const auto index : listing.uncertain) {
      auto& entry = listing.entries[index];
      const auto found = previous.find(entry.name);
      if (found != previous.end()
          && !unsniffed.contains(entry.name)
          && found->second->type == entry.type
          && found->second->size == entry.size
          && found->second->modifiedSeconds == entry.modifiedSeconds
          && found->second->contentRevision == entry.contentRevision) {
        entry.contentType = found->second->contentType;
        entry.icon = found->second->icon;
      } else {
        names->push_back(entry.name);
      }
    }
    scan->sniff = std::move(names);
  }

  // Reads the contents of uncertain entries on a worker, publishing GIO's sniffed types about every 100 ms.
  void sniffContents() {
    struct Job {
      std::string folder;
      std::shared_ptr<const std::vector<std::string>> names;
      std::size_t begin = 0;
      std::chrono::milliseconds interval{0};
      std::vector<std::optional<std::string>> types;
    };
    auto* task = g_task_new(
        nullptr, scan->cancellable.get(),
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          auto& job = *static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            state->sniffed(job.begin, job.types);
          }
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    g_task_set_task_data(
        task,
        new Job{
            .folder = scan->path,
            .names = scan->sniff,
            .begin = scan->sniffed,
            .interval = std::exchange(scan->sniffInterval, std::min(2 * scan->sniffInterval, kMaxSniffPublishInterval)),
            .types = {}
        },
        [](gpointer data) { delete static_cast<Job*>(data); }
    );
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
      auto& job = *static_cast<Job*>(data);
      const auto deadline = std::chrono::steady_clock::now() + job.interval;
      const std::string_view separator = job.folder.ends_with('/') ? "" : "/";
      for (auto i = job.begin; i < job.names->size() && g_cancellable_is_cancelled(cancellable) == FALSE; ++i) {
        job.types.push_back(sniffContentType(std::string(job.folder).append(separator).append((*job.names)[i])));
        if (std::chrono::steady_clock::now() >= deadline) {
          break;
        }
      }
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }

  void sniffed(std::size_t begin, const std::vector<std::optional<std::string>>& types) {
    std::unordered_map<std::string_view, const std::string*> found;
    for (std::size_t i = 0; i < types.size(); ++i) {
      const auto& name = (*scan->sniff)[begin + i];
      unsniffed.erase(name);
      if (types[i]) {
        found.emplace(name, &*types[i]);
      }
    }
    std::vector<DirectoryEntry> entries(snapshot.entries.begin(), snapshot.entries.end());
    bool changed = false;
    for (auto& entry : entries) {
      const auto type = found.find(entry.name);
      if (type == found.end() || (entry.contentType && *entry.contentType == *type->second)) {
        continue;
      }
      entry.contentType = InternedText(*type->second);
      entry.icon = icons.getLocal(entry.name.c_str(), G_FILE_TYPE_REGULAR, type->second->c_str(), *scan->folder);
      changed = true;
    }
    const auto current = scan;
    current->sniffed = begin + types.size();
    const bool done = types.empty() || current->sniffed >= current->sniff->size();
    if (changed || done) {
      if (changed) {
        snapshot.entries = std::move(entries);
        identities.rebase(snapshot.entries);
      }
      snapshot.contentTypesPending = !done;
      notify();
    }
    // The observer may have navigated away.
    if (!done && scan == current) {
      sniffContents();
    }
  }

  // A name keeps its previous ID within this session; new names get new ones.
  std::uint64_t identify(const DirectoryEntry& entry) {
    if (resolveId) {
      return resolveId(entry);
    }
    const auto [identity, inserted] = scan->identities.try_emplace(entry.name, 0);
    if (inserted) {
      identity->second = identities.find(identity->first).value_or(0);
      if (identity->second == 0) {
        identity->second = nextId++;
      }
    }
    return identity->second;
  }

  void enumerate() {
    std::string attributes = kAttributes;
    if (!scan->local) {
      attributes.append(",").append(kIconAttribute);
    }
    if (scan->trash) {
      attributes.append(",").append(kTrashAttributes);
    }
    g_file_enumerate_children_async(
        scan->parent.get(), attributes.c_str(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
        scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          request->scan->enumerator.reset(g_file_enumerate_children_finish(G_FILE(source), result, &error));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            if (error != nullptr) {
              state->complete(fileError(*error));
            } else {
              state->nextBatch();
            }
          }
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
  }

  void append(GFileInfo* info) {
    const char* name = g_file_info_get_name(info);
    DirectoryEntry entry{
        .id = 0,
        .place = EntryPlace::inFolder(scan->folder),
        .name = name,
        .type = fileType(g_file_info_get_file_type(info)),
        .hidden = infoBoolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN).value_or(false) || name[0] == '.',
        .symlink = readIsSymlink(info) != FALSE,
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt,
        .access = readFileAccess(info),
    };
    entry.setDisplayName(g_file_info_get_display_name(info));
    entry.icon = scan->local ? icons.getLocal(
                                   name, g_file_info_get_file_type(info),
                                   infoString(info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE), *scan->folder
                               )
                             : icons.get(info, *scan->folder);
    if (scan->trash) {
      auto trash = readTrashInfo(info);
      entry.setParentDisplayName(trashOriginalParentText(trash));
      entry.setTrash(std::move(trash));
    }
    entry.details = readEntryMetadata(info);
    entry.setIdentity(readFileIdentity(info));
    entry.contentType = readContentType(info);
    if (const auto size = infoUint64(info, G_FILE_ATTRIBUTE_STANDARD_SIZE); size && *size <= G_MAXINT64) {
      entry.size = size;
    }
    entry.contentRevision = readChangeRevision(info);
    entry.modifiedSeconds = infoUint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    // Unlike g_file_info_get_etag, absent on children GIO could not stat is not a programming error here.
    if (const char* etag = infoString(info, G_FILE_ATTRIBUTE_ETAG_VALUE); etag != nullptr) {
      entry.setEtag(etag);
    }
    entry.id = identify(entry);
    scan->entries.push_back(std::move(entry));
  }

  void nextBatch() {
    g_file_enumerator_next_files_async(
        scan->enumerator.get(), kBatchSize, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          GList* infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), result, &error);
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            if (error != nullptr) {
              state->complete(fileError(*error));
            } else if (infos == nullptr) {
              state->complete(std::nullopt);
            } else {
              // Request the next batch before converting this one, so GIO's worker overlaps this thread's work.
              // A batch that can reach the entry limit decides first whether another request is needed.
              const bool prefetch =
                  state->entryLimit == 0 || state->scan->entries.size() + g_list_length(infos) < state->entryLimit;
              if (prefetch) {
                state->nextBatch();
              }
              bool limited = false;
              for (const GList* item = infos; item != nullptr; item = item->next) {
                if (state->entryLimit && state->scan->entries.size() >= state->entryLimit) {
                  limited = true;
                  break;
                }
                state->append(G_FILE_INFO(item->data));
              }
              if (limited) {
                state->complete(FileError{FileErrorKind::Unsupported, std::string(kEntryLimitMessage)});
              } else if (!prefetch) {
                state->nextBatch();
              }
            }
          }
          g_list_free_full(infos, g_object_unref);
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
  }

  // Prefers the identities monitor events maintained during this scan, keeps the scan's IDs unique, and keeps the
  // live identities of names the scan missed for the follow-up rescan.
  void reconcileChurnedIdentities() {
    std::unordered_set<std::uint64_t> used;
    used.reserve(scan->entries.size());
    for (auto& entry : scan->entries) {
      if (const auto live = identities.find(entry.name)) {
        entry.id = *live;
      }
      if (!used.insert(entry.id).second) {
        entry.id = nextId++;
        used.insert(entry.id);
      }
      scan->identities.insert_or_assign(entry.name, entry.id);
    }
    identities.forEach([&](std::string_view name, std::uint64_t id) {
      if (!used.contains(id)) {
        scan->identities.try_emplace(std::string(name), id);
      }
    });
  }

  void complete(std::optional<FileError> error) {
    const auto now = std::chrono::steady_clock::now();
    lastScanDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - scanStarted);
    const bool churned = dirty;
    if (churned && now - lastPublished < kChurnPublishInterval) {
      stopScan();
      schedule();
      return;
    }
    // A folder that keeps changing still publishes its latest complete scan at that interval, then rescans.
    if (churned && !resolveId) {
      reconcileChurnedIdentities();
    }
    lastPublished = now;
    const auto replaced = snapshot.entries.size();
    snapshot.entries = std::move(scan->entries);
    unsniffed.clear();
    snapshot.contentTypesPending = !error && !scan->sniff->empty();
    const bool trim = std::max(replaced, snapshot.entries.size()) >= kHeapTrimEntries;
    entriesAccessInvalidated = false;
    scan->enumerating = false;
    closeEnumerator(scan->enumerator.get());
    scan->enumerator.reset();
    if (!resolveId) {
      // The published entries carry the scan's names and IDs; only other names stay in the change table.
      identities.index(snapshot.entries);
      for (const auto& [name, id] : scan->identities) {
        if (identities.find(name) != id) {
          identities.set(name, id);
        }
      }
      scan->identities = {};
    }
    snapshot.error = std::move(error);
    if (snapshot.location->isTrash() && snapshot.error && snapshot.error->kind == FileErrorKind::Unsupported) {
      snapshot.error->message = "Trash browsing is unavailable. The desktop trash service is not accessible.";
    }
    snapshot.status = snapshot.error ? DirectoryStatus::Error : DirectoryStatus::Ready;
    if (snapshot.error) {
      g_cancellable_cancel(scan->cancellable.get());
      invalidateMetadata(false);
    } else {
      publishMetadata();
    }
    const auto published = scan;
    notify();
    if (trim) {
      scheduleHeapTrim();
    }
    // The observer may have navigated; only this scan's changes still need a rescan.
    if (churned && scan == published) {
      schedule();
    }
  }
};

luciole::DirectorySession::DirectorySession(std::size_t entryLimit, IdResolver resolveId)
    : m_state(std::make_shared<State>()) {
  m_state->entryLimit = entryLimit;
  m_state->resolveId = std::move(resolveId);
}

luciole::DirectorySession::~DirectorySession() {
  m_state->changed = {};
  m_state->stop();
}

void luciole::DirectorySession::open(const Location& location) {
  const auto state = m_state;
  state->stop();
  state->identities.clear();
  state->snapshot.location = location;
  state->snapshot.entries = {};
  state->entriesAccessInvalidated = false;
  state->snapshot.monitorError.reset();
  state->watch();
  state->begin();
}

void luciole::DirectorySession::refresh() {
  const auto state = m_state;
  if (state->snapshot.location) {
    if (!state->monitor) {
      state->snapshot.monitorError.reset();
      state->watch();
      // Changes before this new watch were not observed.
      state->identities.clear();
    }
    state->begin();
  }
}

void luciole::DirectorySession::cancel() {
  const auto state = m_state;
  state->stop();
  state->identities.clear();
  ++state->snapshot.generation;
  state->snapshot.status = DirectoryStatus::Cancelled;
  state->snapshot.contentTypesPending = false;
  state->snapshot.error.reset();
  state->invalidateMetadata(false);
  state->notify();
}

void luciole::DirectorySession::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}

const luciole::DirectorySnapshot& luciole::DirectorySession::snapshot() const { return m_state->snapshot; }
