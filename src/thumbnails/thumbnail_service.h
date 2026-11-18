#pragma once

#include "fs/directory_session.h"
#include "model/thumbnail_options.h"
#include "thumbnails/file_icon_cache.h"

namespace luciole {
  // Adapted from Noctalia's ThumbnailService acquire/release and worker publication model.
  // One process-wide cache for browsing thumbnails and preview images: one memory budget, one decoder pool.
  // Cells borrow immutable prepared pixels; the shared texture cache owns upload and residency.
  class ThumbnailService {
  public:
    // Safety caps only: released images normally leave when they go unseen past an idle limit.
    static constexpr std::size_t kResidentBudget = 256U * 1024U * 1024U;
    static constexpr std::size_t kMaxEntries = 4096;
    // Whether reading a local path may go over a network; the Local policy generates nothing there. Called on
    // worker threads.
    using NetworkFilesystemCheck = std::function<bool(const std::string& path)>;
    ThumbnailService();
    explicit ThumbnailService(NetworkFilesystemCheck networkFilesystem);
    // Concurrent decoder helpers: a quarter of the CPUs, at least two and at most six.
    static std::size_t decoders();
    ~ThumbnailService();
    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;
    // Preview requests decode images up to kMaxPreviewImageSide, ignore the browsing policy, and take the next free
    // decoder ahead of thumbnails.
    std::shared_ptr<const FileIconImage> request(
        const DirectoryEntry& source, int pixels, bool visible = true,
        ThumbnailPurpose purpose = ThumbnailPurpose::Thumbnail
    );
    static int pixelSizeClass(int pixels, ThumbnailPurpose purpose);
    // The largest ready image of `source` at any size and purpose.
    std::shared_ptr<const FileIconImage> prepared(const DirectoryEntry& source);
    bool touch(const std::shared_ptr<const FileIconImage>& image, bool visible);
    void synchronizeSources(const DirectorySnapshot& snapshot);
    void setOptions(ThumbnailOptions options);
    const ThumbnailOptions& options() const;
    void setChangedCallback(std::function<void()> callback);
    void clearCache();
    // Starts queued work and cancels work nobody holds. Released images stay resident until they go unseen for five
    // minutes (thirty seconds for previews) or the safety budget evicts the least recently used.
    void endFrame();
    bool clearing() const;
    const std::string& error() const;
    std::size_t reservedBytes() const;
    std::size_t activeJobs() const;
    std::size_t queuedJobs() const;
    // Cancel only this location's cached/queued/in-flight reads before explicit drive removal.
    void releaseLocation(const Location& root);
    bool readingLocation(const Location& root) const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
