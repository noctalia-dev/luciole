#include "operations/archive_sevenzip.h"

#include "fs/gio_interop.h"
#include "operations/archive_provider_process.h"

#include <charconv>
#include <filesystem>
#include <limits>
#include <map>
#include <ranges>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using namespace luciole;
  using namespace luciole::archive_work;
  // 7-Zip exit status for fatal errors, including wrong passwords and damaged encrypted data.
  constexpr int fatalStatus = 2;
  // A listing record carries ~200 bytes of fixed fields besides its path.
  constexpr std::uint64_t recordBytes = 512, reportLimit = 1024U * 1024U;

  std::string program() {
    // Explicit override supports isolated provider fixtures.
    if (const auto* override = g_getenv("LUCIOLE_7Z")) {
      if (!std::filesystem::path(override).is_absolute()) {
        throwFileError(FileErrorKind::Unavailable, "The 7-Zip command path must be absolute.");
      }
      return override;
    }
    for (const auto* name : {"7zz", "7z", "7za"}) {
      if (const luciole::GString found(g_find_program_in_path(name)); found) {
        return found.get();
      }
    }
    throwFileError(FileErrorKind::Unavailable, "Encrypted 7z archives need the 7-Zip command (7zz or 7z) installed.");
  }

  struct Run {
    int status;
    std::string output;
    bool wrongPassword() const { return status == fatalStatus && output.contains("Wrong password"); }
  };
  Run run(
      Input& input, Work& work, std::string_view password, const std::vector<std::string>& arguments,
      std::uint64_t outputLimit, std::uint64_t fileLimit
  ) {
    if (const auto problem = archivePasswordProblem(password, ArchiveFormat::SevenZip)) {
      throwFileError(FileErrorKind::InvalidArgument, *problem);
    }
    const auto executable = program();
    std::vector<const char*> argv{executable.c_str()};
    for (const auto& argument : arguments) {
      argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);
    const auto output = opened(::memfd_create("luciole-7z-output", MFD_CLOEXEC));
    const auto status = runArchiveProvider(input, work, password, argv, output.get(), outputLimit, fileLimit);
    std::string text(static_cast<std::size_t>(statFd(output.get()).st_size), '\0');
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto count = ::pread(output.get(), text.data() + offset, text.size() - offset, static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        systemError("Could not read the 7-Zip output");
      }
      offset += static_cast<std::size_t>(count);
    }
    return {status, std::move(text)};
  }

  [[noreturn]] void invalidListing() {
    throwFileError(FileErrorKind::InvalidArgument, "The 7-Zip command returned an invalid listing.");
  }
  unsigned permissions(std::string_view text) {
    // "drwxr-xr-x": setuid/setgid/sticky letters keep only their execute meaning.
    unsigned mode = 0;
    for (std::size_t i = 0; i < 9; ++i) {
      const auto c = text[i + 1];
      const bool set = i % 3 == 2 ? (c == 'x' || c == 's' || c == 't') : c != '-';
      mode = (mode << 1U) | (set ? 1U : 0U);
    }
    return mode;
  }
  SevenZipMember member(const std::map<std::string, std::string, std::less<>>& fields) {
    const auto field = [&](std::string_view key) -> std::string_view {
      const auto found = fields.find(key);
      return found == fields.end() ? std::string_view{} : std::string_view(found->second);
    };
    if (!field("Symbolic Link").empty() || !field("Hard Link").empty()) {
      throwFileError(FileErrorKind::Unsupported, "Encrypted 7z link metadata is not supported yet.");
    }
    SevenZipMember result{std::string(field("Path")), 0, 0, false, field("Encrypted") == "+"};
    // "Attributes = DA drwxr-xr-x": Windows letters, then an optional Unix mode.
    const auto attributes = field("Attributes");
    const auto space = attributes.find(' ');
    const auto letters = attributes.substr(0, space);
    result.directory = letters.contains('D') || field("Folder") == "+";
    result.mode = result.directory ? 0755U : 0644U;
    if (space != std::string_view::npos) {
      const auto unix = attributes.substr(space + 1);
      if (unix.size() != 10) {
        invalidListing();
      }
      if (unix[0] != '-' && unix[0] != 'd') {
        throwFileError(FileErrorKind::Unsupported, "Encrypted 7z link metadata is not supported yet.");
      }
      if ((unix[0] == 'd') != result.directory) {
        invalidListing();
      }
      result.mode = permissions(unix);
    }
    const auto size = field("Size");
    if (size.empty()) {
      if (!result.directory) {
        invalidListing();
      }
    } else {
      const auto [end, error] = std::from_chars(size.data(), size.data() + size.size(), result.size);
      if (error != std::errc{} || end != size.data() + size.size() || result.size > INT64_MAX) {
        invalidListing();
      }
    }
    return result;
  }
  // `7z l -slt` prints "Key = Value" records separated by blank lines. 7-Zip replaces
  // control characters in displayed names, so a record never spans a name's newline.
  std::vector<SevenZipMember> parse(std::string_view text, const Work& work) {
    std::vector<SevenZipMember> result;
    std::map<std::string, std::string, std::less<>> fields;
    bool started = false;
    const auto finish = [&] {
      if (fields.empty()) {
        return;
      }
      if (result.size() >= work.limits.maxEntries) {
        throwFileError(FileErrorKind::Unsupported, "Archive contents exceed the metadata limit.");
      }
      result.push_back(member(fields));
      fields.clear();
    };
    for (const auto range : text | std::views::split('\n')) {
      const std::string_view line(range.begin(), range.end());
      if (!started) {
        // Skip the password prompt and blank lines before the first record.
        started = line.starts_with("Path = ");
        if (!started) {
          continue;
        }
      }
      if (line.empty()) {
        finish();
        continue;
      }
      const auto separator = line.find(" = ");
      if (separator == std::string_view::npos
          || (fields.empty() && !line.starts_with("Path = "))
          || !fields.emplace(line.substr(0, separator), line.substr(separator + 3)).second) {
        invalidListing();
      }
    }
    finish();
    return result;
  }
  [[noreturn]] void requestPassword(Work& work, const char* message) {
    work.progress.passwordFormat = ArchiveFormat::SevenZip;
    work.progress.passwordNeeded.store(true);
    throwFileError(FileErrorKind::Denied, message);
  }
  std::vector<std::string_view> attempts(const Passwords& passwords) {
    if (passwords.values.empty()) {
      return {std::string_view{}};
    }
    // Newest password first.
    std::vector<std::string_view> result(passwords.values.rbegin(), passwords.values.rend());
    return result;
  }
} // namespace

std::vector<luciole::archive_work::SevenZipMember>
luciole::archive_work::sevenZipCatalog(Input& input, Work& work, const Passwords& passwords) {
  // No catalog fits below one minimal record; fail before any password prompt.
  if (work.limits.maxMetadataBytes < 32 || work.limits.maxEntries == 0) {
    throwFileError(FileErrorKind::Unsupported, "The 7z catalog exceeds its metadata limit.");
  }
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto outputLimit = work.limits.maxEntries > (maximum - work.limits.maxMetadataBytes) / recordBytes
      ? maximum
      : work.limits.maxMetadataBytes + work.limits.maxEntries * recordBytes;
  const std::vector<std::string> arguments{"l", "-slt", "-ba", "-t7z", "-bsp0", "-bse1", "/proc/self/fd/3"};
  for (const auto password : attempts(passwords)) {
    work.check();
    const auto result = run(input, work, password, arguments, outputLimit, 0);
    if (result.wrongPassword()) {
      continue;
    }
    if (result.status != 0) {
      throwFileError(FileErrorKind::InvalidArgument, "The 7-Zip command could not read this archive.");
    }
    return parse(result.output, work);
  }
  requestPassword(work, "Could not unlock the 7z header.");
}

void luciole::archive_work::extractSevenZip(
    Input& input, Work& work, const Passwords& passwords, const std::vector<std::string>& names, bool encrypted,
    std::uint64_t largest, const std::string& directory
) {
  if (passwords.values.empty() && encrypted) {
    requestPassword(work, "Could not unlock the selected 7z members.");
  }
  // -spd: literal names, no wildcards. "--" keeps names such as "-p..." or "@list" literal.
  std::vector<std::string> arguments{"x",     "-y",   "-t7z",           "-bso0",          "-bsp0",
                                     "-bse1", "-spd", "-o" + directory, "/proc/self/fd/3"};
  if (!names.empty()) {
    arguments.emplace_back("--");
    arguments.insert(arguments.end(), names.begin(), names.end());
  }
  for (const auto password : attempts(passwords)) {
    work.check();
    // A failed attempt may leave partial output; every attempt starts empty.
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (::mkdir(directory.c_str(), 0700) != 0) {
      systemError("Could not prepare 7z output");
    }
    const auto result = run(input, work, password, arguments, reportLimit, std::max<std::uint64_t>(largest, 1));
    if (result.wrongPassword()) {
      continue;
    }
    if (result.status != 0) {
      throwFileError(FileErrorKind::Unsupported, "The 7-Zip command could not extract this archive within its limits.");
    }
    return;
  }
  requestPassword(work, "Could not unlock the selected 7z members.");
}
