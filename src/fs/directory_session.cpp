#include "fs/directory_session.h"

#include "fs/background_job.h"
#include "fs/custom_icons.h"
#include "fs/gio_interop.h"
#include "fs/heap_trim.h"
#include "fs/native_listing.h"
#include "fs/network_listing.h"
#include "fs/system_mounts.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
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
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME "," G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE
      "," G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM;
  // Mounted network shares omit per-child access(R/W/X) and content sniffing: on NFS/SMB those send synchronous
  // ACCESS and OPEN/READ RPCs per entry instead of letting READDIRPLUS satisfy enumeration.
  constexpr const char* kProbeAttributes =
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_READ "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE
      "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE;
  std::string browseAttributes(std::uint8_t details, bool network) {
    std::string attributes = G_FILE_ATTRIBUTE_STANDARD_NAME
        "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
        "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK
        "," G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_CHANGED
        ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "," G_FILE_ATTRIBUTE_ETAG_VALUE
        "," G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME "," G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH
        "," G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE
        "," G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM;
    if (network) {
      // Without access::can-execute, the context menu's Run entry falls back to the mode's execute bits.
      details |= luciole::EntryDetails::Permissions;
    } else {
      attributes.append(kProbeAttributes);
    }
    for (const auto& [flag, attribute] :
         {std::pair{luciole::EntryDetails::Accessed, G_FILE_ATTRIBUTE_TIME_ACCESS},
          std::pair{luciole::EntryDetails::Created, G_FILE_ATTRIBUTE_TIME_CREATED},
          std::pair{luciole::EntryDetails::Owner, G_FILE_ATTRIBUTE_OWNER_USER},
          std::pair{luciole::EntryDetails::Group, G_FILE_ATTRIBUTE_OWNER_GROUP},
          std::pair{luciole::EntryDetails::Permissions, G_FILE_ATTRIBUTE_UNIX_MODE}}) {
      if ((details & flag) != 0) {
        attributes.append(",").append(attribute);
      }
    }
    return attributes;
  }
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
  // The icon pass publishes and gives up its link worker after this long or this many entries, then queues again
  // behind the other passes; each later slice may run twice as long, up to the maximum.
  constexpr std::chrono::milliseconds kIconSliceInterval{100};
  constexpr std::chrono::milliseconds kMaxIconSliceInterval{1600};
  constexpr std::size_t kIconSliceEntries = 4096;
  // Following a link can block without end on an unresponsive network or FUSE mount, and cancellation cannot
  // interrupt it. The link passes of every session share these workers, so hung targets hold at most this many of
  // GIO's pool threads.
  constexpr std::size_t kLinkWorkers = 2;
  // Beyond this many waiting passes the oldest is dropped, and its links keep the kind they were published with.
  constexpr std::size_t kMaxWaitingLinkPasses = 64;
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

  // Bounds the link passes that run at once; later passes wait for a free worker. Main thread only.
  class LinkWorkers {
  public:
    // Returns false when its pass is no longer wanted. A started pass calls finished() once its worker returns.
    using Start = std::move_only_function<bool()>;

    static LinkWorkers& shared() {
      static LinkWorkers workers;
      return workers;
    }

    void run(Start start) {
      if (m_waiting.size() >= kMaxWaitingLinkPasses) {
        m_waiting.pop_front();
      }
      m_waiting.push_back(std::move(start));
      startWaiting();
    }

    void finished() {
      --m_running;
      startWaiting();
    }

  private:
    void startWaiting() {
      while (m_running < kLinkWorkers && !m_waiting.empty()) {
        auto start = std::move(m_waiting.front());
        m_waiting.pop_front();
        if (start()) {
          ++m_running;
        }
      }
    }

    std::size_t m_running = 0;
    std::deque<Start> m_waiting;
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
        const auto slot = probe(entries.brief(i).name);
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
      return slot == kEmpty ? std::nullopt : std::optional(m_entries.brief(slot).id);
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
        if (slot != kEmpty && !m_changes.contains(std::string_view(m_entries.brief(slot).name))) {
          visit(m_entries.brief(slot).name, m_entries.brief(slot).id);
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
        if (m_slots[slot] == kEmpty || m_entries.brief(m_slots[slot]).name == name) {
          return slot;
        }
      }
    }

    luciole::DirectoryEntries m_entries;
    std::vector<std::uint32_t> m_slots;
    std::unordered_map<std::string, std::optional<std::uint64_t>, NameHash, std::equal_to<>> m_changes;
  };
} // namespace

namespace luciole {
  luciole::DirectoryEntry readEntry(
      GFileInfo* info, const std::shared_ptr<const luciole::Location>& folder, luciole::FileIconPool& icons, bool local,
      bool trashFolder
  ) {
    const char* name = g_file_info_get_name(info);
    DirectoryEntry entry{
        .id = 0,
        .place = EntryPlace::inFolder(folder),
        .name = name,
        .type = fileType(g_file_info_get_file_type(info)),
        .hidden = infoBoolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN).value_or(false) || name[0] == '.',
        .symlink = readIsSymlink(info) != FALSE,
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt,
        .access = readFileAccess(info),
    };
    entry.setDisplayName(g_file_info_get_display_name(info));
    if (entry.type == FileType::Shortcut) {
      if (const char* target = infoString(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI)) {
        try {
          entry.setShortcutTarget(Location::fromUri(target));
        } catch (const std::invalid_argument&) {
          // A malformed provider target stays non-navigable; it is never guessed from display text.
        }
      }
    }
    entry.contentType = readContentType(info);
    entry.icon = local
        ? icons.getLocal(
              name, g_file_info_get_file_type(info), entry.contentType ? entry.contentType->c_str() : nullptr, *folder
          )
        : icons.get(info, *folder);
    if (trashFolder) {
      auto trash = readTrashInfo(info);
      entry.setParentDisplayName(trashOriginalParentText(trash));
      entry.setTrash(std::move(trash));
    }
    entry.details = readEntryMetadata(info);
    entry.setIdentity(readFileIdentity(info));
    if (const auto size = infoUint64(info, G_FILE_ATTRIBUTE_STANDARD_SIZE); size && *size <= G_MAXINT64) {
      entry.size = size;
    }
    entry.contentRevision = readChangeRevision(info);
    entry.modifiedSeconds = infoUint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    // Unlike g_file_info_get_etag, absent on children GIO could not stat is not a programming error here.
    if (const char* etag = infoString(info, G_FILE_ATTRIBUTE_ETAG_VALUE); etag != nullptr) {
      entry.setEtag(etag);
    }
    return entry;
  }
} // namespace luciole

struct luciole::DirectorySession::State : std::enable_shared_from_this<State> {
  std::size_t entryLimit = 0;
  bool catalogueEnabled = false;
  guint factsIdle = 0;
  std::unordered_map<std::size_t, std::uint8_t> factsRequested;
  std::deque<std::size_t> factsOrder;
  FileIconPool icons;
  struct Scan {
    CancelToken preparation;
    GioPtr<GCancellable> cancellable{g_cancellable_new()};
    GioPtr<GFileEnumerator> enumerator;
    std::vector<DirectoryEntry> entries;
    std::optional<BrowseCatalogue> catalogue;
    bool factsRunning = false;
    std::unordered_map<std::size_t, std::uint8_t> factsInFlight;
    GioPtr<GFile> parent;
    // Shared by every entry of the scan; each entry's location is this folder's child.
    std::shared_ptr<const Location> folder;
    // Local path of a native listing, and the names whose contents its sniffing pass still has to read.
    std::string path;
    std::shared_ptr<const std::vector<std::string>> sniff = std::make_shared<const std::vector<std::string>>();
    std::size_t sniffed = 0;
    std::chrono::milliseconds sniffInterval = kSniffPublishInterval;
    // A native listing's symlinks by index, and the indices of those that lead to folders.
    std::shared_ptr<const std::vector<std::pair<std::size_t, std::string>>> links =
        std::make_shared<const std::vector<std::pair<std::size_t, std::string>>>();
    std::unordered_set<std::size_t> directoryLinks;
    // A native listing's folders by name, for the icon pass. Links get their icons from the link pass, which already
    // follows them, so a link to a hung mount blocks one worker rather than both.
    std::shared_ptr<const std::vector<std::string>> iconCandidates = std::make_shared<const std::vector<std::string>>();
    // How many candidates the icon pass has read, the names among them with an icon, and those icons, shared between
    // entries with equal ones.
    std::size_t iconsChecked = 0;
    std::unordered_set<std::string, NameHash, std::equal_to<>> iconNames;
    std::vector<SharedFileIcon> iconShared;
    std::chrono::milliseconds iconInterval = kIconSliceInterval;
    // Identities of this scan's entries by name; the published entries replace State::identities, and this table
    // is dropped then.
    std::unordered_map<std::string, std::uint64_t> identities;
    bool local = false;
    bool network = false;
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
  std::unique_ptr<NetworkListing> network;
  std::optional<CancelToken> monitorToken;
  bool monitorPending = false;
  NameIds identities;
  // Names in the published entries whose content type is still the name-based guess.
  std::unordered_set<std::string> unsniffed;
  std::uint64_t nextId = 1;
  IdResolver resolveId;
  LinkTargetCheck linksToDirectory;
  // Names of the published links that lead to folders. A rescan keeps them there until its own pass checks again.
  std::unordered_set<std::string, NameHash, std::equal_to<>> directoryLinkNames;
  // Icons that replace the listing's own: a link's target icon, a folder's custom icon. A rescan keeps them until its
  // own icon pass checks again.
  std::unordered_map<std::string, SharedFileIcon, NameHash, std::equal_to<>> iconOverrides;
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

  void requestFacts(std::size_t index, std::uint64_t id, std::uint8_t details) {
    if (!scan
        || scan->enumerating
        || dirty
        || snapshot.status != DirectoryStatus::Ready
        || !snapshot.entries.isCatalogue()
        || index >= snapshot.entries.size()
        || snapshot.entries.brief(index).id != id
        || factsRequested.size() >= 1024) {
      return;
    }
    const auto* known = snapshot.entries.known(index);
    if (known && (known->metadataFailed || (known->queriedDetails & details) == details)) {
      return;
    }
    const auto inFlight = scan->factsInFlight.find(index);
    if (inFlight != scan->factsInFlight.end() && (inFlight->second & details) == details) {
      return;
    }
    factsRequested[index] |= details;
    if (factsIdle != 0 || scan->factsRunning) {
      return;
    }
    factsIdle = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer data) -> gboolean {
          if (const auto state = static_cast<std::weak_ptr<State>*>(data)->lock()) {
            state->factsIdle = 0;
            state->prepareFacts();
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
  }

  void prepareFacts() {
    if (!scan || scan->enumerating || scan->factsRunning || factsRequested.empty() || !snapshot.entries.isCatalogue()) {
      return;
    }
    struct Job {
      DirectoryEntries entries;
      std::shared_ptr<const Location> folder;
      bool network = false;
      std::vector<std::pair<std::size_t, std::uint8_t>> requests;
      std::vector<std::pair<std::size_t, DirectoryEntry>> facts;
      CatalogueValidation validation;
    };
    auto job = std::make_unique<Job>();
    job->entries = snapshot.entries;
    job->folder = scan->folder;
    job->network = scan->network;
    while (!factsRequested.empty() && job->requests.size() < 128) {
      const auto found = factsRequested.begin();
      auto [index, details] = *found;
      factsRequested.erase(found);
      if (const auto* known = snapshot.entries.known(index)) {
        if (known->metadataFailed || (known->queriedDetails & details) == details) {
          continue;
        }
        details |= known->queriedDetails;
      }
      job->requests.emplace_back(index, details);
      scan->factsInFlight[index] = details;
    }
    if (job->requests.empty()) {
      return;
    }
    scan->factsRunning = true;
    auto* task = g_task_new(
        nullptr, scan->cancellable.get(),
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          auto& job = *static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            request->scan->factsRunning = false;
            request->scan->factsInFlight.clear();
            if (!state->dirty && state->snapshot.entries.sameCatalogue(job.entries)) {
              if (job.validation.parentChanged || !job.validation.changed.empty()) {
                if (job.validation.parentChanged) {
                  state->identities.clear();
                } else {
                  for (const auto index : job.validation.changed) {
                    state->identities.set(job.entries.brief(index).name, std::nullopt);
                  }
                }
                state->begin();
                return;
              }
              for (const auto& [index, entry] : job.facts) {
                std::erase(state->factsOrder, index);
                state->factsOrder.push_back(index);
              }
              while (state->factsOrder.size() > 1024) {
                state->factsOrder.pop_front();
              }
              std::unordered_map<std::size_t, DirectoryEntry> fresh;
              for (auto& [index, entry] : job.facts) {
                const auto icon = state->iconOverrides.find(entry.name);
                if (icon != state->iconOverrides.end() && (entry.symlink || entry.type == FileType::Directory)) {
                  entry.icon = icon->second;
                }
                fresh.emplace(index, std::move(entry));
              }
              std::vector<std::pair<std::size_t, DirectoryEntry>> retained;
              retained.reserve(state->factsOrder.size());
              for (const auto index : state->factsOrder) {
                if (auto found = fresh.find(index); found != fresh.end()) {
                  retained.emplace_back(index, std::move(found->second));
                } else if (const auto* entry = state->snapshot.entries.known(index)) {
                  retained.emplace_back(index, *entry);
                }
              }
              state->snapshot.entries = state->snapshot.entries.withFacts(std::move(retained));
              state->identities.rebase(state->snapshot.entries);
              state->notify();
            }
            // The observer can navigate, replace the scan or destroy its session.
            if (state->scan == request->scan) {
              state->prepareFacts();
            }
          }
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    g_task_set_task_data(task, job.release(), [](gpointer data) { delete static_cast<Job*>(data); });
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
      auto& job = *static_cast<Job*>(data);
      FileIconPool icons;
      const auto path = job.folder->nativePath();
      icons.setLocalFolder(path ? path->c_str() : nullptr);
      for (const auto& [index, details] : job.requests) {
        if (g_cancellable_is_cancelled(cancellable) != FALSE) {
          break;
        }
        const auto brief = job.entries.brief(index);
        const auto file = gioFile(job.folder->child(std::string(brief.name)));
        const auto attributes = browseAttributes(details, job.network);
        const GioPtr<GFileInfo> info(
            g_file_query_info(file.get(), attributes.c_str(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr)
        );
        DirectoryEntry entry;
        if (info) {
          entry = readEntry(info.get(), job.folder, icons, true, false);
        } else {
          entry.place = EntryPlace::inFolder(job.folder);
          entry.name = brief.name;
          entry.setDisplayName(std::string(brief.displayName));
          entry.type = brief.type;
          entry.symlink = brief.symlink;
          entry.hidden = brief.hidden;
        }
        entry.id = brief.id;
        entry.metadataPending = false;
        entry.metadataFailed = !info;
        entry.queriedDetails = info ? details : EntryDetails::All;
        job.facts.emplace_back(index, std::move(entry));
      }
      std::vector<std::size_t> rows;
      rows.reserve(job.requests.size());
      for (const auto& [index, details] : job.requests) {
        rows.push_back(index);
      }
      job.validation = validateCatalogueRows(*job.entries.catalogue(), rows, cancellable);
      if (job.validation.unavailable) {
        for (auto& [index, entry] : job.facts) {
          entry.metadataFailed = true;
          entry.queriedDetails = EntryDetails::All;
        }
      }
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }

  void stopScan() {
    if (scan) {
      scan->preparation.cancel();
    }
    if (factsIdle != 0) {
      g_source_remove(std::exchange(factsIdle, 0));
    }
    factsRequested.clear();
    factsOrder.clear();
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
    if (network) {
      network->setChangedCallback({});
      network.reset();
    }
    stopScan();
    if (monitorToken) {
      monitorToken->cancel();
    }
    monitorPending = false;
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
      if (snapshot.entries.isCatalogue()) {
        snapshot.entries = snapshot.entries.withoutFacts();
      } else {
        std::vector<DirectoryEntry> entries(snapshot.entries.begin(), snapshot.entries.end());
        for (auto& entry : entries) {
          entry.access = {};
        }
        snapshot.entries = std::move(entries);
      }
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

  void connectMonitor() {
    g_signal_connect(
        monitor.get(), "changed",
        G_CALLBACK(+[](GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent event, gpointer data) {
          const auto state = static_cast<State*>(data)->shared_from_this();
          state->monitorChanged(file, other, event);
        }),
        this
    );
  }
  void watch() {
    if (monitorPending) {
      return;
    }
    const auto file = gioFile(*snapshot.location);
    if (snapshot.location->isNetworkDiscovery()) {
      monitorPending = true;
      monitorToken = CancelToken{};
      runJob<GioPtr<GFileMonitor>>(
          *monitorToken,
          [file = gioFile(*snapshot.location), token = *monitorToken] {
            GError* error = nullptr;
            GioPtr<GFileMonitor> result(
                g_file_monitor_directory(file.get(), G_FILE_MONITOR_NONE, gioCancellable(token), &error)
            );
            if (const GErrorPtr owned(error); owned) {
              throw FileErrorException(fileError(*owned));
            }
            return result;
          },
          [weak = weak_from_this(), token = *monitorToken](JobResult<GioPtr<GFileMonitor>> result) {
            if (const auto owner = weak.lock(); owner && !token.cancelled()) {
              owner->monitorPending = false;
              if (result && *result) {
                owner->monitor = std::move(*result);
                owner->connectMonitor();
                // Cover changes between the initial enumeration and monitor attachment.
                owner->dirty = true;
                if (!owner->scan || !owner->scan->enumerating) {
                  owner->schedule();
                }
              } else {
                owner->snapshot.monitorError = result
                    ? FileError{FileErrorKind::Unsupported, "Network changes cannot be monitored; refresh to update."}
                    : result.error();
              }
              owner->notify();
            } else if (result && *result) {
              g_file_monitor_cancel(result->get());
            }
          }
      );
      return;
    }
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
    connectMonitor();
  }

  void publishNetwork() {
    if (!network) {
      return;
    }
    const auto& fresh = network->snapshot();
    auto entries = fresh.entries;
    if (entryLimit && entries.size() > entryLimit) {
      entries.resize(entryLimit);
    }
    for (auto& entry : entries) {
      entry.id = resolveId ? resolveId(entry) : identities.find(entry.name).value_or(0);
      if (!entry.id) {
        entry.id = nextId++;
      }
    }
    snapshot.entries = std::move(entries);
    if (!resolveId) {
      identities.index(snapshot.entries);
    }
    snapshot.status = fresh.loading && snapshot.entries.empty() ? DirectoryStatus::Loading : DirectoryStatus::Ready;
    snapshot.monitorError = fresh.warning;
    snapshot.incrementalResults = true;
    notify();
  }
  void openNetwork() {
    ++snapshot.generation;
    snapshot.status = DirectoryStatus::Loading;
    snapshot.error.reset();
    snapshot.metadataLoading = false;
    snapshot.contentTypesPending = false;
    snapshot.accessError.reset();
    snapshot.filesystemError.reset();
    snapshot.metadata = {
        .access = {.read = true, .write = false, .execute = true, .rename = false, .trash = false, .remove = false},
        .filesystem = {.readOnly = true, .remote = true, .type = "network"}
    };
    network = std::make_unique<NetworkListing>();
    network->setChangedCallback([weak = weak_from_this()] {
      if (const auto state = weak.lock()) {
        state->publishNetwork();
      }
    });
    network->start();
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
    if (scan->local) {
      runJob<bool>(
          scan->preparation,
          [location = *snapshot.location, token = scan->preparation] { return prepareSystemMount(location, token); },
          [weak = weak_from_this(), pending = scan, path = std::string(localPath.get())](JobResult<bool> result) {
            const auto state = weak.lock();
            if (!state || state->scan != pending || pending->preparation.cancelled()) {
              return;
            }
            if (!result) {
              state->complete(result.error());
              return;
            }
            if (*result) {
              if (state->monitor) {
                g_file_monitor_cancel(state->monitor.get());
                state->monitor.reset();
              }
              state->snapshot.monitorError =
                  FileError{FileErrorKind::Unsupported, "Refresh to update this network folder."};
            } else if (!state->monitor) {
              state->watch();
            }
            state->queryMetadata();
            state->listNatively(path);
          }
      );
    } else {
      if (!monitor) {
        watch();
      }
      queryMetadata();
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
      bool catalogue = false;
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
    g_task_set_task_data(
        task,
        new Job{
            .path = std::move(path), .limit = entryLimit, .listing = {}, .catalogue = catalogueEnabled && !resolveId
        },
        [](gpointer data) { delete static_cast<Job*>(data); }
    );
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
      auto& job = *static_cast<Job*>(data);
      job.listing = listNativeFolder(job.path, job.limit, cancellable, job.catalogue);
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }

  void listed(NativeListing listing) {
    scan->network = listing.network;
    if (listing.unsupported) {
      enumerate();
      return;
    }
    if (listing.catalogue) {
      auto& catalogue = *listing.catalogue;
      for (std::size_t i = 0; i < catalogue.items.size(); ++i) {
        const auto previous = identities.find(catalogue.brief(i).name);
        catalogue.items[i].id = previous ? *previous : nextId++;
      }
      scan->catalogue = std::move(listing.catalogue);
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
    collectLinks();
    if (!scan->network) {
      collectIconCandidates();
    }
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
    if (scan && scan == listedScan && !snapshot.error && !scan->links->empty()) {
      resolveLinks();
    }
    if (scan && scan == listedScan && !snapshot.error) {
      if (scan->iconCandidates->empty()) {
        iconsRead(0, {}); // Nothing to read: drop the previous listing's icons.
      } else {
        resolveIcons();
      }
    }
  }

  // The listing never follows links. Until resolveLinks() checks them, a link keeps its published target kind.
  void collectLinks() {
    auto links = std::make_shared<std::vector<std::pair<std::size_t, std::string>>>();
    const auto add = [&](std::size_t index, std::string_view name) {
      links->emplace_back(index, name);
      const bool known = directoryLinkNames.contains(name);
      if (known) {
        scan->directoryLinks.insert(index);
      }
      return known;
    };
    if (scan->catalogue) {
      for (std::size_t i = 0; i < scan->catalogue->items.size(); ++i) {
        if (scan->catalogue->items[i].symlink) {
          add(i, scan->catalogue->brief(i).name);
        }
      }
    } else {
      for (std::size_t i = 0; i < scan->entries.size(); ++i) {
        auto& entry = scan->entries[i];
        if (entry.symlink) {
          entry.setLinksToDirectory(add(i, entry.name));
        }
      }
    }
    scan->links = std::move(links);
  }

  // Follows the published links on a shared link worker; see kLinkWorkers.
  void resolveLinks() {
    LinkWorkers::shared().run([weak = weak_from_this(), listed = scan] {
      const auto state = weak.lock();
      if (!state || state->scan != listed || g_cancellable_is_cancelled(listed->cancellable.get()) != FALSE) {
        return false;
      }
      state->followLinks();
      return true;
    });
  }

  void followLinks() {
    struct Job {
      std::string folder;
      std::shared_ptr<const std::vector<std::pair<std::size_t, std::string>>> links;
      LinkTargetCheck linksToDirectory;
      std::unordered_set<std::size_t> directoryLinks;
      std::vector<std::pair<std::string, FileIconSource>> icons;
      bool complete = false;
    };
    auto* task = g_task_new(
        nullptr, scan->cancellable.get(),
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          LinkWorkers::shared().finished();
          auto& job = *static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
          if (const auto state = request->state.lock(); state && state->scan == request->scan && job.complete) {
            state->linksFollowed(std::move(job.directoryLinks), std::move(job.icons));
          }
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    g_task_set_task_data(
        task,
        new Job{
            .folder = scan->path,
            .links = scan->links,
            .linksToDirectory = linksToDirectory,
            .directoryLinks = {},
            .icons = {},
            .complete = false
        },
        [](gpointer data) { delete static_cast<Job*>(data); }
    );
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
      auto& job = *static_cast<Job*>(data);
      const std::string_view separator = job.folder.ends_with('/') ? "" : "/";
      // Over the network every target icon would cost requests; such links keep their listing's icons.
      const bool readIcons = !networkFilesystem(job.folder);
      const auto mounts = configuredSystemMounts();
      const auto mounted = mounts.empty() ? std::vector<MountedFilesystem>{} : readMountedFilesystems();
      job.complete = true;
      for (const auto& [index, name] : *job.links) {
        if (g_cancellable_is_cancelled(cancellable) != FALSE) {
          job.complete = false;
          break;
        }
        const auto path = std::string(job.folder).append(separator).append(name);
        // Following a link strictly inside an idle automount would activate it just by listing this folder.
        if (!mounts.empty() && symlinkEntersUnmountedAutomount(path, mounts, mounted)) {
          continue;
        }
        if (job.linksToDirectory(path)) {
          job.directoryLinks.insert(index);
        }
        // The target just answered, so following it again for its icon does not risk a second hung worker.
        if (readIcons) {
          if (auto icon = linkTargetIcon(path, mounts)) {
            job.icons.emplace_back(name, std::move(*icon));
          }
        }
      }
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }

  void linksFollowed(
      std::unordered_set<std::size_t> directoryLinks, std::vector<std::pair<std::string, FileIconSource>> read
  ) {
    directoryLinkNames.clear();
    for (const auto& [index, name] : *scan->links) {
      if (directoryLinks.contains(index)) {
        directoryLinkNames.emplace(name);
      }
    }
    if (directoryLinks != scan->directoryLinks) {
      scan->directoryLinks = std::move(directoryLinks);
      snapshot.entries = snapshot.entries.withDirectoryLinks(scan->directoryLinks);
      identities.rebase(snapshot.entries);
      notify();
    }
    std::unordered_set<std::string, NameHash, std::equal_to<>> changed;
    std::unordered_set<std::string, NameHash, std::equal_to<>> withIcon;
    for (auto& [name, source] : read) {
      withIcon.insert(name);
      setIconOverride(std::move(name), std::move(source), changed);
    }
    // This pass owns the links' icons: those it found none for, a dangling link's, go.
    for (const auto& [index, name] : *scan->links) {
      if (!withIcon.contains(name) && iconOverrides.erase(name) != 0) {
        changed.insert(name);
      }
    }
    if (!changed.empty()) {
      applyIcons(changed);
    }
  }

  // Folders get their icons from the icon pass; until it runs, folders and links keep the previous passes'.
  void collectIconCandidates() {
    auto candidates = std::make_shared<std::vector<std::string>>();
    if (scan->catalogue) {
      for (std::size_t i = 0; i < scan->catalogue->items.size(); ++i) {
        const auto brief = scan->catalogue->brief(i);
        if (!brief.symlink && brief.type == FileType::Directory) {
          candidates->emplace_back(brief.name);
        }
      }
    } else {
      for (auto& entry : scan->entries) {
        if (!entry.symlink && entry.type == FileType::Directory) {
          candidates->emplace_back(entry.name);
        }
        const auto icon = iconOverrides.find(entry.name);
        if (icon != iconOverrides.end() && (entry.symlink || entry.type == FileType::Directory)) {
          entry.icon = icon->second;
        }
      }
    }
    scan->iconCandidates = std::move(candidates);
  }

  // Reads custom folder icons on a shared link worker; see kLinkWorkers.
  void resolveIcons() {
    LinkWorkers::shared().run([weak = weak_from_this(), listed = scan] {
      const auto state = weak.lock();
      if (!state || state->scan != listed || g_cancellable_is_cancelled(listed->cancellable.get()) != FALSE) {
        return false;
      }
      state->readIcons();
      return true;
    });
  }

  void readIcons() {
    struct Job {
      std::string folder;
      std::shared_ptr<const std::vector<std::string>> candidates;
      std::size_t begin = 0;
      std::chrono::milliseconds interval{0};
      std::size_t end = 0;
      std::vector<std::pair<std::string, FileIconSource>> icons;
      bool complete = false;
    };
    auto* task = g_task_new(
        nullptr, scan->cancellable.get(),
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          LinkWorkers::shared().finished();
          auto& job = *static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
          if (const auto state = request->state.lock(); state && state->scan == request->scan && job.complete) {
            state->iconsRead(job.end, std::move(job.icons));
          }
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    g_task_set_task_data(
        task,
        new Job{
            .folder = scan->path,
            .candidates = scan->iconCandidates,
            .begin = scan->iconsChecked,
            .interval = std::exchange(scan->iconInterval, std::min(2 * scan->iconInterval, kMaxIconSliceInterval)),
            .end = scan->iconsChecked,
            .icons = {},
            .complete = false
        },
        [](gpointer data) { delete static_cast<Job*>(data); }
    );
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
      auto& job = *static_cast<Job*>(data);
      job.complete = true;
      // Over the network every folder would cost requests; such folders keep their listing's icons.
      if (job.begin == 0 && networkFilesystem(job.folder)) {
        job.end = job.candidates->size();
        g_task_return_boolean(task, TRUE);
        return;
      }
      const auto mounts = configuredSystemMounts();
      const auto deadline = std::chrono::steady_clock::now() + job.interval;
      const std::string_view separator = job.folder.ends_with('/') ? "" : "/";
      for (job.end = job.begin; job.end < job.candidates->size(); ++job.end) {
        if (g_cancellable_is_cancelled(cancellable) != FALSE) {
          job.complete = false;
          break;
        }
        if (job.end > job.begin
            && (job.end - job.begin >= kIconSliceEntries || std::chrono::steady_clock::now() >= deadline)) {
          break;
        }
        const auto& name = (*job.candidates)[job.end];
        const auto path = std::string(job.folder).append(separator).append(name);
        // Opening .directory inside a configured automount would activate it while browsing its parent.
        if (automountFor(Location::fromPath(path), mounts)) {
          continue;
        }
        if (auto icon = customFolderIcon(path)) {
          job.icons.emplace_back(name, std::move(*icon));
        }
      }
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }

  // Shares `source` between entries with equal icons, as the listing's own icons do, and records a changed name.
  void setIconOverride(
      std::string name, FileIconSource source, std::unordered_set<std::string, NameHash, std::equal_to<>>& changed
  ) {
    auto found = std::ranges::find_if(scan->iconShared, [&](const auto& icon) { return *icon == source; });
    if (found == scan->iconShared.end()) {
      scan->iconShared.push_back(std::make_shared<const FileIconSource>(std::move(source)));
      found = std::prev(scan->iconShared.end());
    }
    auto [previous, inserted] = iconOverrides.try_emplace(name, *found);
    if (inserted || !sameIcon(previous->second, *found)) {
      previous->second = *found;
      changed.insert(std::move(name));
    }
  }

  // Applies one slice of the icon pass: its icons at once, and once the pass is done, the removal of icons whose
  // folders no longer set one. Links' icons are linksFollowed()'s. A pass that isn't done queues its next slice.
  void iconsRead(std::size_t end, std::vector<std::pair<std::string, FileIconSource>> read) {
    std::unordered_set<std::string, NameHash, std::equal_to<>> changed;
    for (auto& [name, source] : read) {
      scan->iconNames.insert(name);
      setIconOverride(std::move(name), std::move(source), changed);
    }
    scan->iconsChecked = end;
    const bool done = end >= scan->iconCandidates->size();
    if (done) {
      std::unordered_set<std::string_view> links;
      for (const auto& [index, name] : *scan->links) {
        links.insert(name);
      }
      for (auto it = iconOverrides.begin(); it != iconOverrides.end();) {
        if (scan->iconNames.contains(it->first) || links.contains(it->first)) {
          ++it;
        } else {
          changed.insert(it->first);
          it = iconOverrides.erase(it);
        }
      }
    }
    const auto current = scan;
    if (!changed.empty()) {
      applyIcons(changed);
    }
    // The observer may have navigated away.
    if (!done && scan == current) {
      resolveIcons();
    }
  }

  // The icon `entry` shows: its override, or the one its listing derives from its type.
  SharedFileIcon iconFor(const DirectoryEntry& entry) {
    if (const auto icon = iconOverrides.find(entry.name); icon != iconOverrides.end()) {
      return icon->second;
    }
    if (entry.symlink && entry.linksToDirectory) {
      return localFolderIcon();
    }
    const auto type = entry.symlink         ? G_FILE_TYPE_SYMBOLIC_LINK
        : entry.type == FileType::Directory ? G_FILE_TYPE_DIRECTORY
                                            : G_FILE_TYPE_REGULAR;
    return icons.getLocal(
        entry.name.c_str(), type, entry.contentType ? entry.contentType->c_str() : nullptr, *scan->folder
    );
  }

  void applyIcons(const std::unordered_set<std::string, NameHash, std::equal_to<>>& names) {
    bool touched = false;
    if (snapshot.entries.isCatalogue()) {
      // Rows without facts yet take their icon when their facts arrive.
      std::vector<std::pair<std::size_t, DirectoryEntry>> facts;
      for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        if (const auto* known = snapshot.entries.known(i)) {
          auto entry = *known;
          if (names.contains(entry.name)) {
            entry.icon = iconFor(entry);
            touched = true;
          }
          facts.emplace_back(i, std::move(entry));
        }
      }
      if (touched) {
        snapshot.entries = snapshot.entries.withFacts(std::move(facts));
      }
    } else {
      std::vector<DirectoryEntry> entries(snapshot.entries.begin(), snapshot.entries.end());
      for (auto& entry : entries) {
        if (names.contains(entry.name)) {
          entry.icon = iconFor(entry);
          touched = true;
        }
      }
      if (touched) {
        snapshot.entries = std::move(entries);
      }
    }
    if (touched) {
      identities.rebase(snapshot.entries);
      notify();
    }
  }

  // Uncertain entries reuse the published type of an unchanged file; the others wait for sniffContents().
  void keepSniffedTypes(NativeListing& listing) {
    std::unordered_map<std::string_view, const DirectoryEntry*> previous;
    previous.reserve(
        snapshot.entries.isCatalogue() ? std::min<std::size_t>(snapshot.entries.size(), 1024) : snapshot.entries.size()
    );
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
      if (const auto* entry = snapshot.entries.known(i)) {
        previous.emplace(entry->name, entry);
      }
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
    if (!scan->network) {
      attributes.append(kProbeAttributes);
    }
    if (!scan->local) {
      attributes.append(",").append(kIconAttribute).append(",").append(G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
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
              state->enumerationFailed(fileError(*error));
            } else {
              state->nextBatch();
            }
          }
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
  }

  void enumerationFailed(FileError error) {
    if (error.kind != FileErrorKind::Unsupported || scan->local || scan->trash) {
      complete(std::move(error));
      return;
    }
    runJob<FileError>(
        [location = *snapshot.location, error = std::move(error)] { return directoryOpenError(location, error); },
        [weak = weak_from_this(), requested = scan](JobResult<FileError> result) {
          if (const auto state = weak.lock(); state && state->scan == requested) {
            state->complete(result ? std::move(*result) : std::move(result.error()));
          }
        }
    );
  }

  void append(GFileInfo* info) {
    auto entry = readEntry(info, scan->folder, icons, scan->local, scan->trash);
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
    if (scan->catalogue) {
      auto& catalogue = *scan->catalogue;
      used.reserve(catalogue.items.size());
      for (std::size_t i = 0; i < catalogue.items.size(); ++i) {
        auto& item = catalogue.items[i];
        const auto name = catalogue.brief(i).name;
        if (const auto live = identities.find(name)) {
          item.id = *live;
        }
        if (!used.insert(item.id).second) {
          item.id = nextId++;
          used.insert(item.id);
        }
        scan->identities.insert_or_assign(std::string(name), item.id);
      }
    } else {
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
    snapshot.entries =
        scan->catalogue ? DirectoryEntries(std::move(*scan->catalogue)) : DirectoryEntries(std::move(scan->entries));
    // Native entries already carry the known link kinds; a catalogue keeps them in an overlay.
    if (snapshot.entries.isCatalogue() && !scan->directoryLinks.empty()) {
      snapshot.entries = snapshot.entries.withDirectoryLinks(scan->directoryLinks);
    }
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

luciole::DirectorySession::DirectorySession(
    std::size_t entryLimit, IdResolver resolveId, LinkTargetCheck linksToDirectory
)
    : m_state(std::make_shared<State>()) {
  m_state->entryLimit = entryLimit;
  m_state->resolveId = std::move(resolveId);
  m_state->linksToDirectory = linksToDirectory ? std::move(linksToDirectory) : [](const std::string& path) {
    return luciole::linksToDirectory(path);
  };
}

luciole::DirectorySession::~DirectorySession() {
  m_state->changed = {};
  m_state->stop();
}

void luciole::DirectorySession::open(const Location& location) {
  const auto state = m_state;
  state->stop();
  state->identities.clear();
  state->directoryLinkNames.clear();
  state->iconOverrides.clear();
  state->snapshot.location = location;
  state->snapshot.entries = {};
  state->entriesAccessInvalidated = false;
  state->snapshot.monitorError.reset();
  if (location == Location::network()) {
    state->openNetwork();
    return;
  }
  state->begin();
}

void luciole::DirectorySession::refresh() {
  const auto state = m_state;
  if (state->network) {
    ++state->snapshot.generation;
    state->network->refresh();
    return;
  }
  if (state->snapshot.location) {
    if (!state->monitor) {
      state->snapshot.monitorError.reset();
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

void luciole::DirectorySession::setCatalogueEnabled(bool enabled) {
  const auto state = m_state;
  if (state->catalogueEnabled == enabled) {
    return;
  }
  state->catalogueEnabled = enabled;
  if (state->network) {
    return;
  }
  if (state->snapshot.status == DirectoryStatus::Ready
      && !state->snapshot.entries.isCatalogue()
      && state->snapshot.entries.size() <= 512) {
    return;
  }
  if (state->snapshot.location) {
    state->begin();
  }
}

void luciole::DirectorySession::requestMetadata(std::size_t index, std::uint64_t id, std::uint8_t details) {
  m_state->requestFacts(index, id, details);
}
