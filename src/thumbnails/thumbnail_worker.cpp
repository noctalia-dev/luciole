#include "thumbnails/thumbnail_worker.h"

#include "fs/gio_ptr.h"
#include "imaging/image_limits.h"
#include "imaging/raster_decoder.h"
#include "imaging/svg_rasterizer.h"
#include "model/thumbnail_options.h"
#include "thumbnails/provider_process.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fcntl.h>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  bool writeResult(std::string_view path, std::span<const std::uint8_t> header, std::span<const std::uint8_t> body) {
    const int output = ::open(std::string(path).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
      return false;
    }
    for (const auto data : {header, body}) {
      std::size_t offset = 0;
      while (offset < data.size()) {
        const auto count = ::write(output, data.data() + offset, data.size() - offset);
        if (count <= 0) {
          ::close(output);
          return false;
        }
        offset += static_cast<std::size_t>(count);
      }
    }
    return ::close(output) == 0;
  }
} // namespace

int luciole::runThumbnailWorker(std::span<const std::string_view> arguments) {
  if (arguments.size() != 4) {
    return 2;
  }
  const auto number = [](std::string_view input) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() ? std::optional(value) : std::nullopt;
  };
  const auto side = number(arguments[0]), maximum = number(arguments[1]);
  if (!side || *side < 1 || *side > kMaxPreviewImageSide || !maximum || *maximum > 1024U * 1024U * 1024U) {
    return 2;
  }
  struct stat info{};
  if (::fstat(3, &info) < 0
      || !S_ISREG(info.st_mode)
      || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > *maximum) {
    return 2;
  }
  try {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = ::read(3, bytes.data() + offset, bytes.size() - offset);
      if (count <= 0) {
        return 1;
      }
      offset += static_cast<std::size_t>(count);
    }
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    // No source pixel policy: a decode whose RGBA frame cannot fit the helper memory limit fails there anyway.
    auto image = arguments[2] == "image/svg+xml" || arguments[2] == "image/svg+xml-compressed"
        ? rasterizeSvg(
              bytes, static_cast<int>(*side), false, static_cast<std::uint64_t>(kMaxSvgSourceSide * kMaxSvgSourceSide)
          )
        : decodeRasterImage(bytes, static_cast<int>(*side), kProviderMemoryBytes / 4, cancel.get(), arguments[2]);
    if (!image) {
      // Keep backend diagnostics bounded and on one line; never send the source filename.
      auto message = image.error().substr(0, 768);
      std::ranges::replace_if(message, [](unsigned char c) { return c < 32 || c > 126; }, '?');
      constexpr std::array<std::uint8_t, 4> header{'N', 'F', 'E', '1'};
      (void)writeResult(arguments[3], header, {reinterpret_cast<const std::uint8_t*>(message.data()), message.size()});
      return 1;
    }
    // Private parent/child transport avoids PNG compression followed immediately by decoding.
    std::array<std::uint8_t, 12> header{'N', 'F', 'R', '1'};
    for (int i = 0; i < 4; ++i) {
      header[4 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image->width) >> (24 - i * 8));
      header[8 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image->height) >> (24 - i * 8));
    }
    return writeResult(arguments[3], header, image->rgba) ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}

std::expected<LoadedImageFile, std::string>
luciole::readThumbnailWorkerResult(std::vector<std::uint8_t> bytes, int maxSide) {
  if (bytes.size() > 4
      && bytes.size() <= 1024
      && bytes[0] == 'N'
      && bytes[1] == 'F'
      && bytes[2] == 'E'
      && bytes[3] == '1'
      && std::all_of(bytes.begin() + 4, bytes.end(), [](auto c) { return c >= 32 && c <= 126; })) {
    return std::unexpected(std::string(bytes.begin() + 4, bytes.end()));
  }
  if (bytes.size() < 12
      || maxSide < 1
      || maxSide > kMaxPreviewImageSide
      || bytes[0] != 'N'
      || bytes[1] != 'F'
      || bytes[2] != 'R'
      || bytes[3] != '1') {
    return std::unexpected("invalid thumbnail worker result");
  }
  const auto dimension = [&](std::size_t offset) {
    std::uint32_t result = 0;
    for (std::size_t i = offset; i < offset + 4; ++i) {
      result = (result << 8) | bytes[i];
    }
    return result;
  };
  const auto width = dimension(4), height = dimension(8);
  if (!width
      || !height
      || width > static_cast<unsigned>(maxSide)
      || height > static_cast<unsigned>(maxSide)
      || bytes.size() != 12 + static_cast<std::size_t>(width) * height * 4) {
    return std::unexpected("oversized or truncated thumbnail worker result");
  }
  bytes.erase(bytes.begin(), bytes.begin() + 12);
  return LoadedImageFile{
      .rgba = std::move(bytes), .width = static_cast<int>(width), .height = static_cast<int>(height)
  };
}
