#include "thumbnails/thumbnail_provider.h"

#include "core/log.h"
#include "fs/gio_ptr.h"
#include "fs/posix_fd.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_codec.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <unistd.h>

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
  std::optional<double> videoDuration(int source, const std::filesystem::path& directory, GCancellable* cancel) {
    const auto path = directory / "duration";
    const luciole::Fd output(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!output.valid()
        || !luciole::runProviderProcess(
            {"ffprobe", "-v", "error", "-protocol_whitelist", "file,pipe", "-show_entries", "format=duration", "-of",
             "default=noprint_wrappers=1:nokey=1", "/proc/self/fd/3"},
            source, cancel, 4096, 1, output.get(), std::chrono::seconds(2)
        )) {
      return std::nullopt;
    }
    const auto bytes = luciole::readThumbnailFile(path, 4096);
    if (!bytes || bytes->empty()) {
      return std::nullopt;
    }
    const auto* begin = reinterpret_cast<const char*>(bytes->data());
    double duration = 0;
    const auto parsed = std::from_chars(begin, begin + bytes->size(), duration);
    return parsed.ec == std::errc{} && std::isfinite(duration) && duration > 0 ? std::optional(duration) : std::nullopt;
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
    const auto duration = videoDuration(source, output.parent_path(), cancel);
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

void luciole::RemoveThumbnailDirectory::operator()(char* path) const {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  g_free(path);
}

luciole::ThumbnailDirectory luciole::makeThumbnailDirectory() {
  return ThumbnailDirectory(g_dir_make_tmp("luciole-thumbnail-XXXXXX", nullptr));
}

std::expected<LoadedImageFile, std::string> luciole::renderThumbnailProvider(
    int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable,
    ThumbnailPurpose purpose, const std::filesystem::path& workspace
) {
  const bool image = mime.starts_with("image/");
  if (side < 1 || side > (image ? kMaxPreviewImageSide : 1024)) {
    return std::unexpected("invalid preview size");
  }
  const std::size_t outputLimit = image
      ? std::max(std::size_t{8U * 1024U * 1024U}, 12 + static_cast<std::size_t>(side) * side * 4)
      : 8U * 1024U * 1024U;
  const auto temporary = workspace.empty() ? makeThumbnailDirectory() : ThumbnailDirectory{};
  if (workspace.empty() && !temporary) {
    return std::unexpected("cannot prepare thumbnail provider");
  }
  const std::filesystem::path directory = temporary ? std::filesystem::path(temporary.get()) : workspace;
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
