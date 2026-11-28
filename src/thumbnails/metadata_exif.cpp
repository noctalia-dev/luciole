#include "thumbnails/metadata_probe.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <unistd.h>
#include <vector>

namespace {
  // EXIF lives in the first APP1 segment; photographs with huge embedded thumbnails still answer within this bound.
  constexpr std::size_t kMaximumExifBytes = 128U * 1024U;
  constexpr std::size_t kMaximumIfdEntries = 128;

  std::uint16_t host16(const std::vector<std::uint8_t>& data, std::size_t at, bool little) {
    if (at + 2 > data.size()) {
      return 0;
    }
    const std::uint16_t high = little ? data[at + 1] : data[at];
    const std::uint16_t low = little ? data[at] : data[at + 1];
    return static_cast<std::uint16_t>((high << 8) | low);
  }

  std::uint32_t host32(const std::vector<std::uint8_t>& data, std::size_t at, bool little) {
    if (at + 4 > data.size()) {
      return 0;
    }
    const std::uint32_t a = little ? data[at + 3] : data[at];
    const std::uint32_t b = little ? data[at + 2] : data[at + 1];
    const std::uint32_t c = little ? data[at + 1] : data[at + 2];
    const std::uint32_t d = little ? data[at] : data[at + 3];
    return (a << 24) | (b << 16) | (c << 8) | d;
  }

  std::string_view tiffText(const std::vector<std::uint8_t>& data, std::uint32_t offset, std::uint32_t count) {
    if (offset > data.size() || count > data.size() - offset) {
      return {};
    }
    return {reinterpret_cast<const char*>(data.data() + offset), count};
  }

  // Walks one image file directory; `visit` receives each entry's tag, type, count and entry offset. Two levels of
  // indirection at most: the caller walks IFD0 and the EXIF sub-IFD it points at.
  void walkIfd(
      const std::vector<std::uint8_t>& tiff, bool little, std::uint32_t offset,
      const std::function<void(std::uint16_t, std::uint16_t, std::uint32_t, std::size_t)>& visit
  ) {
    if (offset + 2 > tiff.size()) {
      return;
    }
    const auto count = host16(tiff, offset, little);
    if (count > kMaximumIfdEntries || offset + 2 + count * 12 > tiff.size()) {
      return;
    }
    for (std::size_t index = 0; index < count; ++index) {
      const auto entry = offset + 2 + index * 12;
      visit(host16(tiff, entry, little), host16(tiff, entry + 2, little), host32(tiff, entry + 4, little), entry);
    }
  }
} // namespace

luciole::ExifFacts luciole::readExifFacts(int fd) {
  std::vector<std::uint8_t> prefix(kMaximumExifBytes);
  std::size_t read = 0;
  while (read < prefix.size()) {
    const auto count = ::pread(fd, prefix.data() + read, prefix.size() - read, static_cast<off_t>(read));
    if (count <= 0) {
      break;
    }
    read += static_cast<std::size_t>(count);
  }
  prefix.resize(read);
  if (prefix.size() < 12 || prefix[0] != 0xFF || prefix[1] != 0xD8) {
    return {};
  }
  std::size_t tiff = 0;
  bool little = true;
  for (std::size_t at = 2; at + 4 <= prefix.size();) {
    if (prefix[at] != 0xFF) {
      return {};
    }
    const auto marker = prefix[at + 1];
    if (marker == 0xFF) {
      ++at;
      continue;
    }
    if (marker == 0xD9 || marker == 0xDA) {
      return {}; // The image data begins; EXIF must precede it.
    }
    const std::size_t length = (static_cast<std::size_t>(prefix[at + 2]) << 8) | prefix[at + 3];
    if (marker == 0xE1
        && at + 10 <= prefix.size()
        && prefix[at + 4] == 0x45
        && prefix[at + 5] == 0x78
        && prefix[at + 6] == 0x69
        && prefix[at + 7] == 0x66
        && prefix[at + 8] == 0x00
        && prefix[at + 9] == 0x00) {
      tiff = at + 10;
      break;
    }
    if (length < 2) {
      return {};
    }
    at += 2 + length;
  }
  if (tiff == 0 || tiff + 8 > prefix.size()) {
    return {};
  }
  const std::vector<std::uint8_t> body(prefix.begin() + static_cast<std::ptrdiff_t>(tiff), prefix.end());
  if (body[0] == 0x49 && body[1] == 0x49) {
    little = true;
  } else if (body[0] == 0x4D && body[1] == 0x4D) {
    little = false;
  } else {
    return {};
  }
  if (host16(body, 2, little) != 42) {
    return {};
  }
  ExifFacts facts;
  std::optional<std::uint32_t> exifIfd;
  walkIfd(
      body, little, host32(body, 4, little),
      [&](std::uint16_t tag, std::uint16_t type, std::uint32_t, std::size_t entry) {
        if (tag == 0x0112 && type == 3) {
          // A SHORT value sits inline in the value field, left-justified for big-endian files.
          const auto orientation = host16(body, entry + 8, little);
          if (orientation >= 1 && orientation <= 8) {
            facts.orientation = static_cast<std::uint8_t>(orientation);
          }
        } else if (tag == 0x8769 && type == 4 && !exifIfd) {
          exifIfd = host32(body, entry + 8, little);
        }
      }
  );
  if (exifIfd) {
    walkIfd(body, little, *exifIfd, [&](std::uint16_t tag, std::uint16_t type, std::uint32_t count, std::size_t entry) {
      if (tag != 0x9003 || type != 2 || facts.dateSeconds) {
        return;
      }
      const auto text = tiffText(body, host32(body, entry + 8, little), count);
      if (text.size() < 19) {
        return;
      }
      const auto number = [&](std::size_t at, std::size_t length) {
        std::uint32_t parsed = 0;
        const auto [end, error] = std::from_chars(text.data() + at, text.data() + at + length, parsed);
        return error == std::errc{} ? std::optional(parsed) : std::nullopt;
      };
      const auto year = number(0, 4), month = number(5, 2), day = number(8, 2);
      const auto hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
      if (!year
          || !month
          || !day
          || !hour
          || !minute
          || !second
          || *year < 1000
          || *year > 3000
          || *month < 1
          || *month > 12
          || *day < 1
          || *day > 31
          || *hour > 23
          || *minute > 59
          || *second > 60) {
        return;
      }
      std::tm fields{};
      fields.tm_year = static_cast<int>(*year) - 1900;
      fields.tm_mon = static_cast<int>(*month) - 1;
      fields.tm_mday = static_cast<int>(*day);
      fields.tm_hour = static_cast<int>(*hour);
      fields.tm_min = static_cast<int>(*minute);
      fields.tm_sec = static_cast<int>(*second);
      // EXIF carries no zone, so the local DST rule resolves the offset; forcing standard time shifts DST dates an
      // hour.
      fields.tm_isdst = -1;
      const auto seconds = ::mktime(&fields);
      if (seconds >= 0) {
        facts.dateSeconds = static_cast<std::uint64_t>(seconds);
      }
    });
  }
  return facts;
}
