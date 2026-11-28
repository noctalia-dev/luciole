#pragma once

#include "thumbnails/thumbnail_codec.h"

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <span>

namespace luciole {
  struct ThumbnailCacheWrite {
    ThumbnailPng thumbnail;
    int side;
  };
  // Worker-only access to the shared freedesktop PNG cache. Writes replace any file for the same URI; the byte
  // budget prunes the oldest regular PNGs regardless of author; clear() removes only Luciole-written ones.
  class ThumbnailStore {
  public:
    // Completed writes gathered per worker batch; a batch rescans the cache's size only when its estimate may exceed
    // the budget, the budget changed, or the last scan is a minute old.
    static constexpr std::size_t kMaxBatch = 64;
    ThumbnailStore();
    const std::filesystem::path& root() const { return m_root; }
    static int sizeClass(int pixels);
    static std::string filename(const std::string& uri);
    std::optional<ThumbnailPng> read(
        const std::string& uri, int side, std::uint64_t mtime, std::optional<std::uint64_t> size,
        const std::string& stamp = {}, bool allowSmallerForeign = false
    );
    void write(ThumbnailPng thumbnail, int side, std::uint64_t budget, GCancellable* cancellable);
    void writeBatch(std::span<const ThumbnailCacheWrite> thumbnails, std::uint64_t budget, GCancellable* cancellable);
    void prune(std::uint64_t budget);
    std::string clear();

  private:
    std::filesystem::path m_root;
  };
  std::optional<std::vector<std::uint8_t>> readThumbnailFile(const std::filesystem::path& path, std::size_t maximum);
} // namespace luciole
