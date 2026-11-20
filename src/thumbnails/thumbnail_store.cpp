#include "thumbnails/thumbnail_store.h"

#include "fs/atomic_file.h"
#include "fs/gio_ptr.h"
#include "fs/posix_fd.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <format>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::string_view directory(int side) {
    return side <= 128 ? "normal" : side <= 256 ? "large" : side <= 512 ? "x-large" : "xx-large";
  }
  std::string cacheIdentity(const struct stat& info) {
    return std::format(
        "{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_size, info.st_mtim.tv_sec, info.st_mtim.tv_nsec
    );
  }
  bool safeDirectory(const std::filesystem::path& path) {
    if (::mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) {
      return false;
    }
    const luciole::Fd directory(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat info{};
    return directory.valid()
        && ::fstat(directory.get(), &info) == 0
        && info.st_uid == ::getuid()
        && ::fchmod(directory.get(), 0700) == 0;
  }
  bool safeName(const std::string& name) {
    const auto slash = name.find('/');
    if (slash == std::string::npos) {
      return false;
    }
    const auto prefix = name.substr(0, slash);
    const auto leaf = name.substr(slash + 1);
    return (prefix == "normal" || prefix == "large" || prefix == "x-large" || prefix == "xx-large")
        && leaf.size() == 36
        && leaf.ends_with(".png")
        && leaf.substr(0, 32).find_first_not_of("0123456789abcdef") == std::string::npos;
  }
  nlohmann::json readIndex(const std::optional<std::string>& text) {
    if (!text) {
      return nlohmann::json::array();
    }
    auto index = nlohmann::json::parse(*text);
    if (!index.is_array()) {
      throw std::runtime_error("invalid thumbnail index");
    }
    for (const auto& item : index) {
      if (!item.is_object()
          || !item.contains("name")
          || !item["name"].is_string()
          || !safeName(item["name"].get<std::string>())
          || !item.contains("bytes")
          || !item["bytes"].is_number_unsigned()
          || item["bytes"].get<std::uint64_t>() > 8U * 1024U * 1024U
          || !item.contains("identity")
          || !item["identity"].is_string()) {
        throw std::runtime_error("invalid thumbnail index entry");
      }
    }
    return index;
  }
  void removeOwned(const std::filesystem::path& root, const std::string& name, const std::string& identity) {
    if (!safeName(name)) {
      return;
    }
    struct stat info{};
    if (::lstat((root / name).c_str(), &info) == 0
        && S_ISREG(info.st_mode)
        && info.st_uid == ::getuid()
        && cacheIdentity(info) == identity) {
      (void)::unlink((root / name).c_str());
    }
  }

} // namespace

luciole::ThumbnailStore::ThumbnailStore() : m_root(cacheHome() / "thumbnails") {
  m_index = m_root.parent_path() / "luciole/thumbnail-index.json";
}
int luciole::ThumbnailStore::sizeClass(int pixels) {
  return pixels <= 128 ? 128 : pixels <= 256 ? 256 : pixels <= 512 ? 512 : 1024;
}
std::string luciole::ThumbnailStore::filename(const std::string& uri) {
  const GString hash(g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), static_cast<gssize>(uri.size())));
  return std::string(hash.get()) + ".png";
}
std::optional<std::vector<std::uint8_t>>
luciole::readThumbnailFile(const std::filesystem::path& path, std::size_t maximum) {
  const luciole::Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  return readThumbnailFile(file.get(), maximum);
}
std::optional<std::vector<std::uint8_t>> luciole::readThumbnailFile(int fd, std::size_t maximum) {
  struct stat info{};
  if (fd < 0
      || ::fstat(fd, &info) < 0
      || !S_ISREG(info.st_mode)
      || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > maximum) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = ::pread(fd, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return std::nullopt;
    }
    offset += static_cast<std::size_t>(n);
  }
  return bytes;
}
std::optional<luciole::ThumbnailPng> luciole::ThumbnailStore::read(
    const std::string& uri, int side, std::uint64_t mtime, std::optional<std::uint64_t> size, const std::string& stamp,
    bool allowSmallerForeign
) {
  const auto leaf = filename(uri);
  std::vector<int> sizes;
  for (const int cachedSide : {128, 256, 512, 1024}) {
    if (cachedSide >= side) {
      sizes.push_back(cachedSide);
    }
  }
  if (allowSmallerForeign) {
    for (const int cachedSide : {1024, 512, 256, 128}) {
      if (cachedSide < side) {
        sizes.push_back(cachedSide);
      }
    }
  }
  for (const int cachedSide : sizes) {
    const auto bytes = readThumbnailFile(m_root / directory(cachedSide) / leaf, 8U * 1024U * 1024U);
    if (!bytes) {
      continue;
    }
    auto image = readThumbnailPng(*bytes, cachedSide);
    if (!image
        || !image->text.contains("Thumb::URI")
        || image->text.at("Thumb::URI") != uri
        || !image->text.contains("Thumb::MTime")
        || image->text.at("Thumb::MTime") != std::to_string(mtime)) {
      continue;
    }
    if (size && image->text.contains("Thumb::Size") && image->text.at("Thumb::Size") != std::to_string(*size)) {
      continue;
    }
    if (!stamp.empty() && image->text.contains("X-Luciole::Stamp") && image->text.at("X-Luciole::Stamp") != stamp) {
      continue;
    }
    if (cachedSide < side && image->text.contains("Software") && image->text.at("Software") == "Luciole") {
      continue;
    }
    if (std::max(image->image.width, image->image.height) > side) {
      if (side < 1) {
        return std::nullopt;
      }
      const auto& source = image->image;
      const double ratio = static_cast<double>(side) / std::max(source.width, source.height);
      const int width = std::max(1, static_cast<int>(std::round(source.width * ratio)));
      const int height = std::max(1, static_cast<int>(std::round(source.height * ratio)));
      const GioPtr<GdkPixbuf> input(gdk_pixbuf_new_from_data(
          source.rgba.data(), GDK_COLORSPACE_RGB, true, 8, source.width, source.height, source.width * 4, nullptr,
          nullptr
      ));
      const GioPtr<GdkPixbuf> scaled(
          input ? gdk_pixbuf_scale_simple(input.get(), width, height, GDK_INTERP_BILINEAR) : nullptr
      );
      if (!scaled) {
        return std::nullopt;
      }
      LoadedImageFile resized{
          .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4),
          .width = width,
          .height = height,
          .sourceWidth = source.width,
          .sourceHeight = source.height
      };
      const auto* pixels = gdk_pixbuf_read_pixels(scaled.get());
      const int stride = gdk_pixbuf_get_rowstride(scaled.get());
      for (int y = 0; y < height; ++y) {
        std::copy_n(
            pixels + static_cast<std::size_t>(y) * stride, width * 4,
            resized.rgba.data() + static_cast<std::size_t>(y) * width * 4
        );
      }
      image->image = std::move(resized);
    }
    return std::move(*image);
  }
  return std::nullopt;
}
void luciole::ThumbnailStore::write(ThumbnailPng thumbnail, int side, std::uint64_t budget, GCancellable* cancellable) {
  thumbnail.text["Software"] = "Luciole";
  const std::array entries{ThumbnailCacheWrite{.thumbnail = std::move(thumbnail), .side = side}};
  writeBatch(entries, budget, cancellable);
}
void luciole::ThumbnailStore::writeBatch(
    std::span<const ThumbnailCacheWrite> thumbnails, std::uint64_t budget, GCancellable* cancellable
) {
  if (thumbnails.empty() || thumbnails.size() > kMaxBatch || g_cancellable_is_cancelled(cancellable)) {
    return;
  }
  struct Encoded {
    std::string name;
    std::vector<std::uint8_t> bytes;
    int side;
  };
  std::vector<Encoded> encoded;
  encoded.reserve(thumbnails.size());
  for (const auto& entry : thumbnails) {
    if (g_cancellable_is_cancelled(cancellable)) {
      return;
    }
    const auto uri = entry.thumbnail.text.find("Thumb::URI");
    if (uri == entry.thumbnail.text.end()) {
      continue;
    }
    auto png = writeThumbnailPng(entry.thumbnail);
    if (!png || png->size() > budget || png->size() > 8U * 1024U * 1024U) {
      continue;
    }
    encoded.push_back({std::string(directory(entry.side)) + '/' + filename(uri->second), std::move(*png), entry.side});
  }
  if (encoded.empty()) {
    return;
  }
  // Each visible batch pays for one ledger read, merge and durable replacement.
  std::vector<std::pair<std::string, std::string>> written;
  written.reserve(encoded.size());
  const auto outcome = updateTextFileAtomic(
      m_index, [&](const std::optional<std::string>& text) -> std::expected<std::optional<std::string>, std::string> {
        try {
          auto index = readIndex(text);
          std::uint64_t bytes = 0;
          for (const auto& item : index) {
            bytes += item["bytes"].get<std::uint64_t>();
          }
          if (!safeDirectory(m_root)) {
            return std::unexpected("thumbnail cache directory is not private and owned");
          }
          std::array<bool, 4> prepared{};
          for (const auto& entry : encoded) {
            if (g_cancellable_is_cancelled(cancellable)) {
              break;
            }
            // Shared-cache images can belong to another file manager, even if stale or unreadable.
            // Never claim them merely because their URI hashes to the same destination.
            const auto target = m_root / entry.name;
            const auto owned =
                std::ranges::find_if(index, [&](const auto& item) { return item["name"] == entry.name; });
            struct stat previous{};
            std::string previousIdentity;
            if (::lstat(target.c_str(), &previous) == 0) {
              if (owned == index.end()
                  || !S_ISREG(previous.st_mode)
                  || previous.st_uid != ::getuid()
                  || cacheIdentity(previous) != (*owned)["identity"].template get<std::string>()) {
                continue;
              }
              previousIdentity = cacheIdentity(previous);
            } else if (errno != ENOENT) {
              continue;
            }
            for (auto it = index.begin(); it != index.end();) {
              if ((*it)["name"] == entry.name) {
                bytes -= (*it)["bytes"].get<std::uint64_t>();
                it = index.erase(it);
              } else {
                ++it;
              }
            }
            while (!index.empty() && bytes + entry.bytes.size() > budget) {
              const auto& oldest = index.front();
              bytes -= oldest["bytes"].get<std::uint64_t>();
              removeOwned(m_root, oldest["name"].get<std::string>(), oldest["identity"].get<std::string>());
              index.erase(index.begin());
            }
            const std::size_t classIndex = entry.side <= 128 ? 0 : entry.side <= 256 ? 1 : entry.side <= 512 ? 2 : 3;
            if (!prepared[classIndex]) {
              if (!safeDirectory(m_root / directory(entry.side))) {
                return std::unexpected("thumbnail cache directory is not private and owned");
              }
              prepared[classIndex] = true;
            }
            std::string pattern = (m_root / directory(entry.side) / ".luciole-XXXXXX").native();
            const luciole::Fd file(::mkstemp(pattern.data()));
            if (!file.valid()) {
              return std::unexpected("cannot create thumbnail cache file");
            }
            std::size_t offset = 0;
            while (offset < entry.bytes.size()) {
              const auto count = ::write(file.get(), entry.bytes.data() + offset, entry.bytes.size() - offset);
              if (count < 0 && errno == EINTR) {
                continue;
              }
              if (count <= 0) {
                (void)::unlink(pattern.c_str());
                return std::unexpected("cannot write thumbnail cache file");
              }
              offset += static_cast<std::size_t>(count);
            }
            if (g_cancellable_is_cancelled(cancellable)) {
              (void)::unlink(pattern.c_str());
              break;
            }
            if (!previousIdentity.empty()) {
              removeOwned(m_root, entry.name, previousIdentity);
            }
            // A competing publisher must not be overwritten between the ownership check and publication.
            const bool published = ::link(pattern.c_str(), target.c_str()) == 0;
            (void)::unlink(pattern.c_str());
            if (!published) {
              continue;
            }
            struct stat info{};
            if (::fstat(file.get(), &info) < 0) {
              return std::unexpected("cannot identify cached thumbnail");
            }
            const auto identity = cacheIdentity(info);
            written.emplace_back(entry.name, identity);
            index.push_back({{"name", entry.name}, {"bytes", entry.bytes.size()}, {"identity", identity}});
            bytes += entry.bytes.size();
          }
          return written.empty() ? std::optional<std::string>{} : index.dump();
        } catch (const std::exception& error) {
          return std::unexpected(error.what());
        }
      }
  );
  if (outcome.state == AtomicWriteState::NotCommitted) {
    for (const auto& [name, identity] : written) {
      removeOwned(m_root, name, identity);
    }
  }
}
std::string luciole::ThumbnailStore::clear() { return prune(0); }
std::string luciole::ThumbnailStore::prune(std::uint64_t budget) {
  const auto result = updateTextFileAtomic(
      m_index, [&](const std::optional<std::string>& text) -> std::expected<std::optional<std::string>, std::string> {
        try {
          auto index = readIndex(text);
          std::uint64_t bytes = 0;
          for (const auto& item : index) {
            bytes += item["bytes"].get<std::uint64_t>();
          }
          while (!index.empty() && bytes > budget) {
            const auto& item = index.front();
            bytes -= item["bytes"].get<std::uint64_t>();
            removeOwned(m_root, item["name"].get<std::string>(), item["identity"].get<std::string>());
            index.erase(index.begin());
          }
          return index.dump();
        } catch (const std::exception& error) {
          return std::unexpected(error.what());
        }
      }
  );
  return result.error;
}
