#pragma once

#include "fs/location.h"

#include <string_view>

namespace luciole {
  enum class ArchiveCompression { None, Gzip, Bzip2, Zstd, Xz };
  // Native outer-filter support; this never discovers or starts helper programs.
  bool archiveCompressionAvailable(ArchiveCompression compression);
  // A byte-split ZIP part named <name>.zip.NNN; only part 001 opens the set.
  std::optional<unsigned> splitZipPart(const Location& location);
  // mime is the provider's content type; empty when unknown.
  bool isSupportedArchive(const Location& location, std::string_view mime);
  std::string archiveFolderName(const Location& location);
} // namespace luciole
