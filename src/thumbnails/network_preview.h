#pragma once

#include "fs/network_types.h"
#include "fs/posix_fd.h"
#include "thumbnails/preview_metadata.h"
#include "thumbnails/thumbnail_store.h"

namespace luciole {
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ThumbnailOptions, policy, maxFileMiB, cacheMiB)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MetadataField, label, value, preview)
  NetworkMessage networkIconRequest(const NetworkMessage& request);
  NetworkMessage networkThumbnailRequest(const NetworkMessage& request);
  NetworkMessage networkTextRequest(const NetworkMessage& request);
  NetworkMessage networkMetadataRequest(const NetworkMessage& request);
  NetworkMessage previewRequest(
      std::string_view action, const DirectoryEntry& source, const CancelToken& token,
      NetworkMessage arguments = NetworkMessage::object()
  );
  Fd stagePreviewSource(int source, std::uint64_t size);
  NetworkMessage encodePixels(const LoadedImageFile& image);
  LoadedImageFile decodePixels(NetworkMessage value);
} // namespace luciole
