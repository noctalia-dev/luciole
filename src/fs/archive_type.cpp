#include "fs/archive_type.h"

#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <archive.h>
#include <lzma.h>

namespace {
  bool digits(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
  }
  bool rarVolumeName(std::string_view text) {
    if (const auto part = text.rfind(".rar."); part != std::string_view::npos && digits(text.substr(part + 5))) {
      return true;
    }
    if (text.ends_with(".rar")) {
      const auto stem = text.substr(0, text.size() - 4);
      if (const auto part = stem.rfind(".part"); part != std::string_view::npos && digits(stem.substr(part + 5))) {
        return true;
      }
    }
    const auto dot = text.rfind('.');
    if (dot != std::string_view::npos) {
      const auto suffix = text.substr(dot + 1);
      return suffix.size() == 3 && suffix[0] >= 'r' && suffix[0] <= 'z' && digits(suffix.substr(1));
    }
    return false;
  }
} // namespace

bool luciole::archiveCompressionAvailable(ArchiveCompression compression) {
  switch (compression) {
  case ArchiveCompression::None:
    return true;
  case ArchiveCompression::Gzip:
    return archive_zlib_version() != nullptr;
  case ArchiveCompression::Bzip2:
    return archive_bzlib_version() != nullptr;
  case ArchiveCompression::Zstd:
    return archive_libzstd_version() != nullptr;
  case ArchiveCompression::Xz:
    return lzma_filter_decoder_is_supported(LZMA_FILTER_LZMA2);
  }
  return false;
}

std::optional<unsigned> luciole::splitZipPart(const Location& location) {
  const auto name = location.basename();
  if (!name || name->size() < 8) {
    return std::nullopt;
  }
  const luciole::GString lower(g_ascii_strdown(name->c_str(), -1));
  const std::string_view text(lower.get());
  const auto index = text.substr(text.size() - 3);
  if (!text.substr(0, text.size() - 3).ends_with(".zip.")
      || !std::ranges::all_of(index, [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  return static_cast<unsigned>((index[0] - '0') * 100 + (index[1] - '0') * 10 + index[2] - '0');
}

bool luciole::isSupportedArchive(const Location& location, std::string_view mime) {
  if (!location.nativePath()) {
    return false;
  }
  if (const auto part = splitZipPart(location)) {
    return *part == 1;
  }
  const auto name = location.basename();
  if (!name) {
    return false;
  }
  const luciole::GString lower(g_ascii_strdown(name->c_str(), -1));
  const std::string_view text(lower.get());
  if (const auto part = text.rfind(".7z."); part != std::string_view::npos) {
    const auto index = text.substr(part + 4);
    if (digits(index)) {
      return false;
    }
  }
  if (rarVolumeName(text)) {
    return false;
  }
  if (mime == "application/zip"
      || mime == "application/x-tar"
      || mime == "application/x-7z-compressed"
      || mime == "application/vnd.rar"
      || mime == "application/x-rar"
      || mime == "application/x-rar-compressed") {
    return true;
  }
  if (mime == "application/gzip" || mime == "application/x-compressed-tar") {
    return archiveCompressionAvailable(ArchiveCompression::Gzip);
  }
  if (mime == "application/x-bzip2-compressed-tar" || mime == "application/x-bzip-compressed-tar") {
    return archiveCompressionAvailable(ArchiveCompression::Bzip2);
  }
  if (mime == "application/x-zstd-compressed-tar") {
    return archiveCompressionAvailable(ArchiveCompression::Zstd);
  }
  if (mime == "application/x-xz-compressed-tar") {
    return archiveCompressionAvailable(ArchiveCompression::Xz);
  }
  if (text.ends_with(".zip") || text.ends_with(".tar") || text.ends_with(".7z") || text.ends_with(".rar")) {
    return true;
  }
  if (text.ends_with(".tar.gz") || text.ends_with(".tgz")) {
    return archiveCompressionAvailable(ArchiveCompression::Gzip);
  }
  if (text.ends_with(".tar.bz2") || text.ends_with(".tbz2") || text.ends_with(".tbz")) {
    return archiveCompressionAvailable(ArchiveCompression::Bzip2);
  }
  if (text.ends_with(".tar.xz") || text.ends_with(".txz")) {
    return archiveCompressionAvailable(ArchiveCompression::Xz);
  }
  return (text.ends_with(".tar.zst") || text.ends_with(".tzst"))
      && archiveCompressionAvailable(ArchiveCompression::Zstd);
}
std::string luciole::archiveFolderName(const Location& location) {
  std::string name = location.basename().value_or("Archive");
  const luciole::GString lower(g_ascii_strdown(name.c_str(), -1));
  const std::string_view text(lower.get());
  for (const auto suffix :
       {std::string_view(".zip.001"), std::string_view(".tar.bz2"), std::string_view(".tar.zst"),
        std::string_view(".tar.gz"), std::string_view(".tar.xz"), std::string_view(".txz"), std::string_view(".tbz2"),
        std::string_view(".tzst"), std::string_view(".tbz"), std::string_view(".tgz"), std::string_view(".zip"),
        std::string_view(".tar"), std::string_view(".7z"), std::string_view(".rar")}) {
    if (text.ends_with(suffix) && name.size() >= suffix.size()) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name.empty() || filenameProblem(name) ? "Archive" : name;
}
