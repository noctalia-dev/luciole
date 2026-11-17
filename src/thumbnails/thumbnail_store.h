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
  // Worker-only access to the freedesktop PNG cache. Only files recorded by Luciole are pruned/cleared.
  class ThumbnailStore {
  public:
    // Images per ledger transaction: each transaction rewrites the whole ledger, whose size grows with the budget.
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
    // At most kMaxBatch prepared images, committed under one ownership-ledger transaction. Only the byte budget
    // bounds the ledger; the oldest owned files are removed to stay within it.
    void writeBatch(std::span<const ThumbnailCacheWrite> thumbnails, std::uint64_t budget, GCancellable* cancellable);
    std::string prune(std::uint64_t budget);
    std::string clear();

  private:
    std::filesystem::path m_root;
    std::filesystem::path m_index;
  };
  std::optional<std::vector<std::uint8_t>> readThumbnailFile(const std::filesystem::path& path, std::size_t maximum);
} // namespace luciole
