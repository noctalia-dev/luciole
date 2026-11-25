#include "imaging/pixbuf_image_decoder.h"

#include "imaging/image_limits.h"
#include "imaging/raster_decoder.h"
#include "imaging/svg_rasterizer.h"
#include "render/core/image_decoder.h"
#include "render/core/image_resize.h"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace {
  using luciole::kMaxEncodedImageBytes;
  using luciole::kMaxImagePixels;
  using luciole::kMaxImageSide;

  class PixbufImageDecoder final : public ImageDecoder {
  public:
    std::expected<DecodedRasterImage, std::string>
    decode(const std::uint8_t* data, std::size_t size, std::size_t maxPixels) override {
      if (!data || !size || size > kMaxEncodedImageBytes) {
        return std::unexpected("encoded image exceeds its budget");
      }
      const std::uint64_t pixelBudget =
          maxPixels > 0 ? std::min<std::uint64_t>(maxPixels, kMaxImagePixels) : kMaxImagePixels;
      auto image = luciole::decodeRasterImage({data, size}, kMaxImageSide, pixelBudget, nullptr);
      if (!image) {
        return std::unexpected(image.error());
      }
      if (image->width != image->sourceWidth || image->height != image->sourceHeight) {
        return std::unexpected("decoded image exceeds its side limit");
      }
      return DecodedRasterImage{.pixels = std::move(image->rgba), .width = image->width, .height = image->height};
    }

    std::expected<LoadedImageFile, std::string>
    loadFile(const std::string& path, int targetSize, bool centerSquareCrop) override {
      std::vector<std::uint8_t> bytes;
      if (path.starts_with("data:")) {
        const auto comma = path.find(',');
        if (comma == std::string::npos
            || !std::string_view(path).substr(0, comma).ends_with(";base64")
            || path.size() - comma - 1 > kMaxEncodedImageBytes) {
          return std::unexpected("invalid or oversized image data URI");
        }
        const auto payload = std::string_view(path).substr(comma + 1);
        if (payload.size() % 4 != 0 || !std::ranges::all_of(payload, [](unsigned char c) {
              return (c >= 'A' && c <= 'Z')
                  || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9')
                  || c == '+'
                  || c == '/'
                  || c == '=';
            })) {
          return std::unexpected("invalid image base64 payload");
        }
        gsize length = 0;
        const std::unique_ptr<guchar, decltype(&g_free)> decoded(
            g_base64_decode(path.c_str() + comma + 1, &length), &g_free
        );
        bytes.assign(decoded.get(), decoded.get() + length);
      } else {
        const auto extension = std::filesystem::path(path).extension();
        if (extension == ".svg" || extension == ".SVG") {
          return luciole::rasterizeSvg(path, targetSize, centerSquareCrop);
        }
        auto file = luciole::readEncodedImageFile(path);
        if (!file) {
          return std::unexpected(file.error());
        }
        bytes = std::move(*file);
      }
      auto image = luciole::decodeRasterImage(
          bytes, targetSize > 0 && !centerSquareCrop ? std::min(targetSize, kMaxImageSide) : kMaxImageSide,
          kMaxImagePixels, nullptr
      );
      if (!image || !centerSquareCrop) {
        return image;
      }
      const int side = std::min(image->width, image->height);
      const int left = (image->width - side) / 2, top = (image->height - side) / 2;
      std::vector<std::uint8_t> cropped(static_cast<std::size_t>(side) * side * 4);
      for (int y = 0; y < side; ++y) {
        std::copy_n(
            image->rgba.data() + (static_cast<std::size_t>(top + y) * image->width + left) * 4,
            static_cast<std::size_t>(side) * 4, cropped.data() + static_cast<std::size_t>(y) * side * 4
        );
      }
      const int outputSide = targetSize > 0 ? std::min(side, targetSize) : side;
      image->rgba = outputSide == side
          ? std::move(cropped)
          : resizeToRgba(cropped.data(), side, side, ResizeSourceLayout::Rgba, outputSide, outputSide);
      if (image->rgba.empty()) {
        return std::unexpected("cannot resize cropped image");
      }
      image->width = image->height = outputSide;
      return image;
    }
  };
} // namespace

void luciole::installPixbufImageDecoder() { setImageDecoder(std::make_unique<PixbufImageDecoder>()); }
