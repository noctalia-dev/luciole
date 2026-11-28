#pragma once

#include "model/thumbnail_options.h"
#include "render/core/image_decoder.h"

#include <gio/gio.h>
#include <string_view>

namespace luciole {
  inline constexpr std::string_view kVideoThumbnailVersion = "5";
  // Video browsing thumbnails prefer an embedded cover, else a sampled frame, and optionally decorate either with the
  // system filmstrip helper; previews retain clean pictures.
  std::expected<LoadedImageFile, std::string> renderThumbnailProvider(
      int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable,
      ThumbnailPurpose purpose = ThumbnailPurpose::Preview
  );
} // namespace luciole
