#pragma once

#include "thumbnails/preview_metadata.h"

#include <cstdint>
#include <gio/gio.h>
#include <optional>

namespace luciole {
  // Worker-only source validation and bounded format providers. Missing information is omitted.
  MetadataFields probePreviewMetadata(const DirectoryEntry& entry, ThumbnailOptions options, GCancellable* cancel);
  MetadataFields parseMediaMetadata(std::string_view data);
  MetadataFields parsePdfMetadata(std::string_view data);

  // EXIF facts read from an open image descriptor: the orientation and the original photograph date. EXIF carries
  // no time zone, so the date is local time like the file's other dates.
  struct ExifFacts {
    std::optional<std::uint8_t> orientation;
    std::optional<std::uint64_t> dateSeconds;
  };

  // Reads at most the first 128 KiB; a file without JPEG EXIF answers nothing.
  ExifFacts readExifFacts(int fd);

  EntryMedia parseMediaFacts(std::string_view data);

  // Worker-only list-view column probe; missing information is omitted.
  EntryMedia probeEntryMediaFacts(const std::string& path, const std::string& contentType, GCancellable* cancel);
} // namespace luciole
