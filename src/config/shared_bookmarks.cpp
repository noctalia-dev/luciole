#include "config/shared_bookmarks.h"

#include "fs/posix_fd.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <glib.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace {
  constexpr std::size_t kMaxBytes = 256 * 1024;

  // Shared files may be managed through a read-only symlink (for example by Nix).
  // Reads follow it; writes use the checked atomic writer and refuse linked targets.
  std::expected<std::optional<std::string>, std::string> read(const std::filesystem::path& path) {
    const luciole::Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!file.valid()) {
      return errno == ENOENT ? std::expected<std::optional<std::string>, std::string>(std::nullopt)
                             : std::unexpected("Cannot open shared bookmarks.");
    }
    const auto before = luciole::statFd(file.get());
    if (!S_ISREG(before.st_mode)) {
      return std::unexpected("Shared bookmarks must be a regular text file.");
    }
    std::string text;
    std::array<char, 8192> buffer{};
    while (true) {
      const auto count = ::read(file.get(), buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        return std::unexpected("Cannot read shared bookmarks.");
      }
      if (count == 0) {
        break;
      }
      text.append(buffer.data(), static_cast<std::size_t>(count));
      if (text.size() > kMaxBytes) {
        return std::unexpected("Shared bookmarks exceed the 256 KiB limit.");
      }
    }
    if (!luciole::sameFileRevision(before, luciole::statFd(file.get()))) {
      return std::unexpected("Shared bookmarks changed while reading; retry.");
    }
    return text;
  }

  std::string displayLabel(std::string label) {
    for (auto& c : label) {
      if (static_cast<unsigned char>(c) < 32 || c == 127) {
        c = ' ';
      }
    }
    if (label.size() > 256) {
      auto end = std::size_t{256};
      while ((static_cast<unsigned char>(label[end]) & 0xc0U) == 0x80U) {
        --end;
      }
      label.resize(end);
    }
    return label.empty() ? "Folder" : label;
  }
} // namespace

luciole::SharedBookmarks::SharedBookmarks(std::filesystem::path path, std::filesystem::path legacy)
    : m_path(std::move(path)), m_legacy(std::move(legacy)) {}

std::filesystem::path luciole::SharedBookmarks::pathFromEnvironment() { return configHome() / "gtk-3.0/bookmarks"; }
std::filesystem::path luciole::SharedBookmarks::legacyFromEnvironment() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || !std::filesystem::path(home).is_absolute()) {
    throw std::invalid_argument("HOME must be an absolute path");
  }
  return std::filesystem::path(home) / ".gtk-bookmarks";
}

bool luciole::SharedBookmarks::load() {
  m_error.clear();
  m_loaded = false;
  if (!m_path.is_absolute()
      || m_path.native().contains('\0')
      || (!m_legacy.empty() && (!m_legacy.is_absolute() || m_legacy.native().contains('\0')))) {
    m_error = "Expected absolute bookmark paths.";
    return false;
  }
  auto contents = read(m_path);
  if (!contents) {
    m_error = contents.error();
    return false;
  }
  m_expected = *contents;
  m_seenModern = m_seenModern || contents->has_value();
  if (!*contents && !m_seenModern && !m_legacy.empty()) {
    contents = read(m_legacy);
    if (!contents) {
      m_error = contents.error();
      return false;
    }
  }
  parse(contents->value_or(""));
  m_loaded = true;
  return true;
}

std::vector<std::filesystem::path> luciole::SharedBookmarks::watchPaths() const {
  std::vector<std::filesystem::path> paths;
  for (const auto& path : {m_path, m_legacy}) {
    if (!path.empty()) {
      std::error_code error;
      const auto resolved = std::filesystem::canonical(path, error);
      if (!error && resolved != path) {
        paths.push_back(resolved);
      }
    }
  }
  return paths;
}

void luciole::SharedBookmarks::parse(const std::string& text) {
  m_lines.clear();
  m_bookmarks.clear();
  std::unordered_set<std::string> seen;
  std::string_view remaining(text);
  while (!remaining.empty()) {
    const auto end = remaining.find('\n');
    const auto raw = remaining.substr(0, end);
    Line line{.raw = std::string(raw), .bookmark = {}, .duplicate = false};
    remaining = end == std::string_view::npos ? std::string_view{} : remaining.substr(end + 1);
    auto value = raw;
    if (value.ends_with('\r')) {
      value.remove_suffix(1);
    }
    if (!value.empty()
        && !value.contains('\0')
        && g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr)) {
      const auto space = value.find(' ');
      const std::string uri(value.substr(0, space));
      if (!storedUriProblem(uri)) {
        try {
          auto location = Location::fromUri(uri);
          if (!location.isTrash() || location == Location::trash()) {
            auto label = space == std::string_view::npos ? location.displayName().value_or(uri)
                                                         : std::string(value.substr(space + 1));
            line.bookmark = Bookmark{displayLabel(std::move(label)), std::move(location), {}};
            line.duplicate = seen.contains(line.bookmark->location.uri());
            if (!line.duplicate && m_bookmarks.size() < StateStore::bookmarkLimit) {
              seen.insert(line.bookmark->location.uri());
              m_bookmarks.push_back(*line.bookmark);
            } else if (!line.duplicate) {
              // Overflow records remain opaque and cannot be removed through a truncated view.
              line.bookmark.reset();
            }
          }
        } catch (const std::invalid_argument&) {
          // Foreign records are retained verbatim rather than discarded on save.
        }
      }
    }
    m_lines.push_back(std::move(line));
  }
}

std::string luciole::SharedBookmarks::format(const std::vector<Bookmark>& bookmarks) const {
  std::string text;
  auto next = bookmarks.begin();
  const auto append = [&](const Bookmark& bookmark) {
    const auto original = std::ranges::find_if(m_lines, [&](const auto& line) {
      return line.bookmark && line.bookmark->location == bookmark.location && line.bookmark->label == bookmark.label;
    });
    text += original != m_lines.end() ? original->raw : bookmark.location.uri() + ' ' + bookmark.label;
    text += '\n';
  };
  for (const auto& line : m_lines) {
    if (line.duplicate) {
      continue;
    }
    if (!line.bookmark) {
      text += line.raw + '\n';
    } else if (next != bookmarks.end()) {
      append(*next++);
    }
  }
  for (; next != bookmarks.end(); ++next) {
    append(*next);
  }
  // A reordered primary must precede its retained duplicates, including differently labelled ones.
  for (const auto& line : m_lines) {
    if (line.duplicate
        && std::ranges::any_of(bookmarks, [&](const auto& pin) { return pin.location == line.bookmark->location; })) {
      text += line.raw + '\n';
    }
  }
  return text;
}

luciole::AtomicWriteResult luciole::SharedBookmarks::save(const std::vector<Bookmark>& bookmarks) {
  if (!m_loaded) {
    return {.error = "Load shared bookmarks before editing."};
  }
  if (bookmarks.size() > StateStore::bookmarkLimit) {
    return {.error = "At most 256 bookmarks are supported."};
  }
  std::unordered_set<std::string> seen;
  for (const auto& bookmark : bookmarks) {
    if (storedUriProblem(bookmark.location.uri())
        || bookmarkLabelProblem(bookmark.label)
        || !seen.insert(bookmark.location.uri()).second) {
      return {.error = "Invalid bookmark label, duplicate location or unsafe stored URI."};
    }
  }
  const auto sameSharedContents = bookmarks.size() == m_bookmarks.size()
      && std::ranges::equal(bookmarks, m_bookmarks, [](const auto& left, const auto& right) {
                                    return left.location == right.location && left.label == right.label;
                                  });
  if (sameSharedContents) {
    return {.state = AtomicWriteState::Unchanged, .error = {}};
  }
  const auto text = format(bookmarks);
  auto result = updateTextFileAtomic(
      m_path, [&](const std::optional<std::string>& current) -> std::expected<std::optional<std::string>, std::string> {
        if (current != m_expected) {
          return std::unexpected("Shared bookmarks changed in another application; reload before retrying.");
        }
        return text;
      }
  );
  if (result.state != AtomicWriteState::NotCommitted) {
    m_seenModern = true;
    m_expected = text;
    parse(text);
  }
  return result;
}
