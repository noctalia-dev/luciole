#include "thumbnails/file_icon_cache.h"

#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"
#include "fs/network_info.h"
#include "fs/network_job.h"
#include "imaging/svg_rasterizer.h"
#include "system/icon_resolver.h"
#include "thumbnails/network_preview.h"

#include <algorithm>
#include <array>
#include <deque>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>

namespace {
  constexpr std::size_t kMaxEntries = 128;
  constexpr std::size_t kPixelBudget = 8U * 1024U * 1024U;

  std::string desktopIcon(const std::string& path) {
    if (path.empty()) {
      return {};
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      return {};
    }
    std::array<char, 256U * 1024U + 1> bytes{};
    std::size_t count = 0;
    while (count < bytes.size()) {
      const auto read = ::read(fd, bytes.data() + count, bytes.size() - count);
      if (read <= 0) {
        break;
      }
      count += static_cast<std::size_t>(read);
    }
    ::close(fd);
    if (count == bytes.size()) {
      return {};
    }
    const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> file(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_data(file.get(), bytes.data(), count, G_KEY_FILE_NONE, nullptr)) {
      return {};
    }
    const luciole::GString icon(g_key_file_get_string(file.get(), "Desktop Entry", "Icon", nullptr));
    return icon ? icon.get() : "";
  }

  struct IconColorScheme {
    std::vector<std::string> roots;
    std::string stylesheet;
  };

  bool insideRoot(const std::string& path, const std::vector<std::string>& roots) {
    return std::ranges::any_of(roots, [&path](const std::string& root) {
      return path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
    });
  }

  luciole::FileIconImage loadIcon(
      const luciole::FileIconSource& source, int size, std::string theme, std::uint64_t generation,
      const IconColorScheme& scheme
  ) {
    IconResolver resolver(std::move(theme), generation);
    std::vector<std::string> candidates;
    if (const auto icon = desktopIcon(source.desktopFile); !icon.empty()) {
      candidates.push_back(icon);
    }
    if (!source.filePath.empty()) {
      candidates.push_back(source.filePath);
    }
    candidates.insert(candidates.end(), source.names.begin(), source.names.end());
    for (const auto& candidate : candidates) {
      const auto& path = resolver.resolve(candidate, size);
      if (path.empty()) {
        continue;
      }
      auto image = path.ends_with(".svg") && insideRoot(path, scheme.roots)
          ? luciole::rasterizeSvg(path, size, false, scheme.stylesheet)
          : loadImageFile(path, size);
      if (image) {
        return {
            .pixels = std::make_shared<const LoadedImageFile>(std::move(*image)),
            .path = path,
            .symbolic = path.ends_with("-symbolic.svg") || path.ends_with("-symbolic.png"),
            .ready = true,
            .sourceKey = {},
            .revision = {}
        };
      }
    }
    return {.pixels = {}, .path = {}, .symbolic = false, .ready = true, .sourceKey = {}, .revision = {}};
  }

  std::string keyFor(const luciole::FileIconSource& source, int size) {
    std::string key = std::to_string(size);
    const auto append = [&](const std::string& part) { key += ':' + std::to_string(part.size()) + ':' + part; };
    append(source.filePath);
    append(source.desktopFile);
    for (const auto& name : source.names) {
      append(name);
    }
    return key;
  }
} // namespace

struct luciole::FileIconCache::State : std::enable_shared_from_this<State> {
  struct Item {
    std::shared_ptr<FileIconImage> image;
    std::uint64_t used = 0;
    std::chrono::steady_clock::time_point retry = std::chrono::steady_clock::time_point::max();
  };
  struct Request {
    std::shared_ptr<State> owner;
    FileIconSource source;
    std::shared_ptr<FileIconImage> image;
    IoLane lane;
    int pixels = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<const IconColorScheme> colorScheme;
  };
  std::unordered_map<std::string, Item> cache;
  std::deque<Request> pending;
  std::function<void()> changed;
  std::uint64_t generation = 0;
  std::shared_ptr<const IconColorScheme> colorScheme;
  std::uint64_t clock = 0;
  std::size_t active = 0;
  CancelToken cancel;
  bool alive = true, networkActive = false;

  void trim(bool reserveEntry) {
    std::erase_if(cache, [](const auto& item) {
      return item.second.image->ready && item.second.image.use_count() == 1;
    });
    std::size_t bytes = 0;
    for (const auto& [_, item] : cache) {
      bytes += item.image->pixels ? item.image->pixels->rgba.size() : 0;
    }
    while (cache.size() + (reserveEntry ? 1 : 0) > kMaxEntries || bytes > kPixelBudget) {
      auto oldest = cache.end();
      for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->second.image.use_count() == 1 && (oldest == cache.end() || it->second.used < oldest->second.used)) {
          oldest = it;
        }
      }
      if (oldest == cache.end()) {
        break;
      }
      bytes -= oldest->second.image->pixels ? oldest->second.image->pixels->rgba.size() : 0;
      cache.erase(oldest);
    }
  }

  void pump() {
    while (alive && active < 2 && !pending.empty()) {
      const auto next = std::ranges::find_if(pending, [this](const Request& item) {
        return !networkActive || item.lane == IoLane::PreviewProbe;
      });
      if (next == pending.end()) {
        break;
      }
      auto* request = new Request(std::move(*next));
      pending.erase(next);
      if (request->lane == IoLane::NetworkPreviewProbe) {
        networkActive = true;
      }
      ++active;
      GioPtr<GTask> task(g_task_new(
          nullptr, gioCancellable(cancel),
          [](GObject*, GAsyncResult* result, gpointer) {
            auto* task = G_TASK(result);
            const auto* request = static_cast<Request*>(g_task_get_task_data(task));
            const auto owner = request->owner;
            auto image =
                std::unique_ptr<FileIconImage>(static_cast<FileIconImage*>(g_task_propagate_pointer(task, nullptr)));
            const bool failed = !image;
            if (failed) {
              image = std::make_unique<FileIconImage>();
              image->ready = true;
            }
            --owner->active;
            if (request->lane == IoLane::NetworkPreviewProbe) {
              owner->networkActive = false;
            }
            if (owner->alive && owner->generation == request->generation) {
              const auto key = keyFor(request->source, request->pixels);
              if (failed) {
                owner->cache.at(key).retry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
              }
              image->sourceKey = "icon:" + key;
              image->revision = "artwork-v1:" + std::to_string(request->generation);
              *request->image = std::move(*image);
              owner->trim(false);
              const auto changed = owner->changed;
              if (changed) {
                changed();
              }
            }
            owner->pump();
          },
          nullptr
      ));
      g_task_set_task_data(task.get(), request, [](gpointer data) { delete static_cast<Request*>(data); });
      runIoTask(task.get(), request->lane, [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
        const auto& request = *static_cast<Request*>(data);
        auto image = std::make_unique<FileIconImage>();
        NetworkMessage args{
            {"action", "icon"},
            {"source", request.source},
            {"size", request.pixels},
            {"theme", IconResolver::activeThemeName()},
            {"generation", IconResolver::themeGeneration()}
        };
        if (request.colorScheme) {
          args["roots"] = request.colorScheme->roots;
          args["stylesheet"] = request.colorScheme->stylesheet;
        }
        auto value = NetworkProcess::acquire(request.lane)->call(std::move(args), cancellationToken(cancel));
        if (!value.at("pixels").is_null()) {
          image->pixels = std::make_shared<const LoadedImageFile>(decodePixels(std::move(value.at("pixels"))));
        }
        image->path = value.at("path").get<std::string>();
        image->symbolic = value.at("symbolic").get<bool>();
        image->ready = true;
        g_task_return_pointer(task, image.release(), [](gpointer data) { delete static_cast<FileIconImage*>(data); });
      });
    }
  }
};

luciole::FileIconCache::FileIconCache() : m_state(std::make_shared<State>()) {}
luciole::FileIconCache::~FileIconCache() {
  m_state->alive = false;
  m_state->cancel.cancel();
  m_state->changed = {};
  m_state->pending.clear();
}
void luciole::FileIconCache::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void luciole::FileIconCache::setColorScheme(std::vector<std::string> roots, std::string stylesheet) {
  m_state->colorScheme = roots.empty()
      ? nullptr
      : std::make_shared<const IconColorScheme>(
            IconColorScheme{.roots = std::move(roots), .stylesheet = std::move(stylesheet)}
        );
}
void luciole::FileIconCache::invalidate() {
  ++m_state->generation;
  m_state->cancel.cancel();
  m_state->cancel = CancelToken{};
  m_state->pending.clear();
  m_state->cache.clear();
}
std::shared_ptr<const luciole::FileIconImage>
luciole::FileIconCache::request(const FileIconSource& source, int pixels) {
  m_state->trim(false);
  pixels = std::clamp(pixels, 1, 512);
  const auto key = keyFor(source, pixels);
  if (const auto found = m_state->cache.find(key); found != m_state->cache.end()) {
    if (std::chrono::steady_clock::now() < found->second.retry) {
      found->second.used = ++m_state->clock;
      return found->second.image;
    }
    m_state->cache.erase(found);
  }
  m_state->trim(true);
  if (m_state->cache.size() >= kMaxEntries) {
    return {};
  }
  auto image = std::make_shared<FileIconImage>();
  m_state->cache.emplace(key, State::Item{image, ++m_state->clock});
  const auto& path = !source.desktopFile.empty() ? source.desktopFile : source.filePath;
  const bool network = !path.empty() && ioLane(Location::fromPath(path)) == IoLane::Network;
  m_state->pending.push_back(
      {m_state, source, image, network ? IoLane::NetworkPreviewProbe : IoLane::PreviewProbe, pixels,
       m_state->generation, m_state->colorScheme}
  );
  m_state->pump();
  return image;
}

void luciole::FileIconCache::imageChanged() {
  if (m_state->alive && m_state->changed) {
    m_state->changed();
  }
}

luciole::NetworkMessage luciole::networkIconRequest(const NetworkMessage& request) {
  const auto value = loadIcon(
      request.at("source").get<FileIconSource>(), request.at("size").get<int>(), request.at("theme").get<std::string>(),
      request.at("generation").get<std::uint64_t>(),
      {.roots = request.value("roots", std::vector<std::string>{}),
       .stylesheet = request.value("stylesheet", std::string{})}
  );
  return {
      {"pixels", value.pixels ? encodePixels(*value.pixels) : NetworkMessage(nullptr)},
      {"path", value.path},
      {"symbolic", value.symbolic}
  };
}
