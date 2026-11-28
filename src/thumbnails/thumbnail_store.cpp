#include "thumbnails/thumbnail_store.h"

#include "fs/gio_ptr.h"
#include "fs/posix_fd.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <format>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <mutex>
#include <ranges>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::string_view directory(int side) {
    return side <= 128 ? "normal" : side <= 256 ? "large" : side <= 512 ? "x-large" : "xx-large";
  }
  constexpr std::array kSides{128, 256, 512, 1024};
  // The cache size found by the last full scan plus Luciole's writes since, so most batches skip rescanning a
  // shared cache that can hold hundreds of thousands of files. Other applications' writes are seen at the next scan.
  struct CacheEstimate {
    std::mutex mutex;
    std::filesystem::path root;
    std::uint64_t budget = 0;
    std::uint64_t bytes = 0;
    std::chrono::steady_clock::time_point scanned;
    bool valid = false;
  };
  CacheEstimate& cacheEstimate() {
    static CacheEstimate estimate;
    return estimate;
  }
  constexpr auto kRescanInterval = std::chrono::seconds(60);
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
  struct CachedFile {
    std::filesystem::path path;
    int side;
    std::uint64_t bytes;
    timespec modified;
  };
  // Regular, user-owned, spec-named PNGs in the four size folders; anything else is left alone.
  std::vector<CachedFile> cachedFiles(const std::filesystem::path& root) {
    std::vector<CachedFile> files;
    for (const int side : kSides) {
      std::error_code error;
      for (const auto& item : std::filesystem::directory_iterator(root / directory(side), error)) {
        const auto leaf = item.path().filename().native();
        struct stat info{};
        if (leaf.size() == 36
            && leaf.ends_with(".png")
            && leaf.substr(0, 32).find_first_not_of("0123456789abcdef") == std::string::npos
            && ::lstat(item.path().c_str(), &info) == 0
            && S_ISREG(info.st_mode)
            && info.st_uid == ::getuid()) {
          files.push_back({item.path(), side, static_cast<std::uint64_t>(info.st_size), info.st_mtim});
        }
      }
    }
    return files;
  }

} // namespace

luciole::ThumbnailStore::ThumbnailStore() : m_root(cacheHome() / "thumbnails") {}
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
  struct stat info{};
  if (!file.valid()
      || ::fstat(file.get(), &info) < 0
      || !S_ISREG(info.st_mode)
      || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > maximum) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
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
  if (thumbnails.empty() || g_cancellable_is_cancelled(cancellable) || !safeDirectory(m_root)) {
    return;
  }
  std::array<bool, 4> prepared{};
  bool written = false;
  std::uint64_t writtenBytes = 0;
  for (const auto& entry : thumbnails) {
    if (g_cancellable_is_cancelled(cancellable)) {
      break;
    }
    const auto uri = entry.thumbnail.text.find("Thumb::URI");
    if (uri == entry.thumbnail.text.end()) {
      continue;
    }
    const auto png = writeThumbnailPng(entry.thumbnail);
    if (!png || png->size() > budget || png->size() > 8U * 1024U * 1024U) {
      continue;
    }
    const std::size_t classIndex = entry.side <= 128 ? 0 : entry.side <= 256 ? 1 : entry.side <= 512 ? 2 : 3;
    const auto folder = m_root / directory(entry.side);
    if (!prepared[classIndex]) {
      if (!safeDirectory(folder)) {
        continue;
      }
      prepared[classIndex] = true;
    }
    std::string pattern = (folder / ".luciole-XXXXXX").native();
    const luciole::Fd file(::mkstemp(pattern.data()));
    if (!file.valid()) {
      continue;
    }
    std::size_t offset = 0;
    while (offset < png->size()) {
      const auto count = ::write(file.get(), png->data() + offset, png->size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        break;
      }
      offset += static_cast<std::size_t>(count);
    }
    // The cache is shared and regenerable: the newest thumbnail for a URI replaces whatever is there.
    if (offset == png->size()
        && !g_cancellable_is_cancelled(cancellable)
        && ::rename(pattern.c_str(), (folder / filename(uri->second)).c_str()) == 0) {
      written = true;
      writtenBytes += png->size();
    } else {
      (void)::unlink(pattern.c_str());
    }
  }
  if (!written) {
    return;
  }
  {
    auto& estimate = cacheEstimate();
    const std::scoped_lock lock(estimate.mutex);
    if (estimate.valid
        && estimate.root == m_root
        && estimate.budget == budget
        && std::chrono::steady_clock::now() - estimate.scanned < kRescanInterval
        && writtenBytes <= budget - std::min(budget, estimate.bytes)) {
      // Counts replacements of existing thumbnails as growth, so this errs towards an earlier rescan.
      estimate.bytes += writtenBytes;
      return;
    }
  }
  prune(budget);
}
std::string luciole::ThumbnailStore::clear() {
  {
    auto& estimate = cacheEstimate();
    const std::scoped_lock lock(estimate.mutex);
    estimate.valid = false;
  }
  for (const auto& file : cachedFiles(m_root)) {
    const auto bytes = readThumbnailFile(file.path, 8U * 1024U * 1024U);
    if (!bytes) {
      continue;
    }
    const auto image = readThumbnailPng(*bytes, file.side);
    // Other applications' thumbnails survive an explicit clear; the budget prune may still age them out.
    if (image
        && image->text.contains("Software")
        && image->text.at("Software") == "Luciole"
        && ::unlink(file.path.c_str()) < 0
        && errno != ENOENT) {
      return std::format("cannot remove {}", file.path.string());
    }
  }
  return {};
}
void luciole::ThumbnailStore::prune(std::uint64_t budget) {
  auto files = cachedFiles(m_root);
  std::uint64_t bytes = 0;
  for (const auto& file : files) {
    bytes += file.bytes;
  }
  if (bytes > budget) {
    std::ranges::sort(files, [](const CachedFile& a, const CachedFile& b) {
      return std::tie(a.modified.tv_sec, a.modified.tv_nsec) < std::tie(b.modified.tv_sec, b.modified.tv_nsec);
    });
    for (const auto& file : files) {
      if (bytes <= budget) {
        break;
      }
      if (::unlink(file.path.c_str()) == 0) {
        bytes -= file.bytes;
      }
    }
  }
  auto& estimate = cacheEstimate();
  const std::scoped_lock lock(estimate.mutex);
  estimate.root = m_root;
  estimate.budget = budget;
  estimate.bytes = bytes;
  estimate.scanned = std::chrono::steady_clock::now();
  estimate.valid = true;
}
