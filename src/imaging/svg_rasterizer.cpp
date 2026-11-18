#include "imaging/svg_rasterizer.h"

#include "fs/gio_ptr.h"
#include "imaging/image_limits.h"
#include "imaging/raster_decoder.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <librsvg/rsvg.h>

namespace {
  // Convert Cairo's native-endian premultiplied pixels back to straight RGBA.
  void copyPixels(cairo_surface_t* surface, LoadedImageFile& image) {
    cairo_surface_flush(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < image.height; ++y) {
      const auto* row = reinterpret_cast<const std::uint32_t*>(data + y * stride);
      for (int x = 0; x < image.width; ++x) {
        const auto pixel = row[x];
        const auto alpha = (pixel >> 24U) & 255U;
        const auto index = (static_cast<std::size_t>(y) * image.width + x) * 4U;
        for (std::size_t channel = 0; channel < 3; ++channel) {
          const auto value = (pixel >> (16U - static_cast<unsigned>(channel) * 8U)) & 255U;
          image.rgba[index + channel] =
              static_cast<std::uint8_t>(alpha == 0 ? 0 : std::min(255U, (value * 255U + alpha / 2U) / alpha));
        }
        image.rgba[index + 3] = static_cast<std::uint8_t>(alpha);
      }
    }
  }
} // namespace

std::expected<LoadedImageFile, std::string>
luciole::rasterizeSvg(const std::string& path, int targetSize, bool centerSquareCrop, std::string_view stylesheet) {
  auto bytes = readEncodedImageFile(path);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  const std::string_view content(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  return rasterizeSvg(
      *bytes, targetSize, centerSquareCrop, static_cast<std::uint64_t>(kMaxSvgSourceSide * kMaxSvgSourceSide),
      content.contains("current-color-scheme") ? stylesheet : std::string_view()
  );
}

std::expected<LoadedImageFile, std::string> luciole::rasterizeSvg(
    std::span<const std::uint8_t> bytes, int targetSize, bool centerSquareCrop, std::uint64_t maxSourcePixels,
    std::string_view stylesheet
) {
  if (bytes.empty() || bytes.size() > kMaxEncodedImageBytes || !maxSourcePixels) {
    return std::unexpected("invalid SVG decode input");
  }
  GError* error = nullptr;
  const GioPtr<RsvgHandle> svg(rsvg_handle_new_from_data(bytes.data(), bytes.size(), &error));
  if (!svg) {
    const std::string message = error != nullptr ? error->message : "invalid SVG";
    g_clear_error(&error);
    return std::unexpected(message);
  }
  if (!stylesheet.empty()
      && !rsvg_handle_set_stylesheet(
          svg.get(), reinterpret_cast<const guint8*>(stylesheet.data()), stylesheet.size(), &error
      )) {
    const std::string message = error != nullptr ? error->message : "invalid SVG stylesheet";
    g_clear_error(&error);
    return std::unexpected(message);
  }
  double sourceWidth = 0;
  double sourceHeight = 0;
  if (!rsvg_handle_get_intrinsic_size_in_pixels(svg.get(), &sourceWidth, &sourceHeight)) {
    gboolean hasWidth = FALSE, hasHeight = FALSE, hasViewbox = FALSE;
    RsvgLength width{}, height{};
    RsvgRectangle viewbox{};
    rsvg_handle_get_intrinsic_dimensions(svg.get(), &hasWidth, &width, &hasHeight, &height, &hasViewbox, &viewbox);
    sourceWidth = hasViewbox ? viewbox.width : 256.0;
    sourceHeight = hasViewbox ? viewbox.height : 256.0;
  }
  if (!std::isfinite(sourceWidth)
      || !std::isfinite(sourceHeight)
      || sourceWidth <= 0
      || sourceHeight <= 0
      || sourceWidth > kMaxSvgSourceSide
      || sourceHeight > kMaxSvgSourceSide) {
    return std::unexpected("invalid image dimensions");
  }
  if (sourceWidth * sourceHeight > static_cast<double>(maxSourcePixels)) {
    return std::unexpected("image exceeds source pixel limit");
  }
  const double cropWidth = centerSquareCrop ? std::min(sourceWidth, sourceHeight) : sourceWidth;
  const double cropHeight = centerSquareCrop ? cropWidth : sourceHeight;
  const double scale = targetSize > 0
      ? static_cast<double>(std::min(targetSize, kMaxSvgRasterSide)) / std::max(cropWidth, cropHeight)
      : 1.0;
  const int width = std::max(1, static_cast<int>(std::round(cropWidth * scale)));
  const int height = std::max(1, static_cast<int>(std::round(cropHeight * scale)));
  if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxImagePixels) {
    return std::unexpected("decoded image exceeds its budget");
  }
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return std::unexpected("cannot allocate resized image");
  }
  cairo_t* cr = cairo_create(surface);
  cairo_scale(cr, static_cast<double>(width) / cropWidth, static_cast<double>(height) / cropHeight);
  cairo_translate(cr, -(sourceWidth - cropWidth) / 2.0, -(sourceHeight - cropHeight) / 2.0);
  const RsvgRectangle viewport{.x = 0, .y = 0, .width = sourceWidth, .height = sourceHeight};
  const bool success =
      rsvg_handle_render_document(svg.get(), cr, &viewport, nullptr) && cairo_status(cr) == CAIRO_STATUS_SUCCESS;
  cairo_destroy(cr);
  LoadedImageFile image{
      .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U),
      .width = width,
      .height = height,
      .sourceWidth = static_cast<int>(sourceWidth),
      .sourceHeight = static_cast<int>(sourceHeight),
  };
  if (success) {
    copyPixels(surface, image);
  }
  cairo_surface_destroy(surface);
  if (!success) {
    return std::unexpected("image rasterization failed");
  }
  return image;
}
