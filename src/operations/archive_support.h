#pragma once

#include "fs/posix_fd.h"
#include "operations/archive_work.h"

#include <chrono>
#include <clocale>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

struct archive;

// Helpers shared by the archive reader and writer.
namespace luciole::archive_work {
  using Clock = std::chrono::steady_clock;
  class ArchiveLocale {
  public:
    ArchiveLocale() {
      m_locale = ::newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
      if (!m_locale) {
        throwFileError(FileErrorKind::Unavailable, "A UTF-8 archive locale is unavailable.");
      }
      m_previous = ::uselocale(m_locale);
    }
    ~ArchiveLocale() {
      ::uselocale(m_previous);
      ::freelocale(m_locale);
    }
    ArchiveLocale(const ArchiveLocale&) = delete;
    ArchiveLocale& operator=(const ArchiveLocale&) = delete;

  private:
    locale_t m_locale{}, m_previous{};
  };
  struct Work {
    ArchiveLimits limits;
    CancelToken cancel;
    luciole::archive_work::Progress& progress;
    Clock::time_point deadline;
    std::exception_ptr failure;
    std::uint64_t expanded = 0;
    std::size_t count = 0;
    void check() const {
      if (cancel.cancelled()) {
        throwFileError(FileErrorKind::Cancelled, "Archive operation cancelled.");
      }
      if (deadline != Clock::time_point::max() && Clock::now() > deadline) {
        throwFileError(FileErrorKind::Other, "Archive operation exceeded its time limit.");
      }
    }
    void item() {
      check();
      if (count >= limits.maxEntries) {
        throwFileError(FileErrorKind::Unsupported, "Archive has too many entries.");
      }
      progress.items.store(++count);
    }
    void account(std::uint64_t bytes) {
      if (bytes > limits.maxExpandedBytes - expanded) {
        throwFileError(FileErrorKind::Unsupported, "Archive exceeds the expanded-size limit.");
      }
      expanded += bytes;
    }
  };
  struct Stage {
    std::string path;
    Stage() {
      std::error_code ignored;
      auto pattern = (std::filesystem::temp_directory_path(ignored) / "luciole-archive-XXXXXX").native();
      if (ignored || !::mkdtemp(pattern.data())) {
        throwFileError(FileErrorKind::Other, "Could not create private archive staging storage.");
      }
      path = std::move(pattern);
    }
    ~Stage() {
      if (!path.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    }
    std::string release() { return std::exchange(path, {}); }
  };
  [[noreturn]] void systemError(const char* operation);
  Fd opened(int fd);
  struct stat statAt(int fd, const std::string& name);
  void requireSpace(int folder, std::uint64_t bytes);
  std::string stamp(const struct stat& info);
  std::uint64_t decodedContainerBudget(const ArchiveLimits& limits);
  Work workFor(ArchiveLimits limits, const CancelToken& cancel, Progress& progress);
  std::string bounded(const char* text);
  std::vector<std::string> parts(const std::string& name);
  std::string join(const std::vector<std::string>& components);
  std::string safePath(const std::string& name, const Work& work);
  std::string linkTarget(const std::string& path, const std::string& target, const Work& work);
  void validateLinks(const std::map<std::string, std::string>& links, Work& work);
  std::string leaf(const std::string& path);
  std::string parentName(const std::string& path);
  std::string native(const Location& location);
  std::pair<std::string, std::string> destinationParts(const Location& destination);
  [[noreturn]] void archiveError(struct archive* value, Work& work);
  void requireArchive(int status, struct archive* value, Work& work);
} // namespace luciole::archive_work
