#pragma once

namespace luciole {
  inline constexpr int kMaxPreviewImageSide = 2048;
  enum class ThumbnailPurpose { Thumbnail, Preview };
  enum class ThumbnailPolicy { Never, Local, All };
  struct ThumbnailOptions {
    ThumbnailPolicy policy = ThumbnailPolicy::Local;
    int maxFileMiB = 64; // Source cap for files outside the seekable video/audio providers.
    int cacheMiB = 1024;
    bool operator==(const ThumbnailOptions&) const = default;
  };
} // namespace luciole
