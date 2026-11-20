#pragma once

#include "model/thumbnail_options.h"
#include "render/core/image_decoder.h"

#include <filesystem>
#include <gio/gio.h>
#include <memory>
#include <string_view>

namespace luciole {
  inline constexpr std::string_view kVideoThumbnailVersion = "3";
  struct RemoveThumbnailDirectory {
    void operator()(char* path) const;
  };
  using ThumbnailDirectory = std::unique_ptr<char, RemoveThumbnailDirectory>;
  ThumbnailDirectory makeThumbnailDirectory();
  // Video browsing thumbnails optionally use the system filmstrip helper; previews retain clean frames.
  std::expected<LoadedImageFile, std::string> renderThumbnailProvider(
      int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable,
      ThumbnailPurpose purpose = ThumbnailPurpose::Preview, const std::filesystem::path& workspace = {}
  );
} // namespace luciole
