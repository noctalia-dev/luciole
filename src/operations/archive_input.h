#pragma once

#include "operations/archive_support.h"

#include <span>

namespace luciole::archive_work {
  // One seekable compressed input, optionally assembled from numbered ZIP parts.
  // Descriptors and all discovery/validation stay on the archive worker.
  class Input {
  public:
    Input(const Location& source, Work& work);
    std::size_t read(std::span<char> buffer);
    std::int64_t seek(std::int64_t offset, int whence);
    void unchanged() const;
    Fd duplicateSingle() const;
    const std::string& fingerprint() const { return m_fingerprint; }
    // All captured input paths, including every numbered ZIP part; no further I/O.
    std::vector<std::filesystem::path> sourcePaths() const;
    bool split() const { return !m_prefix.empty(); }

  private:
    struct Part {
      std::string name, original;
      Fd fd;
      std::uint64_t start, size;
    };
    std::vector<std::string> discover() const;
    Work& m_work;
    std::string m_parentPath, m_prefix, m_fingerprint;
    Fd m_parent;
    struct stat m_parentIdentity;
    std::vector<Part> m_parts;
    std::uint64_t m_size = 0, m_position = 0, m_readBytes = 0;
  };
} // namespace luciole::archive_work
