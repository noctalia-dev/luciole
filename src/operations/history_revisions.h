#pragma once

#include "fs/file_error.h"
#include "fs/posix_fd.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

namespace luciole {
  // One native job's confirmed rename/unlink revisions, shared with its retained-item cleanup.
  // Rebase revisions caused by our own operations; subsequent content and metadata checks remain strict.
  class HistoryRevisions {
  public:
    using Key = std::pair<dev_t, ino_t>;
    auto lock() const { return std::unique_lock(m_mutex); }
    void capture(const struct stat& value) {
      const auto guard = lock();
      if (m_known.contains({value.st_dev, value.st_ino})) {
        validate(value, value);
        return;
      }
      if (m_known.size() == 32768) {
        throwFileError(FileErrorKind::Unsupported, "Native history exceeds its inode revision budget.");
      }
      m_known[{value.st_dev, value.st_ino}] = value;
    }
    struct stat expected(const struct stat& original, bool changedFolder = false) const {
      const auto guard = lock();
      auto result = original;
      if (const auto found = m_known.find({original.st_dev, original.st_ino}); found != m_known.end()) {
        if (changedFolder && S_ISDIR(result.st_mode)) {
          result.st_size = found->second.st_size;
          result.st_mtim = found->second.st_mtim;
        }
        result.st_ctim = found->second.st_ctim;
        result.st_nlink = found->second.st_nlink;
        require(result, found->second);
      }
      return result;
    }
    void validate(const struct stat& original, const struct stat& actual) const { require(expected(original), actual); }
    void renamed(const struct stat& original, const struct stat& actual, bool exchanged = false) {
      const auto guard = lock();
      if (!m_known.contains({original.st_dev, original.st_ino})) {
        capture(original);
      }
      if (!sameRenamedRevision(expected(original), actual, exchanged)) {
        throwFileError(
            FileErrorKind::Changed, "The file or its hard-link aliases changed. History is no longer valid."
        );
      }
      m_known[{actual.st_dev, actual.st_ino}] = actual;
    }
    void directoryChanged(const struct stat& original, const struct stat& actual) {
      const auto guard = lock();
      auto result = expected(original, true);
      if (!S_ISDIR(result.st_mode)) {
        throwFileError(FileErrorKind::Changed, "Invalid merged-folder history.");
      }
      result.st_size = actual.st_size;
      result.st_nlink = actual.st_nlink;
      result.st_mtim = actual.st_mtim;
      result.st_ctim = actual.st_ctim;
      require(result, actual);
      m_known[{actual.st_dev, actual.st_ino}] = actual;
    }
    void observed(const struct stat& actual, bool removed) {
      const auto guard = lock();
      if (S_ISDIR(actual.st_mode)) {
        if (removed && actual.st_nlink == 0) {
          m_known.erase({actual.st_dev, actual.st_ino});
        }
        return;
      }
      if (const auto found = m_known.find({actual.st_dev, actual.st_ino}); found != m_known.end()) {
        auto result = found->second;
        if (removed ? result.st_nlink == 0 || actual.st_nlink != result.st_nlink - 1
                    : actual.st_nlink != result.st_nlink) {
          throwFileError(FileErrorKind::Changed, "Hard-link ownership changed during transfer.");
        }
        result.st_ctim = actual.st_ctim;
        result.st_nlink = actual.st_nlink;
        require(result, actual);
        if (actual.st_nlink == 0) {
          m_known.erase(found);
        } else {
          found->second = actual;
        }
      }
    }
    std::uint64_t generation() const {
      const auto guard = lock();
      return m_generation;
    }
    void advance() {
      const auto guard = lock();
      ++m_generation;
    }

  private:
    static void require(const struct stat& expected, const struct stat& actual) {
      if (!sameFileRevision(expected, actual)) {
        throwFileError(
            FileErrorKind::Changed, "The file or its hard-link aliases changed. History is no longer valid."
        );
      }
    }
    mutable std::recursive_mutex m_mutex;
    std::map<Key, struct stat> m_known;
    std::uint64_t m_generation = 0;
  };
} // namespace luciole
