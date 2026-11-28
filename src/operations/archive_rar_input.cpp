#include "operations/archive_rar_input.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {
  using namespace luciole;
  [[noreturn]] void invalid() {
    throwFileError(FileErrorKind::InvalidArgument, "RAR archive has an invalid or truncated header.");
  }
  [[noreturn]] void multipart() {
    throwFileError(FileErrorKind::Unsupported, "Multipart RAR archives are not supported yet.");
  }
  unsigned little16(std::span<const unsigned char> bytes, std::size_t position) {
    return bytes[position] | (static_cast<unsigned>(bytes[position + 1]) << 8);
  }
  std::uint64_t varint(std::span<const unsigned char> bytes, std::size_t& position) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (position >= bytes.size()) {
        invalid();
      }
      const auto byte = bytes[position++];
      if (shift == 63 && (byte & 0xFEU)) {
        invalid();
      }
      value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
      if (!(byte & 0x80U)) {
        return value;
      }
    }
    invalid();
  }
  // Returns true for an encryption header (headers encrypted with a password).
  bool validateRar5(std::span<const unsigned char> bytes) {
    std::size_t position = 12; // Signature and header CRC32; libarchive verifies CRCs.
    const auto size = varint(bytes, position);
    if (!size || size > 2U * 1024U * 1024U) {
      invalid();
    }
    const auto end = position + static_cast<std::size_t>(size);
    bytes = bytes.first(std::min(end, bytes.size()));
    const auto type = varint(bytes, position);
    if (type == 4) {
      return true;
    }
    if (type != 1) {
      invalid();
    }
    const auto flags = varint(bytes, position);
    const auto extra = (flags & 1U) ? varint(bytes, position) : 0;
    const auto data = (flags & 2U) ? varint(bytes, position) : 0;
    const auto archiveFlags = varint(bytes, position);
    if (position > end || extra > end - position || data) {
      invalid();
    }
    if (archiveFlags & 3U) {
      multipart();
    }
    return false;
  }
} // namespace

luciole::archive_work::RarInput luciole::archive_work::validateRarInput(Input& input) {
  if (input.split()) {
    return RarInput::None;
  }
  std::array<unsigned char, 96> prefix{};
  auto count = input.read(std::span(reinterpret_cast<char*>(prefix.data()), 8));
  constexpr std::array<unsigned char, 6> magic{'R', 'a', 'r', '!', 0x1A, 0x07};
  if (count < 7 || !std::equal(magic.begin(), magic.end(), prefix.begin())) {
    input.seek(0, SEEK_SET);
    return RarInput::None;
  }
  count += input.read(std::span(reinterpret_cast<char*>(prefix.data() + count), prefix.size() - count));
  input.seek(0, SEEK_SET);
  const std::span<const unsigned char> bytes(prefix.data(), count);
  if (prefix[6] == 0) {
    if (count < 20 || prefix[9] != 0x73 || little16(bytes, 12) < 13) {
      invalid();
    }
    const auto flags = little16(bytes, 10);
    if (flags & 1U) {
      multipart();
    }
    return (flags & 0x80U) ? RarInput::Rar4Encrypted : RarInput::Rar4;
  }
  if (prefix[6] == 1 && count >= 8 && prefix[7] == 0) {
    return validateRar5(bytes) ? RarInput::Rar5Encrypted : RarInput::Rar5;
  }
  invalid();
}

namespace {
  std::uint32_t crc32(std::span<const unsigned char> bytes, std::uint32_t initial = 0) {
    static constexpr auto table = [] {
      std::array<std::uint32_t, 256> values{};
      for (std::uint32_t i = 0; i < values.size(); ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit) {
          value = (value >> 1) ^ ((value & 1U) ? 0xEDB88320U : 0U);
        }
        values[i] = value;
      }
      return values;
    }();
    std::uint32_t value = initial ^ 0xFFFFFFFFU;
    for (const auto byte : bytes) {
      value = (value >> 8) ^ table[(value ^ byte) & 255U];
    }
    return value ^ 0xFFFFFFFFU;
  }
  std::vector<unsigned char> rar5Header(luciole::archive_work::Input& input, std::size_t budget) {
    std::array<unsigned char, 14> prefix{};
    if (input.read(std::span(reinterpret_cast<char*>(prefix.data()), 4)) != 4) {
      invalid();
    }
    std::size_t count = 4;
    while (count < prefix.size()) {
      if (input.read(std::span(reinterpret_cast<char*>(prefix.data() + count), 1)) != 1) {
        invalid();
      }
      if (!(prefix[count++] & 0x80U)) {
        break;
      }
    }
    std::size_t position = 4;
    const auto size = varint(std::span(prefix).first(count), position);
    if (!size || size > 2U * 1024U * 1024U) {
      invalid();
    }
    const auto total = count + static_cast<std::size_t>(size);
    if (total > budget) {
      throwFileError(FileErrorKind::Unsupported, "RAR trailing headers exceed the metadata limit.");
    }
    std::vector<unsigned char> header(total);
    std::copy_n(prefix.begin(), count, header.begin());
    if (input.read(std::span(reinterpret_cast<char*>(header.data() + count), header.size() - count)) != size) {
      invalid();
    }
    std::uint32_t expected = 0;
    for (unsigned i = 0; i < 4; ++i) {
      expected |= static_cast<std::uint32_t>(header[i]) << (i * 8);
    }
    if (crc32(std::span(header).subspan(4)) != expected) {
      throwFileError(FileErrorKind::InvalidArgument, "RAR end-header checksum is invalid.");
    }
    return header;
  }
} // namespace

void luciole::archive_work::validateRar5End(Input& input, std::int64_t offset, Work& work) {
  input.seek(offset, SEEK_SET);
  std::size_t metadata = 0;
  for (std::size_t count = 0; count < work.limits.maxEntries; ++count) {
    work.check();
    const auto header = rar5Header(input, work.limits.maxMetadataBytes - metadata);
    metadata += header.size();
    std::size_t position = 4;
    (void)varint(header, position);
    const auto type = varint(header, position);
    const auto flags = varint(header, position);
    const auto extra = (flags & 1U) ? varint(header, position) : 0;
    const auto data = (flags & 2U) ? varint(header, position) : 0;
    if (extra > header.size() - position || data > static_cast<std::uint64_t>(INT64_MAX)) {
      invalid();
    }
    if (type == 5) {
      const auto endFlags = varint(header, position);
      if (endFlags & 1U) {
        multipart();
      }
      if (data || position > header.size() - extra) {
        invalid();
      }
      return;
    }
    if (type != 3 && !(flags & 4U)) {
      invalid();
    }
    input.seek(static_cast<std::int64_t>(data), SEEK_CUR);
  }
  throwFileError(FileErrorKind::Unsupported, "RAR has too many trailing headers.");
}

void luciole::archive_work::verifyRar4Stored(Input& input, std::int64_t offset, Work& work) {
  const auto saved = input.seek(0, SEEK_CUR);
  input.seek(offset, SEEK_SET);
  std::size_t metadata = 0;
  for (;;) {
    work.check();
    const auto start = input.seek(0, SEEK_CUR);
    std::array<unsigned char, 40> bytes{};
    const auto count = input.read(std::span(reinterpret_cast<char*>(bytes.data()), bytes.size()));
    if (count < 7) {
      invalid();
    }
    const auto flags = little16(bytes, 3);
    const auto size = little16(bytes, 5);
    if (size < 7 || size > work.limits.maxMetadataBytes - metadata) {
      throwFileError(FileErrorKind::Unsupported, "RAR member headers exceed the metadata limit.");
    }
    metadata += size;
    const auto little32 = [&](std::size_t position) {
      if (position + 4 > count) {
        invalid();
      }
      std::uint32_t value = 0;
      for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[position + i]) << (i * 8);
      }
      return value;
    };
    std::uint64_t packed = (flags & 0x8000U) ? little32(7) : 0;
    if (bytes[2] == 0x74) {
      if (count < 32 || size < 32) {
        invalid();
      }
      if (flags & 0x100U) {
        if (size < 40) {
          invalid();
        }
        packed |= static_cast<std::uint64_t>(little32(32)) << 32;
      }
      if (bytes[25] == 0x30) {
        input.seek(start, SEEK_SET);
        input.seek(size, SEEK_CUR);
        std::array<unsigned char, 65536> buffer{};
        std::uint32_t checksum = 0;
        auto remaining = packed;
        while (remaining) {
          work.check();
          const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
          if (input.read(std::span(reinterpret_cast<char*>(buffer.data()), wanted)) != wanted) {
            invalid();
          }
          checksum = crc32(std::span(buffer).first(wanted), checksum);
          remaining -= wanted;
        }
        if (checksum != little32(16)) {
          throwFileError(FileErrorKind::InvalidArgument, "RAR member checksum is invalid.");
        }
      }
      input.seek(saved, SEEK_SET);
      return;
    }
    input.seek(start, SEEK_SET);
    input.seek(size, SEEK_CUR);
    input.seek(static_cast<std::int64_t>(packed), SEEK_CUR);
  }
}
