#include "thumbnails/network_preview.h"

#include "fs/network_info.h"
#include "thumbnails/thumbnail_provider.h"

#include <array>
#include <sys/mman.h>
#include <unistd.h>

luciole::NetworkMessage luciole::previewRequest(
    std::string_view action, const DirectoryEntry& source, const CancelToken& token, NetworkMessage arguments
) {
  arguments["action"] = action;
  arguments["source"] = source;
  const bool network = ioLane(source.location()) == IoLane::Network;
  const bool decode = action == "thumbnail" && arguments.value("generate", false);
  // The caller cleans up provider output even when a stalled helper is killed.
  const auto directory = decode ? makeThumbnailDirectory() : ThumbnailDirectory{};
  if (decode) {
    if (!directory) {
      throwFileError(FileErrorKind::Unavailable, "Cannot prepare thumbnail provider.");
    }
    arguments["workspace"] = directory.get();
  }
  const auto lane = decode ? (network ? IoLane::NetworkPreviewDecode : IoLane::PreviewDecode)
                           : (network ? IoLane::NetworkPreviewProbe : IoLane::PreviewProbe);
  return NetworkProcess::acquire(lane)->call(std::move(arguments), token);
}
luciole::NetworkMessage luciole::encodePixels(const LoadedImageFile& image) {
  return {
      {"rgba", NetworkMessage::binary(image.rgba)},
      {"width", image.width},
      {"height", image.height},
      {"sourceWidth", image.sourceWidth},
      {"sourceHeight", image.sourceHeight}
  };
}
LoadedImageFile luciole::decodePixels(NetworkMessage value) {
  const auto width = value.at("width").get<int>(), height = value.at("height").get<int>();
  auto& bytes = value.at("rgba").get_binary();
  if (width < 1
      || height < 1
      || width > kMaxPreviewImageSide
      || height > kMaxPreviewImageSide
      || bytes.size() != static_cast<std::size_t>(width) * height * 4U) {
    throwFileError(FileErrorKind::Unavailable, "Invalid preview dimensions.");
  }
  return {
      .rgba = std::move(bytes),
      .width = width,
      .height = height,
      .sourceWidth = value.at("sourceWidth").get<int>(),
      .sourceHeight = value.at("sourceHeight").get<int>()
  };
}

luciole::Fd luciole::stagePreviewSource(int source, std::uint64_t size) {
  constexpr std::uint64_t maximum = 256ULL * 1024 * 1024;
  if (size > maximum) {
    throwFileError(FileErrorKind::Unsupported, "The network file exceeds its preview limit.");
  }
  auto local = checkedFd(::memfd_create("luciole-preview", MFD_CLOEXEC));
  std::array<char, 64U * 1024U> buffer{};
  std::uint64_t offset = 0;
  while (offset < size) {
    const auto count = ::pread(
        source, buffer.data(), std::min<std::uint64_t>(buffer.size(), size - offset), static_cast<off_t>(offset)
    );
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      throwErrno(errno);
    }
    if (count == 0) {
      throwFileError(FileErrorKind::Changed, "The preview source changed while reading.");
    }
    std::size_t written = 0;
    while (written < static_cast<std::size_t>(count)) {
      const auto bytes = ::write(local.get(), buffer.data() + written, static_cast<std::size_t>(count) - written);
      if (bytes < 0 && errno == EINTR) {
        continue;
      }
      if (bytes <= 0) {
        throwErrno(errno);
      }
      written += static_cast<std::size_t>(bytes);
    }
    offset += static_cast<std::uint64_t>(count);
    noteWorkerIoProgress();
  }
  if (::lseek(local.get(), 0, SEEK_SET) < 0) {
    throwErrno(errno);
  }
  return local;
}
