#include "operations/archive_input.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <limits>
#include <set>
#include <unistd.h>

namespace {
  struct CloseDirectory {
    void operator()(DIR* value) const { ::closedir(value); }
  };
} // namespace

std::vector<std::string> luciole::archive_work::Input::discover() const {
  const int fd = ::openat(m_parent.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    systemError("Could not inspect split ZIP parts");
  }
  const std::unique_ptr<DIR, CloseDirectory> stream(::fdopendir(fd));
  if (!stream) {
    const int code = errno;
    ::close(fd);
    errno = code;
    systemError("Could not inspect split ZIP parts");
  }
  std::set<unsigned> numbers;
  for (;;) {
    m_work.check();
    errno = 0;
    const auto* entry = ::readdir(stream.get());
    if (!entry) {
      if (errno) {
        systemError("Could not enumerate split ZIP parts");
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (!name.starts_with(m_prefix) || name.size() != m_prefix.size() + 3) {
      continue;
    }
    const auto suffix = name.substr(m_prefix.size());
    if (!std::ranges::all_of(suffix, [](char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    const auto index = static_cast<unsigned>((suffix[0] - '0') * 100 + (suffix[1] - '0') * 10 + suffix[2] - '0');
    if (!index) {
      throwFileError(FileErrorKind::InvalidArgument, "Split ZIP parts start at 001.");
    }
    numbers.insert(index);
  }
  if (!numbers.contains(1)) {
    throwFileError(FileErrorKind::Missing, "Split ZIP part 001 is missing.");
  }
  if (numbers.size() < 2) {
    throwFileError(FileErrorKind::Missing, "Split ZIP part 002 is missing. Keep all parts in the same folder.");
  }
  std::vector<std::string> names;
  for (unsigned index = 1; index <= *numbers.rbegin(); ++index) {
    if (!numbers.contains(index)) {
      throwFileError(FileErrorKind::Missing, std::format("Split ZIP part {:03} is missing.", index));
    }
    names.push_back(m_prefix + std::format("{:03}", index));
  }
  return names;
}

luciole::archive_work::Input::Input(const Location& source, Work& work)
    : m_work(work), m_parentPath(std::filesystem::path(native(source)).parent_path().native()),
      m_parent(opened(::open(m_parentPath.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC))),
      m_parentIdentity(statFd(m_parent.get())) {
  m_work.check();
  const auto name = std::filesystem::path(native(source)).filename().native();
  std::vector<std::string> names{name};
  if (const auto part = splitZipPart(source)) {
    if (*part != 1) {
      throwFileError(FileErrorKind::InvalidArgument, "Open the first split ZIP part (.zip.001).");
    }
    m_prefix = name.substr(0, name.size() - 3);
    names = discover();
  }
  std::set<std::pair<dev_t, ino_t>> identities;
  for (const auto& part : names) {
    m_work.check();
    auto fd = opened(::openat(m_parent.get(), part.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    const auto info = statFd(fd.get());
    if (!S_ISREG(info.st_mode)
        || info.st_size <= 0
        || static_cast<std::uint64_t>(info.st_size) > work.limits.maxArchiveBytes - m_size) {
      throwFileError(
          FileErrorKind::Unsupported, "Archive is empty, special, or exceeds the combined input-size limit."
      );
    }
    if (!identities.emplace(info.st_dev, info.st_ino).second) {
      throwFileError(FileErrorKind::InvalidArgument, "Split ZIP parts must be distinct regular files.");
    }
    const auto size = static_cast<std::uint64_t>(info.st_size);
    m_parts.push_back({part, stamp(info), std::move(fd), m_size, size});
    m_size += size;
    if (m_size > static_cast<std::uint64_t>(INT64_MAX)) {
      throwFileError(FileErrorKind::Unsupported, "Archive exceeds the seekable input-size limit.");
    }
  }
  if (!split()) {
    m_fingerprint = m_parts.front().original;
  } else {
    for (const auto& part : m_parts) {
      m_fingerprint += std::format("{}:{}:{};", part.name.size(), part.name, part.original);
    }
  }
  unchanged();
}

std::size_t luciole::archive_work::Input::read(std::span<char> buffer) {
  std::size_t count = 0;
  while (count < buffer.size() && m_position < m_size) {
    m_work.check();
    m_work.check();
    const auto part = std::prev(std::ranges::upper_bound(m_parts, m_position, {}, &Part::start));
    const auto offset = m_position - part->start;
    const auto size = std::min<std::uint64_t>(buffer.size() - count, part->size - offset);
    const auto budget = m_work.limits.maxArchiveBytes > std::numeric_limits<std::uint64_t>::max() / 4
        ? std::numeric_limits<std::uint64_t>::max()
        : m_work.limits.maxArchiveBytes * 4;
    if (m_readBytes > budget || size > budget - m_readBytes) {
      throwFileError(FileErrorKind::Unsupported, "Archive exceeded its input-work limit.");
    }
    const auto received = ::pread(part->fd.get(), buffer.data() + count, size, static_cast<off_t>(offset));
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0) {
      systemError("Could not read archive part");
    }
    if (!received) {
      throwFileError(FileErrorKind::Changed, "An archive part was truncated during this operation.");
    }
    const auto bytes = static_cast<std::uint64_t>(received);
    count += bytes;
    m_position += bytes;
    m_readBytes += bytes;
  }
  return count;
}

std::int64_t luciole::archive_work::Input::seek(std::int64_t offset, int whence) {
  m_work.check();
  const auto base = whence == SEEK_SET ? 0U : whence == SEEK_CUR ? m_position : m_size;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    throwFileError(FileErrorKind::InvalidArgument, "Invalid archive seek origin.");
  }
  std::uint64_t next;
  if (offset < 0) {
    const auto distance = static_cast<std::uint64_t>(-(offset + 1)) + 1;
    if (distance > base) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive seek precedes its input.");
    }
    next = base - distance;
  } else {
    if (base > static_cast<std::uint64_t>(INT64_MAX) || static_cast<std::uint64_t>(offset) > INT64_MAX - base) {
      throwFileError(FileErrorKind::InvalidArgument, "Archive seek exceeds its input range.");
    }
    next = base + static_cast<std::uint64_t>(offset);
  }
  m_position = next;
  return static_cast<std::int64_t>(next);
}

void luciole::archive_work::Input::unchanged() const {
  m_work.check();
  struct stat parent{};
  if (::stat(m_parentPath.c_str(), &parent) != 0 || !sameIdentity(parent, m_parentIdentity)) {
    throwFileError(FileErrorKind::Changed, "The archive folder moved or was replaced.");
  }
  for (const auto& part : m_parts) {
    m_work.check();
    struct stat named{};
    if (stamp(statFd(part.fd.get())) != part.original
        || ::fstatat(m_parent.get(), part.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0
        || stamp(named) != part.original) {
      throwFileError(FileErrorKind::Changed, "An archive part changed. Reload the archive contents.");
    }
  }
  if (split()) {
    const auto names = discover();
    if (names.size() != m_parts.size() || !std::ranges::equal(names, m_parts, {}, {}, &Part::name)) {
      throwFileError(FileErrorKind::Changed, "The split ZIP part set changed. Reload the archive contents.");
    }
  }
}

luciole::Fd luciole::archive_work::Input::duplicateSingle() const {
  if (split() || m_parts.size() != 1) {
    throwFileError(FileErrorKind::Unsupported, "This provider requires one ordinary archive file.");
  }
  return opened(::fcntl(m_parts.front().fd.get(), F_DUPFD_CLOEXEC, 4));
}
