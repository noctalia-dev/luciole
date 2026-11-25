#include "operations/archive_support.h"

#include "fs/filename.h"

#include <algorithm>
#include <archive.h>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <format>
#include <limits>
#include <ranges>
#include <sys/statvfs.h>

namespace fs = std::filesystem;

namespace {
  bool asciiAlpha(char value) { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); }
  bool absolute(const std::string& name) {
    return !name.empty()
        && (name.front() == '/' || name.front() == '\\' || (name.size() >= 2 && asciiAlpha(name[0]) && name[1] == ':'));
  }
} // namespace

namespace luciole::archive_work {
  std::uint64_t decodedContainerBudget(const ArchiveLimits& limits) {
    // TAR headers, alignment, PAX metadata and bounded trailing padding.
    std::uint64_t budget = limits.maxExpandedBytes;
    const auto add = [&](std::uint64_t value) {
      budget += std::min(value, std::numeric_limits<std::uint64_t>::max() - budget);
    };
    add(limits.maxMetadataBytes);
    add(limits.maxMetadataBytes);
    for (unsigned i = 0; i < 3; ++i) {
      const auto entries = std::min<std::uint64_t>(limits.maxEntries, std::numeric_limits<std::uint64_t>::max() / 512);
      add(entries * 512);
    }
    add(1024U * 1024U);
    return budget;
  }
  [[noreturn]] void systemError(const char* operation) {
    const int code = errno;
    throwFileError(errnoError(code).kind, std::string(operation) + ": " + std::strerror(code));
  }
  Fd opened(int fd) {
    if (fd < 0) {
      systemError("Could not open archive item");
    }
    return Fd(fd);
  }
  struct stat statAt(int fd, const std::string& name) {
    struct stat value{};
    if (::fstatat(fd, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
      systemError("Could not inspect archive source");
    }
    return value;
  }
  std::string stamp(const struct stat& info) {
    return std::format(
        "{}:{}:{}:{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtim.tv_sec,
        info.st_mtim.tv_nsec, info.st_ctim.tv_sec, info.st_ctim.tv_nsec
    );
  }
  Work workFor(ArchiveLimits limits, const CancelToken& cancel, luciole::archive_work::Progress& progress) {
    auto deadline = Clock::time_point::max();
    if (limits.maxTime != std::chrono::seconds::max()) {
      const auto now = Clock::now();
      const auto available = std::chrono::duration_cast<std::chrono::seconds>(deadline - now);
      if (limits.maxTime < available) {
        deadline = now + std::max(limits.maxTime, std::chrono::seconds::zero());
      }
    }
    return {limits, cancel, progress, deadline, {}, 0, 0};
  }
  void requireSpace(int folder, std::uint64_t bytes) {
    struct statvfs info{};
    if (::fstatvfs(folder, &info) != 0) {
      systemError("Could not inspect available archive storage");
    }
    const auto block = info.f_frsize ? info.f_frsize : info.f_bsize;
    if (!block) {
      return; // This filesystem supplies no useful capacity estimate; writes still report failures.
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto available = info.f_bavail > maximum / block ? maximum : info.f_bavail * block;
    if (bytes > available) {
      throwFileError(FileErrorKind::NoSpace, "Not enough available space to extract the selected archive files.");
    }
  }
  std::string bounded(const char* text) {
    if (!text) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive entry has no pathname.");
    }
    const auto size = ::strnlen(text, 4097);
    if (size > 4096) {
      throwFileError(FileErrorKind::NameTooLong, "Archive pathname is too long.");
    }
    return {text, size};
  }
  std::vector<std::string> parts(const std::string& name) {
    if (absolute(name) || name.contains('\\')) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive contains an absolute or nonportable pathname.");
    }
    std::vector<std::string> result;
    for (std::size_t start = 0; start < name.size();) {
      const auto end = name.find('/', start);
      auto component = name.substr(start, end == std::string::npos ? end : end - start);
      if (component == "..") {
        throwFileError(FileErrorKind::InvalidArgument, "Archive contains a parent-path traversal.");
      }
      if (!component.empty() && component != ".") {
        result.push_back(std::move(component));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return result;
  }
  std::string join(const std::vector<std::string>& components) {
    std::string result;
    for (const auto& component : components) {
      if (!result.empty()) {
        result += '/';
      }
      result += component;
    }
    return result;
  }
  std::string safePath(const std::string& name, const Work& work) {
    auto components = parts(name);
    if (components.size() > work.limits.maxDepth) {
      throwFileError(FileErrorKind::Unsupported, "Archive paths are too deep.");
    }
    return join(components);
  }
  std::string linkTarget(const std::string& path, const std::string& target, const Work& work) {
    if (target.empty() || absolute(target) || target.contains('\\')) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes the extraction folder.");
    }
    auto resolved = parts(path);
    resolved.pop_back();
    for (std::size_t start = 0; start < target.size();) {
      const auto end = target.find('/', start);
      const auto component = target.substr(start, end == std::string::npos ? end : end - start);
      if (component == "..") {
        if (resolved.empty()) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes the extraction folder.");
        }
        resolved.pop_back();
      } else if (!component.empty() && component != ".") {
        resolved.push_back(component);
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    if (resolved.size() > work.limits.maxDepth) {
      throwFileError(FileErrorKind::Unsupported, "Archive link is too deep.");
    }
    return join(resolved);
  }
  void validateLinks(const std::map<std::string, std::string>& links, Work& work) {
    for (const auto& [path, target] : links) {
      auto resolved = parts(path);
      resolved.pop_back();
      std::deque<std::string> remaining;
      const auto append = [&](const std::string& text) {
        if (absolute(text) || text.contains('\\')) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes the extraction folder.");
        }
        std::vector<std::string> tokens;
        for (std::size_t start = 0; start < text.size();) {
          const auto end = text.find('/', start);
          auto token = text.substr(start, end == std::string::npos ? end : end - start);
          if (!token.empty() && token != ".") {
            tokens.push_back(std::move(token));
          }
          if (end == std::string::npos) {
            break;
          }
          start = end + 1;
        }
        for (const auto& token : std::views::reverse(tokens)) {
          remaining.push_front(token);
        }
      };
      append(target);
      unsigned hops = 0;
      while (!remaining.empty()) {
        work.check();
        auto token = std::move(remaining.front());
        remaining.pop_front();
        if (token == "..") {
          if (resolved.empty()) {
            throwFileError(FileErrorKind::InvalidArgument, "Archive link escapes through another link.");
          }
          resolved.pop_back();
          continue;
        }
        resolved.push_back(std::move(token));
        if (resolved.size() > work.limits.maxDepth) {
          throwFileError(FileErrorKind::Unsupported, "Archive link is too deep.");
        }
        if (const auto found = links.find(join(resolved)); found != links.end()) {
          if (++hops > 40) {
            throwFileError(FileErrorKind::InvalidArgument, "Archive has a cyclic link chain.");
          }
          resolved.pop_back();
          append(found->second);
        }
      }
    }
  }
  std::string leaf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }
  std::string parentName(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
  }
  std::string native(const Location& location) {
    const auto value = location.nativePath();
    if (!value) {
      throwFileError(FileErrorKind::Unsupported, "Archives currently support native files and folders only.");
    }
    return *value;
  }
  std::pair<std::string, std::string> destinationParts(const Location& destination) {
    const fs::path path(native(destination));
    const auto name = path.filename().native();
    if (const auto problem = filenameProblem(name)) {
      throwFileError(FileErrorKind::InvalidArgument, *problem);
    }
    const Fd parent = opened(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return {path.parent_path().native(), name};
  }
  [[noreturn]] void archiveError(struct archive* value, Work& work) {
    if (work.failure) {
      std::rethrow_exception(work.failure);
    }
    throwFileError(
        FileErrorKind::Other, archive_error_string(value) ? archive_error_string(value) : "Archive backend failed."
    );
  }
  void requireArchive(int status, struct archive* value, Work& work) {
    if (status != ARCHIVE_OK) {
      archiveError(value, work);
    }
  }
} // namespace luciole::archive_work
std::optional<luciole::FileError> luciole::archive_work::cleanup(const std::vector<std::string>& paths) {
  std::optional<FileError> error;
  for (const auto& path : paths) {
    std::error_code result;
    fs::remove_all(path, result);
    if (result) {
      error =
          FileError{errnoError(result.value()).kind, "Could not remove private archive staging: " + result.message()};
    }
  }
  return error;
}
