#include "thumbnails/thumbnail_service.h"

#include "core/log.h"
#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"
#include "fs/native_listing.h"
#include "fs/network_info.h"
#include "fs/network_job.h"
#include "fs/posix_fd.h"
#include "thumbnails/network_preview.h"
#include "thumbnails/thumbnail_codec.h"
#include "thumbnails/thumbnail_provider.h"
#include "thumbnails/thumbnail_store.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
  std::string revisionText(const std::optional<luciole::ChangeRevision>& revision) {
    return revision ? revision->text() : std::string();
  }
  // Purpose is part of the identity: previews accept no smaller foreign thumbnail and take clean video frames.
  std::string cacheKey(const luciole::DirectoryEntry& source, int side, luciole::ThumbnailPurpose purpose) {
    const auto identity = source.identity();
    return std::format(
        "{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}", source.location().uri(), source.etag().value_or(""),
        source.modifiedSeconds.value_or(0), source.size.value_or(0), revisionText(source.contentRevision),
        identity ? identity->filesystem : "", identity ? identity->file : "", side,
        purpose == luciole::ThumbnailPurpose::Preview ? "preview" : "thumbnail"
    );
  }

  // Released images stay resident until they go unseen this long; the byte budget is only a safety cap.
  constexpr std::chrono::minutes kThumbnailIdle{5};
  constexpr std::chrono::seconds kPreviewIdle{30};
  constexpr unsigned kSweepSeconds = 30;
  std::mutex diskMutex;
  std::string stamp(const struct stat& info) {
    return std::format(
        "{}:{}:{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_size, info.st_mtim.tv_sec, info.st_mtim.tv_nsec,
        info.st_ctim.tv_sec, info.st_ctim.tv_nsec
    );
  }
  bool privateUri(const std::string& uri) {
    const std::unique_ptr<GUri, decltype(&g_uri_unref)> parsed(
        g_uri_parse(uri.c_str(), static_cast<GUriFlags>(G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED), nullptr),
        &g_uri_unref
    );
    return !parsed
        || g_uri_get_password(parsed.get())
        || g_uri_get_query(parsed.get())
        || g_uri_get_fragment(parsed.get());
  }
  struct LoadResult {
    luciole::FileIconImage image{
        .pixels = {}, .path = {}, .symbolic = false, .ready = true, .sourceKey = {}, .revision = {}
    };
    std::optional<luciole::ThumbnailCacheWrite> write;
    bool decodeNeeded = false;
  };
  LoadResult load(
      const luciole::DirectoryEntry& source, int side, const luciole::ThumbnailOptions& options, GCancellable* cancel,
      bool generate, luciole::ThumbnailPurpose purpose, std::optional<bool> networkOverride,
      const std::filesystem::path& workspace = {}
  ) {
    using namespace luciole;
    if (!inNetworkWorker()) {
      auto value = previewRequest(
          "thumbnail", source, cancellationToken(cancel),
          {{"side", side},
           {"options", options},
           {"generate", generate},
           {"purpose", purpose},
           {"network", networkOverride}}
      );
      LoadResult result;
      result.decodeNeeded = value.at("decodeNeeded").get<bool>();
      if (!value.at("pixels").is_null()) {
        result.image.pixels = std::make_shared<const LoadedImageFile>(decodePixels(std::move(value.at("pixels"))));
      }
      if (result.image.pixels && !value.at("write").is_null()) {
        const auto& write = value.at("write");
        result.write.emplace(
            ThumbnailCacheWrite{
                .thumbnail =
                    {.image = *result.image.pixels, .text = write.at("text").get<std::map<std::string, std::string>>()},
                .side = write.at("side").get<int>()
            }
        );
      }
      return result;
    }
    LoadResult result;
    ThumbnailStore store;
    const auto location = source.location();
    const auto native = location.nativePath();
    if (!native) {
      if (options.policy == ThumbnailPolicy::All && source.modifiedSeconds) {
        if (auto cached = store.read(
                location.uri(), side, *source.modifiedSeconds, source.size, {}, purpose == ThumbnailPurpose::Thumbnail
            )) {
          result.image.pixels = std::make_shared<const LoadedImageFile>(std::move(cached->image));
        }
      }
      return result;
    }
    if (g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    const bool remote = networkOverride ? *networkOverride : networkFilesystem(*native);
    if (options.policy == ThumbnailPolicy::Local && remote) {
      return result;
    }
    const std::string mime(source.contentType.value_or(""));
    const bool media = mime.starts_with("video/") || mime.starts_with("audio/");
    const luciole::Fd file(::open(native->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat before{};
    if (!file.valid()
        || ::fstat(file.get(), &before) < 0
        || !S_ISREG(before.st_mode)
        || before.st_size <= 0
        || (!media
            && static_cast<std::uint64_t>(before.st_size)
                > static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U)) {
      return result;
    }
    if ((source.size && *source.size != static_cast<std::uint64_t>(before.st_size))
        || (source.modifiedSeconds && *source.modifiedSeconds != static_cast<std::uint64_t>(before.st_mtim.tv_sec))) {
      return result;
    }
    const GioPtr<GFileInfo> current(g_file_query_info(
        gioFile(location).get(),
        G_FILE_ATTRIBUTE_ETAG_VALUE "," G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM
                                    "," G_FILE_ATTRIBUTE_TIME_CHANGED
                                    ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr
    ));
    const auto etag = source.etag();
    if (!current || (etag && (!g_file_info_get_etag(current.get()) || *etag != g_file_info_get_etag(current.get())))) {
      return result;
    }
    if (const auto identity = source.identity(); identity && readFileIdentity(current.get()) != identity) {
      return result;
    }
    if (source.contentRevision && readChangeRevision(current.get()) != source.contentRevision) {
      return result;
    }
    const auto sourceStamp = stamp(before);
    const auto relative = std::filesystem::path(*native).lexically_relative(store.root());
    const bool insideCache = !relative.empty() && *relative.begin() != "..";
    const bool videoPreview = mime.starts_with("video/") && purpose == luciole::ThumbnailPurpose::Preview;
    if (!generate && !insideCache && side <= 1024 && !videoPreview) {
      if (auto cached = store.read(
              location.uri(), side, static_cast<std::uint64_t>(before.st_mtim.tv_sec),
              static_cast<std::uint64_t>(before.st_size), sourceStamp, purpose == ThumbnailPurpose::Thumbnail
          )) {
        const bool oldVideo = mime.starts_with("video/")
            && cached->text.contains("Software")
            && cached->text.at("Software") == "Luciole"
            && (!cached->text.contains("X-Luciole::VideoVersion")
                || cached->text.at("X-Luciole::VideoVersion") != kVideoThumbnailVersion);
        if (!oldVideo) {
          struct stat after{};
          if (::fstat(file.get(), &after) == 0 && stamp(after) == sourceStamp && !g_cancellable_is_cancelled(cancel)) {
            result.image.pixels = std::make_shared<const LoadedImageFile>(std::move(cached->image));
          }
          return result;
        }
      }
    }
    if (!generate) {
      result.decodeNeeded = true;
      return result;
    }
    std::expected<LoadedImageFile, std::string> decoded = std::unexpected("no thumbnail decoder");
    Fd staged;
    if (remote) {
      staged = stagePreviewSource(file.get(), static_cast<std::uint64_t>(before.st_size));
    }
    if (mime.starts_with("image/") || !insideCache) {
      decoded = renderThumbnailProvider(
          staged.valid() ? staged.get() : file.get(), mime, side, options, cancel, purpose, workspace
      );
    }
    if (!decoded && !g_cancellable_is_cancelled(cancel)) {
      logDebug("[thumbnail] mime={} size={}: {}", mime, side, decoded.error());
    }
    struct stat after{};
    if (!decoded
        || ::fstat(file.get(), &after) < 0
        || sourceStamp != stamp(after)
        || g_cancellable_is_cancelled(cancel)) {
      return result;
    }
    // Reject replacement of the original pathname while the held descriptor was decoded.
    struct stat named{};
    if (::lstat(native->c_str(), &named) < 0 || stamp(named) != sourceStamp) {
      return result;
    }
    if (!insideCache && side <= 1024 && !videoPreview) {
      ThumbnailPng cached{
          .image = *decoded,
          .text = {
              {"Thumb::URI", location.uri()},
              {"Thumb::MTime", std::to_string(before.st_mtim.tv_sec)},
              {"Thumb::Size", std::to_string(before.st_size)},
              {"X-Luciole::Stamp", sourceStamp},
              {"Software", "Luciole"}
          }
      };
      if (mime.starts_with("video/")) {
        cached.text["X-Luciole::VideoVersion"] = kVideoThumbnailVersion;
      }
      result.write.emplace(ThumbnailCacheWrite{std::move(cached), side});
    }
    result.image.pixels = std::make_shared<const LoadedImageFile>(std::move(*decoded));
    return result;
  }
} // namespace

struct luciole::ThumbnailService::State : std::enable_shared_from_this<State> {
  enum class Stage : std::uint8_t { ProbeQueued, Probing, DecodeQueued, Decoding, Ready };
  struct Item {
    explicit Item(DirectoryEntry entry) : source(std::move(entry)) {}
    std::shared_ptr<FileIconImage> image;
    DirectoryEntry source;
    GioPtr<GCancellable> cancel{g_cancellable_new()};
    std::uint64_t serial = 0, used = 0;
    // Pixel bytes charged to the budget: the full square while pending, then the published image's real size.
    std::size_t bytes = 0;
    std::chrono::steady_clock::time_point seen;
    int side = 128;
    ThumbnailPurpose purpose = ThumbnailPurpose::Thumbnail;
    bool visible = false, failed = false, network = false;
    Stage stage = Stage::ProbeQueued;
    std::chrono::steady_clock::time_point retry;
  };
  struct Request {
    std::shared_ptr<State> owner;
    std::string key;
    DirectoryEntry source;
    ThumbnailOptions options;
    std::uint64_t epoch, serial;
    int side;
    bool generate;
    ThumbnailPurpose purpose;
  };
  struct Result {
    LoadResult loaded;
    std::string error;
  };
  struct WriteBatch {
    std::shared_ptr<State> owner;
    std::vector<ThumbnailCacheWrite> writes;
    std::uint64_t budget;
  };
  static constexpr std::size_t kMaxProbes = 4;
  static constexpr std::size_t kWriteBudget = 64U * 1024U * 1024U;
  ThumbnailOptions options;
  NetworkFilesystemCheck networkFilesystem;
  std::unordered_map<std::string, Item> cache;
  std::unordered_map<const FileIconImage*, Item*> leases;
  std::function<void()> changed;
  std::uint64_t epoch = 0, clock = 0, serial = 0;
  std::size_t active = 0, probes = 0, reserved = 0, writeBytes = 0;
  bool alive = true, clearing = false, writing = false;
  std::string error;
  GSource* idle = nullptr;
  GSource* writeTimer = nullptr;
  GSource* sweepTimer = nullptr;
  std::deque<ThumbnailCacheWrite> writes;
  GioPtr<GCancellable> writeCancel{g_cancellable_new()};
  struct Running {
    GioPtr<GCancellable> cancel;
    Location source;
    bool network, generate;
  };
  std::vector<Running> running;
  void notify() {
    const auto callback = changed;
    if (alive && callback) {
      callback();
    }
  }
  static void destroySource(GSource*& source) {
    if (source) {
      g_source_destroy(source);
      g_source_unref(std::exchange(source, nullptr));
    }
  }
  void retire() {
    ++epoch;
    for (const auto& cancel : running) {
      g_cancellable_cancel(cancel.cancel.get());
    }
    for (const auto& [_, item] : cache) {
      g_cancellable_cancel(item.cancel.get());
      item.image->pixels.reset();
      item.image->ready = false;
    }
    cache.clear();
    leases.clear();
    reserved = 0;
    destroySource(idle);
    destroySource(writeTimer);
    destroySource(sweepTimer);
    g_cancellable_cancel(writeCancel.get());
    writeCancel.reset(g_cancellable_new());
    writes.clear();
    writeBytes = 0;
  }
  void erase(std::unordered_map<std::string, Item>::iterator item) {
    g_cancellable_cancel(item->second.cancel.get());
    item->second.image->pixels.reset();
    item->second.image->ready = false;
    leases.erase(item->second.image.get());
    reserved -= item->second.bytes;
    cache.erase(item);
  }
  bool reserve(std::size_t bytes, bool visible) {
    while (reserved + bytes > kResidentBudget || cache.size() >= kMaxEntries) {
      auto oldest = cache.end();
      for (auto it = cache.begin(); it != cache.end(); ++it) {
        if ((it->second.image.use_count() == 1 || (visible && !it->second.visible))
            && (oldest == cache.end() || it->second.used < oldest->second.used)) {
          oldest = it;
        }
      }
      if (oldest == cache.end()) {
        return false;
      }
      erase(oldest);
    }
    reserved += bytes;
    return true;
  }
  // Held images count as seen; released ones are dropped once unseen past their purpose's idle limit.
  void armSweep() {
    if (sweepTimer || !alive) {
      return;
    }
    sweepTimer = g_timeout_source_new_seconds(kSweepSeconds);
    g_source_set_callback(
        sweepTimer,
        [](gpointer data) -> gboolean {
          auto* owner = static_cast<State*>(data);
          if (owner->sweep()) {
            return G_SOURCE_CONTINUE;
          }
          g_source_unref(std::exchange(owner->sweepTimer, nullptr));
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(sweepTimer, g_main_context_default());
  }
  bool sweep() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = cache.begin(); it != cache.end();) {
      auto& item = it->second;
      const std::chrono::seconds idle = item.purpose == ThumbnailPurpose::Preview ? kPreviewIdle : kThumbnailIdle;
      if (item.image.use_count() > 1) {
        item.seen = now;
      } else if (item.stage == Stage::Ready && now - item.seen >= idle) {
        const auto old = it++;
        erase(old);
        continue;
      }
      ++it;
    }
    return !cache.empty();
  }
  void schedule() {
    if (idle || !alive || clearing) {
      return;
    }
    idle = g_idle_source_new();
    g_source_set_callback(
        idle,
        [](gpointer data) -> gboolean {
          auto* owner = static_cast<State*>(data);
          g_source_unref(std::exchange(owner->idle, nullptr));
          owner->pump();
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(idle, g_main_context_default());
  }
  void queueWrite(ThumbnailCacheWrite write) {
    const auto bytes = write.thumbnail.image.rgba.size();
    if (!alive || clearing || writes.size() >= 2 * ThumbnailStore::kMaxBatch || bytes > kWriteBudget - writeBytes) {
      return;
    }
    writeBytes += bytes;
    writes.push_back(std::move(write));
    scheduleWrites();
  }
  void scheduleWrites() {
    if (!alive || clearing || writing || writeTimer || writes.empty()) {
      return;
    }
    // Gather nearby completions into one ledger rewrite without holding up decoded-pixel publication.
    writeTimer = g_timeout_source_new(250);
    g_source_set_callback(
        writeTimer,
        [](gpointer data) -> gboolean {
          auto* owner = static_cast<State*>(data);
          g_source_unref(std::exchange(owner->writeTimer, nullptr));
          owner->startWrites();
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(writeTimer, g_main_context_default());
  }
  void startWrites() {
    if (writing || writes.empty() || !alive || clearing) {
      return;
    }
    auto batch = std::make_unique<WriteBatch>();
    batch->owner = shared_from_this();
    batch->budget = static_cast<std::uint64_t>(options.cacheMiB) * 1024U * 1024U;
    batch->writes.reserve(std::min(writes.size(), ThumbnailStore::kMaxBatch));
    while (!writes.empty() && batch->writes.size() < ThumbnailStore::kMaxBatch) {
      writeBytes -= writes.front().thumbnail.image.rgba.size();
      batch->writes.push_back(std::move(writes.front()));
      writes.pop_front();
    }
    writing = true;
    const GioPtr<GTask> task(g_task_new(
        nullptr, writeCancel.get(),
        [](GObject*, GAsyncResult* result, gpointer) {
          const auto owner = static_cast<WriteBatch*>(g_task_get_task_data(G_TASK(result)))->owner;
          GError* error = nullptr;
          (void)g_task_propagate_boolean(G_TASK(result), &error);
          g_clear_error(&error);
          owner->writing = false;
          owner->scheduleWrites();
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), batch.release(), [](gpointer data) { delete static_cast<WriteBatch*>(data); });
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
      const auto& batch = *static_cast<WriteBatch*>(data);
      try {
        std::scoped_lock lock(diskMutex);
        ThumbnailStore{}.writeBatch(batch.writes, batch.budget, cancel);
      } catch (const std::exception&) {
        // A regenerable cache write cannot delay or undo a published thumbnail.
      }
      g_task_return_boolean(task, true);
    });
  }
  // Previews first, then visible cells, then the least recently requested.
  auto next(Stage stage) {
    const bool generate = stage == Stage::DecodeQueued;
    const auto remote = std::ranges::count_if(running, [generate](const Running& job) {
      return job.network && job.generate == generate;
    });
    // Keep one slot available for local artwork when a share stops answering.
    const bool allowNetwork = static_cast<std::size_t>(remote) + 1 < (generate ? decoders() : kMaxProbes);
    const auto before = [](const Item& a, const Item& b) {
      const bool aPreview = a.purpose == ThumbnailPurpose::Preview, bPreview = b.purpose == ThumbnailPurpose::Preview;
      if (aPreview != bPreview) {
        return aPreview;
      }
      if (a.visible != b.visible) {
        return a.visible;
      }
      return a.used < b.used;
    };
    auto result = cache.end();
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->second.stage == stage
          && (!it->second.network || allowNetwork)
          && (result == cache.end() || before(it->second, result->second))) {
        result = it;
      }
    }
    return result;
  }
  void start(std::unordered_map<std::string, Item>::iterator next, bool generate) {
    auto& item = next->second;
    item.stage = generate ? Stage::Decoding : Stage::Probing;
    if (generate) {
      ++active;
    } else {
      ++probes;
    }
    running.push_back(
        {GioPtr<GCancellable>(static_cast<GCancellable*>(g_object_ref(item.cancel.get()))), item.source.location(),
         item.network, generate}
    );
    const GioPtr<GTask> task(g_task_new(
        nullptr, item.cancel.get(),
        [](GObject*, GAsyncResult* result, gpointer) {
          auto* task = G_TASK(result);
          const auto* request = static_cast<Request*>(g_task_get_task_data(task));
          const auto owner = request->owner;
          GError* error = nullptr;
          auto loaded = std::unique_ptr<Result>(static_cast<Result*>(g_task_propagate_pointer(task, &error)));
          if (!loaded && !g_cancellable_is_cancelled(g_task_get_cancellable(task))) {
            loaded = std::make_unique<Result>();
            loaded->error = error ? error->message : "Thumbnail unavailable.";
          }
          g_clear_error(&error);
          if (request->generate) {
            --owner->active;
          } else {
            --owner->probes;
          }
          std::erase_if(owner->running, [&](const auto& cancel) {
            return cancel.cancel.get() == g_task_get_cancellable(task);
          });
          auto found = owner->cache.find(request->key);
          if (owner->alive
              && owner->epoch == request->epoch
              && found != owner->cache.end()
              && found->second.serial == request->serial
              && loaded) {
            auto& item = found->second;
            if (loaded->loaded.decodeNeeded) {
              item.stage = Stage::DecodeQueued;
            } else {
              item.stage = Stage::Ready;
              const auto& source = request->source;
              const auto identity = source.identity();
              loaded->loaded.image.sourceKey = "file:" + source.location().uri();
              loaded->loaded.image.revision =
                  nlohmann::json::array({"pixbuf-media-v1", source.etag().value_or(""),
                                         source.modifiedSeconds.value_or(0), source.size.value_or(0),
                                         revisionText(source.contentRevision), identity ? identity->filesystem : "",
                                         identity ? identity->file : "", request->side, request->options.maxFileMiB})
                      .dump();
              *item.image = std::move(loaded->loaded.image);
              const std::size_t bytes = item.image->pixels ? item.image->pixels->rgba.size() : 0;
              owner->reserved = owner->reserved - item.bytes + bytes;
              item.bytes = bytes;
              item.failed = !item.image->pixels;
              item.retry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
              if (loaded->loaded.write) {
                owner->queueWrite(std::move(*loaded->loaded.write));
              }
              if (!loaded->error.empty()) {
                owner->error = loaded->error;
              }
            }
          }
          owner->pump();
          owner->notify();
        },
        nullptr
    ));
    auto requestOptions = options;
    if (item.purpose == ThumbnailPurpose::Preview) {
      requestOptions.policy = ThumbnailPolicy::All;
    }
    g_task_set_task_data(
        task.get(),
        new Request{
            shared_from_this(), next->first, item.source, requestOptions, epoch, item.serial, item.side, generate,
            item.purpose
        },
        [](gpointer data) { delete static_cast<Request*>(data); }
    );
    runIoTask(
        task.get(),
        item.network ? (generate ? IoLane::NetworkPreviewDecode : IoLane::NetworkPreviewProbe)
                     : (generate ? IoLane::PreviewDecode : IoLane::PreviewProbe),
        [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
          const auto& request = *static_cast<Request*>(data);
          auto result = std::make_unique<Result>();
          try {
            const auto path = request.source.location().nativePath();
            std::optional<bool> network;
            if (path && request.owner->networkFilesystem) {
              network = request.owner->networkFilesystem(*path);
            }
            result->loaded =
                load(request.source, request.side, request.options, cancel, request.generate, request.purpose, network);
          } catch (const std::exception& error) {
            result->error = error.what();
          }
          g_task_return_pointer(task, result.release(), [](gpointer data) { delete static_cast<Result*>(data); });
        }
    );
  }
  void pump() {
    if (!alive || clearing) {
      return;
    }
    // Cancellation must run even while both decoders are occupied.
    for (auto it = cache.begin(); it != cache.end();) {
      if (it->second.stage != Stage::Ready && it->second.image.use_count() == 1) {
        const auto old = it++;
        erase(old);
      } else {
        ++it;
      }
    }
    while (probes < kMaxProbes) {
      const auto item = next(Stage::ProbeQueued);
      if (item == cache.end()) {
        break;
      }
      start(item, false);
    }
    while (active < decoders()) {
      const auto item = next(Stage::DecodeQueued);
      if (item == cache.end()) {
        break;
      }
      start(item, true);
    }
  }
};

luciole::ThumbnailService::ThumbnailService() : ThumbnailService(NetworkFilesystemCheck{}) {}
luciole::ThumbnailService::ThumbnailService(NetworkFilesystemCheck networkFilesystem)
    : m_state(std::make_shared<State>()) {
  m_state->networkFilesystem = std::move(networkFilesystem);
}
std::size_t luciole::ThumbnailService::decoders() {
  static const std::size_t count = std::clamp<std::size_t>(std::thread::hardware_concurrency() / 4, 2, 6);
  return count;
}
int luciole::ThumbnailService::pixelSizeClass(int pixels, ThumbnailPurpose purpose) {
  return purpose == ThumbnailPurpose::Preview && pixels > 1024 ? kMaxPreviewImageSide
                                                               : ThumbnailStore::sizeClass(std::clamp(pixels, 1, 1024));
}
luciole::ThumbnailService::~ThumbnailService() {
  m_state->alive = false;
  m_state->changed = {};
  m_state->retire();
}
std::shared_ptr<const luciole::FileIconImage>
luciole::ThumbnailService::request(const DirectoryEntry& source, int pixels, bool visible, ThumbnailPurpose purpose) {
  auto& state = *m_state;
  const auto& mime = source.contentType;
  const auto location = source.location();
  const auto policy = purpose == ThumbnailPurpose::Preview ? ThumbnailPolicy::All : state.options.policy;
  if (policy == ThumbnailPolicy::Never
      || state.clearing
      || !mime
      || !(
          mime->starts_with("image/")
          || mime->starts_with("video/")
          || mime->starts_with("audio/")
          || *mime == "application/pdf"
      )
      || source.type != luciole::FileType::Regular
      || source.symlink
      || !source.modifiedSeconds
      || privateUri(location.uri())
      || (policy == ThumbnailPolicy::Local && !location.nativePath())) {
    return {};
  }
  const int side = pixelSizeClass(pixels, mime->starts_with("image/") ? purpose : ThumbnailPurpose::Thumbnail);
  const auto key = cacheKey(source, side, purpose);
  auto found = state.cache.find(key);
  if (found != state.cache.end() && found->second.failed && std::chrono::steady_clock::now() >= found->second.retry) {
    state.erase(found);
    found = state.cache.end();
  }
  if (found == state.cache.end()) {
    const auto bytes = static_cast<std::size_t>(side) * side * 4;
    if (!state.reserve(bytes, visible)) {
      return {};
    }
    State::Item item(source);
    item.network = ioLane(location) == IoLane::Network;
    item.image = std::make_shared<FileIconImage>();
    item.serial = ++state.serial;
    item.bytes = bytes;
    item.side = side;
    item.purpose = purpose;
    found = state.cache.emplace(key, std::move(item)).first;
    state.leases.emplace(found->second.image.get(), &found->second);
    state.armSweep();
  }
  found->second.visible = visible;
  found->second.used = ++state.clock;
  found->second.seen = std::chrono::steady_clock::now();
  if (found->second.stage == State::Stage::ProbeQueued || found->second.stage == State::Stage::DecodeQueued) {
    state.schedule();
  }
  return found->second.image;
}
bool luciole::ThumbnailService::touch(const std::shared_ptr<const FileIconImage>& image, bool visible) {
  const auto found = m_state->leases.find(image.get());
  if (found == m_state->leases.end()) {
    return false;
  }
  auto& item = *found->second;
  if (item.failed && std::chrono::steady_clock::now() >= item.retry) {
    return false;
  }
  item.visible = visible;
  item.used = ++m_state->clock;
  item.seen = std::chrono::steady_clock::now();
  return true;
}
std::shared_ptr<const luciole::FileIconImage> luciole::ThumbnailService::prepared(const DirectoryEntry& source) {
  if (m_state->clearing || m_state->options.policy == ThumbnailPolicy::Never) {
    return {};
  }
  for (const int side : {kMaxPreviewImageSide, 1024, 512, 256, 128}) {
    for (const auto purpose : {ThumbnailPurpose::Preview, ThumbnailPurpose::Thumbnail}) {
      const auto found = m_state->cache.find(cacheKey(source, side, purpose));
      if (found != m_state->cache.end()
          && found->second.image->pixels
          && found->second.source.type == source.type
          && found->second.source.symlink == source.symlink
          && found->second.source.contentType == source.contentType) {
        (void)touch(found->second.image, true);
        return found->second.image;
      }
    }
  }
  return {};
}
void luciole::ThumbnailService::synchronizeSources(const DirectorySnapshot& snapshot) {
  if (m_state->cache.empty()) {
    return;
  }
  std::unordered_map<std::string, const DirectoryEntry*> current;
  std::unordered_set<std::string_view> names;
  current.reserve(m_state->cache.size());
  names.reserve(m_state->cache.size());
  for (const auto& [_, item] : m_state->cache) {
    current.emplace(item.source.location().uri(), nullptr);
    names.insert(item.source.name);
  }
  // A location is built on demand, so only entries named like a cached source build theirs.
  for (const auto& entry : snapshot.entries) {
    if (!names.contains(entry.name)) {
      continue;
    }
    const auto found = current.find(entry.location().uri());
    if (found != current.end()) {
      found->second = &entry;
    }
  }
  for (auto it = m_state->cache.begin(); it != m_state->cache.end();) {
    const auto found = current.find(it->second.source.location().uri());
    const auto& before = it->second.source;
    const auto* after = found == current.end() ? nullptr : found->second;
    if (after
        && (before.size != after->size
            || before.modifiedSeconds != after->modifiedSeconds
            || before.etag() != after->etag()
            || before.identity() != after->identity()
            || before.contentRevision != after->contentRevision
            || before.type != after->type
            || before.symlink != after->symlink
            || before.contentType != after->contentType)) {
      const auto old = it++;
      m_state->erase(old);
    } else {
      ++it;
    }
  }
}
void luciole::ThumbnailService::setOptions(ThumbnailOptions options) {
  if (options == m_state->options) {
    return;
  }
  const bool trim = options.cacheMiB < m_state->options.cacheMiB;
  m_state->retire();
  m_state->options = options;
  m_state->error.clear();
  if (trim) {
    const GioPtr<GTask> task(g_task_new(nullptr, nullptr, nullptr, nullptr));
    g_task_set_task_data(
        task.get(), new std::uint64_t(static_cast<std::uint64_t>(options.cacheMiB) * 1024U * 1024U),
        [](gpointer data) { delete static_cast<std::uint64_t*>(data); }
    );
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
      try {
        std::scoped_lock lock(diskMutex);
        (void)ThumbnailStore{}.prune(*static_cast<std::uint64_t*>(data));
      } catch (const std::exception&) {
      }
      g_task_return_boolean(task, true);
    });
  }
  m_state->notify();
}
const luciole::ThumbnailOptions& luciole::ThumbnailService::options() const { return m_state->options; }
void luciole::ThumbnailService::releaseLocation(const Location& root) {
  const auto inside = [&](const Location& location) {
    return location == root || location.uri().starts_with(root.uri() + (root.uri().ends_with('/') ? "" : "/"));
  };
  for (const auto& running : m_state->running) {
    if (inside(running.source)) {
      g_cancellable_cancel(running.cancel.get());
    }
  }
  for (auto item = m_state->cache.begin(); item != m_state->cache.end();) {
    if (inside(item->second.source.location())) {
      const auto old = item++;
      m_state->erase(old);
    } else {
      ++item;
    }
  }
}

bool luciole::ThumbnailService::readingLocation(const Location& root) const {
  return std::ranges::any_of(m_state->running, [&](const auto& running) {
    const auto& location = running.source;
    return location == root || location.uri().starts_with(root.uri() + (root.uri().ends_with('/') ? "" : "/"));
  });
}

void luciole::ThumbnailService::endFrame() { m_state->schedule(); }
void luciole::ThumbnailService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void luciole::ThumbnailService::clearCache() {
  if (m_state->clearing) {
    return;
  }
  m_state->retire();
  m_state->clearing = true;
  m_state->error.clear();
  const GioPtr<GTask> task(g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        const auto owner = *static_cast<std::shared_ptr<State>*>(g_task_get_task_data(G_TASK(result)));
        const GString error(static_cast<char*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
        owner->clearing = false;
        owner->error = error ? error.get() : "";
        owner->notify();
      },
      nullptr
  ));
  g_task_set_task_data(task.get(), new std::shared_ptr<State>(m_state), [](gpointer data) {
    delete static_cast<std::shared_ptr<State>*>(data);
  });
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer, GCancellable*) {
    std::string error;
    try {
      std::scoped_lock lock(diskMutex);
      error = ThumbnailStore{}.clear();
    } catch (const std::exception& exception) {
      error = exception.what();
    }
    g_task_return_pointer(task, g_strdup(error.c_str()), g_free);
  });
  m_state->notify();
}
bool luciole::ThumbnailService::clearing() const { return m_state->clearing; }
const std::string& luciole::ThumbnailService::error() const { return m_state->error; }
std::size_t luciole::ThumbnailService::reservedBytes() const { return m_state->reserved; }
std::size_t luciole::ThumbnailService::activeJobs() const { return m_state->active; }
std::size_t luciole::ThumbnailService::queuedJobs() const {
  return static_cast<std::size_t>(std::ranges::count_if(m_state->cache, [](const auto& item) {
    return item.second.stage != State::Stage::Ready && item.second.stage != State::Stage::Decoding;
  }));
}

luciole::NetworkMessage luciole::networkThumbnailRequest(const NetworkMessage& request) {
  const CancelToken token;
  const auto result = load(
      request.at("source").get<DirectoryEntry>(), request.at("side").get<int>(),
      request.at("options").get<ThumbnailOptions>(), gioCancellable(token), request.at("generate").get<bool>(),
      request.at("purpose").get<ThumbnailPurpose>(), request.at("network").get<std::optional<bool>>(),
      request.value("workspace", std::string{})
  );
  NetworkMessage value{
      {"decodeNeeded", result.decodeNeeded},
      {"pixels", result.image.pixels ? encodePixels(*result.image.pixels) : NetworkMessage(nullptr)},
      {"write", nullptr}
  };
  if (result.write) {
    value["write"] = {{"text", result.write->thumbnail.text}, {"side", result.write->side}};
  }
  return value;
}
