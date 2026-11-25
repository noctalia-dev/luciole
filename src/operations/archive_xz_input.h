#pragma once

#include "operations/archive_input.h"

#include <array>
#include <lzma.h>

namespace luciole::archive_work {
  // Decode outside libarchive to validate concatenated frames, integrity checks and trailing padding.
  // Only TAR consumes this stream. Source identity remains owned by Input.
  class XzInput {
  public:
    static bool recognizes(Input& input);
    XzInput(Input& input, Work& work);
    ~XzInput();
    XzInput(const XzInput&) = delete;
    XzInput& operator=(const XzInput&) = delete;
    std::size_t read(std::span<char> buffer);
    void finish();

  private:
    Input& m_input;
    Work& m_work;
    lzma_stream m_stream = LZMA_STREAM_INIT;
    std::array<char, 65536> m_encoded{};
    std::uint64_t m_decoded = 0, m_budget;
    bool m_eof = false, m_finished = false;
  };
} // namespace luciole::archive_work
