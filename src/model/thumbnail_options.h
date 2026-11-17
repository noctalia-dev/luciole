#pragma once

namespace luciole {
  inline constexpr int kMaxPreviewImageSide = 2048;
  enum class ThumbnailPurpose { Thumbnail, Preview };
  enum class ThumbnailPolicy { Never, Local, All };
  struct ThumbnailOptions {
    ThumbnailPolicy policy = ThumbnailPolicy::Local;
    int maxFileMiB = 64; // Image/PDF source cap; seekable video/audio providers have separate resource limits.
    int cacheMiB = 1024;
    bool operator==(const ThumbnailOptions&) const = default;
  };
} // namespace luciole
