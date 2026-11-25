#include "operations/archive_xz_input.h"

#include <cstdio>
#include <limits>

namespace {
  [[noreturn]] void decodeError(lzma_ret status) {
    using namespace luciole;
    if (status == LZMA_MEMLIMIT_ERROR) {
      throwFileError(FileErrorKind::Unsupported, "XZ archive exceeds the decoder memory limit.");
    }
    if (status == LZMA_OPTIONS_ERROR || status == LZMA_UNSUPPORTED_CHECK) {
      throwFileError(FileErrorKind::Unsupported, "XZ archive uses unsupported compression or integrity checks.");
    }
    if (status == LZMA_MEM_ERROR) {
      throwFileError(FileErrorKind::Other, "Could not allocate memory for the XZ decoder.");
    }
    throwFileError(FileErrorKind::InvalidArgument, "XZ archive is corrupt or truncated.");
  }
} // namespace

bool luciole::archive_work::XzInput::recognizes(Input& input) {
  if (input.split()) {
    return false;
  }
  std::array<char, 6> signature{};
  const auto count = input.read(signature);
  input.seek(0, SEEK_SET);
  constexpr std::array<char, 6> magic{static_cast<char>(0xFD), '7', 'z', 'X', 'Z', 0};
  return count == signature.size() && signature == magic;
}
luciole::archive_work::XzInput::XzInput(Input& input, Work& work)
    : m_input(input), m_work(work), m_budget(decodedContainerBudget(work.limits)) {
  const auto status = lzma_stream_decoder(
      &m_stream, std::numeric_limits<std::uint64_t>::max(), LZMA_CONCATENATED | LZMA_TELL_UNSUPPORTED_CHECK
  );
  if (status != LZMA_OK) {
    lzma_end(&m_stream);
    decodeError(status);
  }
}
luciole::archive_work::XzInput::~XzInput() { lzma_end(&m_stream); }
std::size_t luciole::archive_work::XzInput::read(std::span<char> buffer) {
  m_work.check();
  if (m_finished || buffer.empty()) {
    return 0;
  }
  m_stream.next_out = reinterpret_cast<std::uint8_t*>(buffer.data());
  m_stream.avail_out = buffer.size();
  while (m_stream.avail_out != 0 && !m_finished) {
    m_work.check();
    if (m_stream.avail_in == 0 && !m_eof) {
      m_stream.avail_in = m_input.read(m_encoded);
      m_stream.next_in = reinterpret_cast<const std::uint8_t*>(m_encoded.data());
      m_eof = m_stream.avail_in == 0;
    }
    const auto before = m_stream.avail_out;
    const auto status = lzma_code(&m_stream, m_eof ? LZMA_FINISH : LZMA_RUN);
    const auto produced = before - m_stream.avail_out;
    if (produced > m_budget - m_decoded) {
      throwFileError(FileErrorKind::Unsupported, "Compressed archive exceeds its decoded-container limit.");
    }
    m_decoded += produced;
    if (status == LZMA_STREAM_END) {
      m_finished = true;
    } else if (status != LZMA_OK) {
      decodeError(status);
    }
  }
  const auto produced = buffer.size() - m_stream.avail_out;
  if (m_finished) {
    lzma_end(&m_stream);
  }
  return produced;
}
void luciole::archive_work::XzInput::finish() {
  // TAR stops at its end markers; drain the remaining XZ frames, checks and
  // padding before accepting a catalog or publishing extracted data.
  std::array<char, 65536> discard{};
  while (read(discard) != 0) {
  }
}
