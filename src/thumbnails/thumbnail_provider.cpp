#include "thumbnails/thumbnail_provider.h"

#include "core/log.h"
#include "fs/gio_ptr.h"
#include "fs/posix_fd.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_codec.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {
  LoadedImageFile videoFilmstrip(
      LoadedImageFile frame, int side, GCancellable* cancel, const std::filesystem::path& directory,
      std::chrono::steady_clock::time_point deadline
  ) {
    const luciole::GString helper(g_find_program_in_path("ffmpegthumbnailer"));
    if (!helper || g_cancellable_is_cancelled(cancel)) {
      return frame;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return frame;
    }
    // Only pass our bounded, re-encoded pixels to the optional helper, never the original media.
    // This retains the source decoder's pixel/protocol limits on older thumbnailer releases too.
    const auto bytes = luciole::writeThumbnailPng({.image = frame, .text = {}});
    const auto inputPath = directory / "filmstrip-input.png";
    const luciole::Fd input(::open(inputPath.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!bytes || !input.valid()) {
      return frame;
    }
    std::size_t offset = 0;
    while (offset < bytes->size()) {
      const auto count = ::write(input.get(), bytes->data() + offset, bytes->size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return frame;
      }
      offset += static_cast<std::size_t>(count);
    }
    if (::lseek(input.get(), 0, SEEK_SET) < 0) {
      return frame;
    }
    const auto remaining = std::min(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
        std::chrono::milliseconds(2000)
    );
    if (remaining <= std::chrono::milliseconds::zero()) {
      return frame;
    }
    const auto output = directory / "filmstrip.png";
    const auto result = luciole::runProviderProcess(
        {helper.get(), "-i", "/proc/self/fd/3", "-o", output.native(), "-s", std::to_string(side), "-t", "0", "-f",
         "-c", "png"},
        input.get(), cancel, 8U * 1024U * 1024U, 1, -1, remaining
    );
    if (result) {
      if (const auto encoded = luciole::readThumbnailFile(output, 8U * 1024U * 1024U)) {
        if (auto decorated = luciole::readThumbnailPng(*encoded, side);
            decorated && decorated->image.width == frame.width && decorated->image.height == frame.height) {
          return std::move(decorated->image);
        }
      }
    }
    logDebug("[thumbnail] filmstrip unavailable; keeping selected video frame");
    return frame;
  }
  struct VideoProbe {
    std::optional<double> duration;
    std::optional<int> cover; // stream index of the best embedded cover
  };
  template <typename T> std::optional<T> parseNumber(std::string_view text) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} ? std::optional(value) : std::nullopt;
  }
  VideoProbe probeVideo(int source, const std::filesystem::path& directory, GCancellable* cancel) {
    constexpr std::size_t kLimit = 16U * 1024U;
    const auto path = directory / "probe";
    const luciole::Fd output(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!output.valid()
        || !luciole::runProviderProcess(
            {"ffprobe", "-v", "error", "-protocol_whitelist", "file,pipe", "-select_streams", "v", "-show_entries",
             "format=duration:stream=index,width,height:stream_disposition=attached_pic:stream_tags=filename", "-of",
             "compact=p=0", "/proc/self/fd/3"},
            source, cancel, kLimit, 1, output.get(), std::chrono::seconds(2)
        )) {
      return {};
    }
    const auto bytes = luciole::readThumbnailFile(path, kLimit);
    if (!bytes) {
      return {};
    }
    // Rank covers: Matroska cover names first (landscape, then large), then the most pixels.
    constexpr std::array<std::string_view, 4> kCoverNames{"small_cover", "cover", "small_cover_land", "cover_land"};
    VideoProbe probe;
    std::pair<std::size_t, std::int64_t> best{0, -1};
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    for (const auto lineRange : std::views::split(text, '\n')) {
      const std::string_view line(lineRange.begin(), lineRange.end());
      if (line.starts_with("duration=")) {
        const auto duration = parseNumber<double>(line.substr(9));
        if (duration && std::isfinite(*duration) && *duration > 0) {
          probe.duration = duration;
        }
        continue;
      }
      std::optional<int> index;
      std::int64_t width = 0;
      std::int64_t height = 0;
      bool attached = false;
      std::size_t nameRank = 0;
      for (const auto fieldRange : std::views::split(line, '|')) {
        const std::string_view field(fieldRange.begin(), fieldRange.end());
        const auto equals = field.find('=');
        const auto key = field.substr(0, equals);
        const auto value = equals == std::string_view::npos ? std::string_view{} : field.substr(equals + 1);
        if (key == "index") {
          index = parseNumber<int>(value);
        } else if (key == "width") {
          width = parseNumber<std::int64_t>(value).value_or(0);
        } else if (key == "height") {
          height = parseNumber<std::int64_t>(value).value_or(0);
        } else if (key == "disposition:attached_pic") {
          attached = value == "1";
        } else if (key == "tag:filename") {
          const auto* const found = std::ranges::find(kCoverNames, value.substr(0, value.find('.')));
          nameRank = found == kCoverNames.end() ? 0 : static_cast<std::size_t>(found - kCoverNames.begin()) + 1;
        }
      }
      const std::pair rank{nameRank, width * height};
      if (attached && index && rank > best) {
        best = rank;
        probe.cover = index;
      }
    }
    return probe;
  }
  bool darkFrame(const LoadedImageFile& image) {
    std::size_t dark = 0;
    const auto pixels = image.rgba.size() / 4;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
      dark += image.rgba[i] < 12 && image.rgba[i + 1] < 12 && image.rgba[i + 2] < 12;
    }
    return pixels != 0 && dark * 100 >= pixels * 98;
  }
  std::expected<LoadedImageFile, std::string> videoThumbnail(
      int source, int side, GCancellable* cancel, const std::filesystem::path& output, luciole::ThumbnailPurpose purpose
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    const auto probe = probeVideo(source, output.parent_path(), cancel);
    // An embedded cover (attached picture, e.g. yt-dlp --embed-thumbnail) is the file's intended preview, so prefer
    // it to decoded frames. Files without a usable cover fall through to the frame loop.
    if (probe.cover) {
      std::vector<std::string> arguments{"ffmpeg",   "-nostdin",
                                         "-v",       "error",
                                         "-xerror",  "-threads",
                                         "1",        "-filter_threads",
                                         "1",        "-protocol_whitelist",
                                         "file,pipe"};
      arguments.insert(
          arguments.end(),
          {"-i", "/proc/self/fd/3", "-map", "0:" + std::to_string(*probe.cover), "-frames:v", "1", "-an", "-sn",
           "-threads", "1", "-vf",
           "scale=" + std::to_string(side) + ':' + std::to_string(side) + ":force_original_aspect_ratio=decrease", "-y",
           output.native()}
      );
      (void)::unlink(output.c_str());
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      const auto result =
          luciole::runProviderProcess(std::move(arguments), source, cancel, 8U * 1024U * 1024U, 0, -1, remaining);
      if (result) {
        const auto bytes = luciole::readThumbnailFile(output, 8U * 1024U * 1024U);
        if (bytes) {
          if (auto cover = luciole::readThumbnailPng(*bytes, side)) {
            // A cover is already the intended artwork: skip the filmstrip border added to decoded frames.
            return std::move(cover->image);
          }
        }
      }
    }
    const auto& duration = probe.duration;
    const std::vector<double> positions =
        duration && *duration >= 1 ? std::vector{*duration * 0.1, *duration / 3, 0.0} : std::vector{0.0};
    std::optional<LoadedImageFile> dark;
    std::string failure = "no video frame available";
    for (const auto position : positions) {
      if (g_cancellable_is_cancelled(cancel)) {
        return std::unexpected("thumbnail request cancelled");
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) {
        break;
      }
      std::vector<std::string> arguments{"ffmpeg",   "-nostdin",
                                         "-v",       "error",
                                         "-xerror",  "-threads",
                                         "1",        "-filter_threads",
                                         "1",        "-protocol_whitelist",
                                         "file,pipe"};
      // Decode only keyframes: an accurate seek decodes every frame since the previous keyframe, which dominates
      // generation time for long GOPs.
      arguments.insert(arguments.end(), {"-skip_frame:v", "nokey"});
      if (position > 0) {
        arguments.insert(arguments.end(), {"-noaccurate_seek", "-ss", std::to_string(position)});
      }
      arguments.insert(
          arguments.end(),
          {"-i", "/proc/self/fd/3", "-map", "0:V:0", "-frames:v", "1", "-an", "-sn", "-threads", "1", "-vf",
           "scale=" + std::to_string(side) + ':' + std::to_string(side) + ":force_original_aspect_ratio=decrease", "-y",
           output.native()}
      );
      (void)::unlink(output.c_str());
      const auto result =
          luciole::runProviderProcess(std::move(arguments), source, cancel, 8U * 1024U * 1024U, 0, -1, remaining);
      if (!result) {
        failure = result.error();
        continue;
      }
      const auto bytes = luciole::readThumbnailFile(output, 8U * 1024U * 1024U);
      auto image = bytes ? luciole::readThumbnailPng(*bytes, side)
                         : std::expected<luciole::ThumbnailPng, std::string>(std::unexpected("no video frame output"));
      if (!image) {
        failure = image.error();
        continue;
      }
      if (!darkFrame(image->image)) {
        return purpose == luciole::ThumbnailPurpose::Thumbnail
            ? videoFilmstrip(std::move(image->image), side, cancel, output.parent_path(), deadline)
            : std::move(image->image);
      }
      if (dark) {
        break;
      }
      dark = std::move(image->image);
    }
    // A genuinely dark video is still a valid preview; do not turn it into a missing thumbnail.
    if (!dark) {
      return std::unexpected(failure);
    }
    return purpose == luciole::ThumbnailPurpose::Thumbnail
        ? videoFilmstrip(std::move(*dark), side, cancel, output.parent_path(), deadline)
        : std::move(*dark);
  }
} // namespace

std::expected<LoadedImageFile, std::string> luciole::renderThumbnailProvider(
    int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable,
    ThumbnailPurpose purpose
) {
  const bool image = mime.starts_with("image/");
  if (side < 1 || side > (image ? kMaxPreviewImageSide : 1024)) {
    return std::unexpected("invalid preview size");
  }
  const std::size_t outputLimit = image
      ? std::max(std::size_t{8U * 1024U * 1024U}, 12 + static_cast<std::size_t>(side) * side * 4)
      : 8U * 1024U * 1024U;
  const GString temporary(g_dir_make_tmp("luciole-thumbnail-XXXXXX", nullptr));
  if (!temporary) {
    return std::unexpected("cannot prepare thumbnail provider");
  }
  const std::filesystem::path directory(temporary.get());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};
  const auto output = directory / (mime.starts_with("image/") ? "result.rgba" : "result.png");
  std::expected<void, std::string> success = std::unexpected("no supported thumbnail provider");
  if (mime.starts_with("image/")) {
    // Glycin transfers source pixels through a memfd before scaling. RLIMIT_FSIZE applies there too, so it allows
    // any frame that fits the helper memory limit. The private result remains separately validated against
    // outputLimit and the requested dimensions.
    const auto fileLimit = std::max(
        {static_cast<std::uint64_t>(outputLimit), static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U,
         kProviderMemoryBytes}
    );
    success = runProviderProcess(
        {"/proc/self/exe", "--thumbnail-worker", std::to_string(side),
         std::to_string(static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U), mime, output.native()},
        sourceFd, cancellable, fileLimit, 2
    );
  } else if (mime == "application/pdf") {
    success = runProviderProcess(
        {"pdftoppm", "-f", "1", "-singlefile", "-scale-to", std::to_string(side), "-png", "-q", "/proc/self/fd/3",
         (directory / "result").native()},
        sourceFd, cancellable
    );
  } else if (mime.starts_with("video/")) {
    return videoThumbnail(sourceFd, side, cancellable, output, purpose);
  } else if (mime.starts_with("audio/")) {
    std::vector<std::string> arguments{"ffmpeg",   "-nostdin",
                                       "-v",       "error",
                                       "-xerror",  "-threads",
                                       "1",        "-filter_threads",
                                       "1",        "-protocol_whitelist",
                                       "file,pipe"};
    arguments.insert(
        arguments.end(),
        {"-i", "/proc/self/fd/3", "-map", "0:v:0", "-frames:v", "1", "-an", "-sn", "-threads", "1", "-vf",
         std::string("thumbnail=4,scale=")
             + std::to_string(side)
             + ':'
             + std::to_string(side)
             + ":force_original_aspect_ratio=decrease",
         "-y", output.native()}
    );
    success = runProviderProcess(std::move(arguments), sourceFd, cancellable);
  }
  if (!success) {
    if (image && !g_cancellable_is_cancelled(cancellable)) {
      auto report = readThumbnailFile(output, 1024);
      if (report
          && report->size() > 4
          && (*report)[0] == 'N'
          && (*report)[1] == 'F'
          && (*report)[2] == 'E'
          && (*report)[3] == '1') {
        return readThumbnailWorkerResult(std::move(*report), side);
      }
    }
    return std::unexpected(success.error());
  }
  auto encoded = readThumbnailFile(output, outputLimit);
  if (!encoded) {
    return std::unexpected("thumbnail provider exceeded output limits");
  }
  if (mime.starts_with("image/")) {
    return readThumbnailWorkerResult(std::move(*encoded), side);
  }
  auto decoded = readThumbnailPng(*encoded, side);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  return std::move(decoded->image);
}
