#pragma once

#include "render/core/image_decoder.h"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace luciole {
  // Rasterizes an SVG file with librsvg, optionally center-cropped to a square, at most targetSize pixels per side.
  // A non-empty `stylesheet` is laid over a file that has a `<style id="current-color-scheme">`: the icon-theme
  // color scheme of iconColorSchemeStylesheet().
  std::expected<LoadedImageFile, std::string>
  rasterizeSvg(const std::string& path, int targetSize, bool centerSquareCrop, std::string_view stylesheet = {});
  // Decode captured bytes without a base URI; linked external resources stay unavailable. A non-empty `stylesheet` is
  // applied as librsvg's user stylesheet.
  std::expected<LoadedImageFile, std::string> rasterizeSvg(
      std::span<const std::uint8_t> bytes, int targetSize, bool centerSquareCrop, std::uint64_t maxSourcePixels,
      std::string_view stylesheet = {}
  );
} // namespace luciole
