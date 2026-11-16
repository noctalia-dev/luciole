#include "imaging/raster_decoder.h"

#include "fs/gio_ptr.h"
#include "imaging/image_limits.h"
#include "render/core/image_orientation.h"
#include "render/core/image_resize.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
  struct DecodeSize {
    int target;
    std::uint64_t maxPixels;
    bool rejected = false;
    int width = 0, height = 0;
  };
  void sizePrepared(GdkPixbufLoader* loader, int width, int height, gpointer data) {
    auto& state = *static_cast<DecodeSize*>(data);
    state.width = width;
    state.height = height;
    state.rejected = width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > state.maxPixels;
    const double ratio =
        state.rejected ? 0 : std::min(1.0, static_cast<double>(state.target) / std::max(width, height));
    gdk_pixbuf_loader_set_size(
        loader, std::max(1, static_cast<int>(std::round(width * ratio))),
        std::max(1, static_cast<int>(std::round(height * ratio)))
    );
  }
} // namespace

std::expected<std::vector<std::uint8_t>, std::string> luciole::readEncodedImageFile(const std::string& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return std::unexpected("image path is not a regular file");
  }
  const auto length = std::filesystem::file_size(path, error);
  if (error || length == 0 || length > kMaxEncodedImageBytes) {
    return std::unexpected("encoded image exceeds its budget");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  std::ifstream stream(path, std::ios::binary);
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    return std::unexpected("cannot read image file");
  }
  return bytes;
}

std::expected<LoadedImageFile, std::string> luciole::decodeRasterImage(
    std::span<const std::uint8_t> bytes, int target, std::uint64_t maxPixels, GCancellable* cancellable,
    std::string_view contentType
) {
  if (bytes.empty() || target < 1 || target > kMaxImageSide || !maxPixels) {
    return std::unexpected("invalid image decode input");
  }
  if (g_cancellable_is_cancelled(cancellable)) {
    return std::unexpected("image decode cancelled");
  }
  const GString guessed(
      contentType.empty()
          ? g_content_type_guess(nullptr, bytes.data(), std::min<std::size_t>(bytes.size(), 4096), nullptr)
          : nullptr
  );
  const std::string mime(contentType.empty() ? (guessed ? guessed.get() : "") : contentType);
  // Select an explicit raster loader before handing it bytes. SVG resource loading stays excluded.
  if (!mime.starts_with("image/") || mime.starts_with("image/svg")) {
    return std::unexpected("no supported raster decoder");
  }
  GError* setupError = nullptr;
  const GioPtr<GdkPixbufLoader> loader(gdk_pixbuf_loader_new_with_mime_type(mime.c_str(), &setupError));
  if (!loader) {
    const std::string message = setupError ? setupError->message : "no image loader installed";
    g_clear_error(&setupError);
    return std::unexpected("GdkPixbuf: " + message);
  }
  DecodeSize state{target, maxPixels};
  g_signal_connect(loader.get(), "size-prepared", G_CALLBACK(sizePrepared), &state);
  GError* error = nullptr;
  bool ok = true;
  for (std::size_t offset = 0; offset < bytes.size(); offset += 65536) {
    if (g_cancellable_is_cancelled(cancellable) || state.rejected) {
      ok = false;
      break;
    }
    const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
    if (!gdk_pixbuf_loader_write(loader.get(), bytes.data() + offset, count, &error)) {
      ok = false;
      break;
    }
    if (auto* format = gdk_pixbuf_loader_get_format(loader.get())) {
      const GString name(gdk_pixbuf_format_get_name(format));
      if (name && std::strcmp(name.get(), "svg") == 0) {
        ok = false;
        break;
      }
    }
  }
  std::string failure;
  if (error) {
    failure = error->message;
    g_clear_error(&error);
  }
  if (!gdk_pixbuf_loader_close(loader.get(), &error)) {
    ok = false;
  }
  if (error) {
    if (failure.empty()) {
      failure = error->message;
    }
    g_clear_error(&error);
  }
  auto* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader.get());
  if (g_cancellable_is_cancelled(cancellable)) {
    return std::unexpected("image decode cancelled");
  }
  if (state.rejected) {
    return std::unexpected("image exceeds source pixel limit");
  }
  if (!ok || !pixbuf) {
    return std::unexpected(failure.empty() ? "GdkPixbuf returned no pixels" : "GdkPixbuf: " + failure);
  }
  if (state.width < 1 || state.height < 1) {
    return std::unexpected("image decoder supplied no source dimensions");
  }
  auto image = fitRasterPixels(pixbuf, target, maxPixels);
  if (!image) {
    return image;
  }
  image->sourceWidth = state.width;
  image->sourceHeight = state.height;
  auto orientation = exifOrientation(bytes.data(), bytes.size());
  if (const char* option = gdk_pixbuf_get_option(pixbuf, "orientation")) {
    const std::string_view value(option);
    unsigned parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed >= 1 && parsed <= 8) {
      orientation = static_cast<ImageOrientation>(parsed);
    }
  }
  applyImageOrientation(image->rgba, image->width, image->height, orientation);
  if (orientation == ImageOrientation::Transpose
      || orientation == ImageOrientation::Rotate90
      || orientation == ImageOrientation::Transverse
      || orientation == ImageOrientation::Rotate270) {
    std::swap(image->sourceWidth, image->sourceHeight);
  }
  return image;
}

std::expected<LoadedImageFile, std::string>
luciole::fitRasterPixels(GdkPixbuf* pixbuf, int target, std::uint64_t maxPixels) {
  if (!pixbuf || target < 1 || target > kMaxImageSide) {
    return std::unexpected("invalid decoded pixels");
  }
  const int width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
  if (width < 1
      || height < 1
      || static_cast<std::uint64_t>(width) * height > maxPixels
      || gdk_pixbuf_get_bits_per_sample(pixbuf) != 8) {
    return std::unexpected("image exceeds decoded pixel limits");
  }
  const int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
  if (channels != 3 && channels != 4) {
    return std::unexpected("unsupported image channels");
  }
  const auto* pixels = gdk_pixbuf_read_pixels(pixbuf);
  const auto fitted = fitImageWithin(width, height, target, target);
  if (stride == width * channels && (fitted.width != width || fitted.height != height)) {
    auto resized = resizeToRgba(
        pixels, width, height, channels == 4 ? ResizeSourceLayout::Rgba : ResizeSourceLayout::Rgb, fitted.width,
        fitted.height
    );
    if (resized.empty()) {
      return std::unexpected("cannot scale decoded image");
    }
    return LoadedImageFile{
        .rgba = std::move(resized), .width = fitted.width, .height = fitted.height, .sourceWidth = 0, .sourceHeight = 0
    };
  }
  LoadedImageFile image{
      .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4),
      .width = width,
      .height = height,
      .sourceWidth = 0,
      .sourceHeight = 0
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto source = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * channels;
      const auto dest = (static_cast<std::size_t>(y) * width + x) * 4;
      for (int c = 0; c < 3; ++c) {
        image.rgba[dest + c] = pixels[source + c];
      }
      image.rgba[dest + 3] = channels == 4 ? pixels[source + 3] : 255;
    }
  }
  if (fitted.width != width || fitted.height != height) {
    auto resized =
        resizeToRgba(image.rgba.data(), width, height, ResizeSourceLayout::Rgba, fitted.width, fitted.height);
    if (resized.empty()) {
      return std::unexpected("cannot scale decoded image");
    }
    image.rgba = std::move(resized);
    image.width = fitted.width;
    image.height = fitted.height;
  }
  return image;
}
