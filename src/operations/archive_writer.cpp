#include "operations/archive_support.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <glib.h>
#include <set>
#include <unistd.h>

namespace {
  using namespace luciole;
  using namespace luciole::archive_work;
  namespace fs = std::filesystem;
  int addFilter(struct archive* writer, ArchiveCompression compression) {
    switch (compression) {
    case ArchiveCompression::None:
      return archive_write_add_filter_none(writer);
    case ArchiveCompression::Gzip:
      return archive_write_add_filter_gzip(writer);
    case ArchiveCompression::Bzip2:
      return archive_write_add_filter_bzip2(writer);
    case ArchiveCompression::Zstd:
      return archive_write_add_filter_zstd(writer);
    case ArchiveCompression::Xz: {
      // Never register libarchive's external xz helper fallback. Fix encoder
      // memory use independently of machine core count or user environment.
      if (!archive_liblzma_version()) {
        return ARCHIVE_FATAL;
      }
      const auto status = archive_write_add_filter_xz(writer);
      if (status != ARCHIVE_OK) {
        return status;
      }
      const auto level = archive_write_set_filter_option(writer, "xz", "compression-level", "6");
      return level == ARCHIVE_OK ? archive_write_set_filter_option(writer, "xz", "threads", "1") : level;
    }
    }
    return ARCHIVE_FATAL;
  }
  int setFormat(struct archive* writer, ArchiveFormat format) {
    if (format == ArchiveFormat::Zip) {
      return archive_write_set_format_zip(writer);
    }
    if (format != ArchiveFormat::SevenZip) {
      return archive_write_set_format_pax_restricted(writer);
    }
    if (!archive_liblzma_version()) {
      return ARCHIVE_FATAL;
    }
    auto status = archive_write_set_format_7zip(writer);
    if (status == ARCHIVE_OK) {
      status = archive_write_set_format_option(writer, "7zip", "compression", "lzma2");
    }
    if (status == ARCHIVE_OK) {
      status = archive_write_set_format_option(writer, "7zip", "compression-level", "6");
    }
    if (status == ARCHIVE_OK) {
      status = archive_write_set_format_option(writer, "7zip", "threads", "1");
    }
    return status;
  }
  void forEachChild(int fd, Work& work, const std::function<void(const std::string&)>& visit) {
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
      systemError("Could not inspect archive source");
    }
    struct CloseDirectory {
      void operator()(DIR* value) const { ::closedir(value); }
    };
    const std::unique_ptr<DIR, CloseDirectory> stream(::fdopendir(duplicate));
    if (!stream) {
      ::close(duplicate);
      systemError("Could not inspect archive source");
    }
    errno = 0;
    while (const auto* entry = ::readdir(stream.get())) {
      work.check();
      if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..") {
        visit(entry->d_name);
      }
      errno = 0;
    }
    if (errno) {
      systemError("Could not enumerate archive source");
    }
  }
  struct Writer {
    Work& work;
    Fd output;
    std::uint64_t bytes = 0;
    std::size_t metadataBytes = 0;
    // One buffer per job, rather than a large stack frame for every nested folder.
    std::array<char, 65536> buffer{};
    std::unique_ptr<struct archive, decltype(&archive_write_free)> value{archive_write_new(), &archive_write_free};
    Writer(const std::string& filename, const ArchiveFormatInfo& format, Work& request, const Passwords& passwords)
        : work(request),
          output(opened(::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600))) {
      requireArchive(setFormat(value.get(), format.format), value.get(), work);
      if (format.format == ArchiveFormat::Zip && !passwords.values.empty()) {
        requireArchive(archive_write_set_options(value.get(), "zip:encryption=aes256"), value.get(), work);
        requireArchive(archive_write_set_passphrase(value.get(), passwords.values.back().c_str()), value.get(), work);
      }
      requireArchive(addFilter(value.get(), format.compression), value.get(), work);
      requireArchive(archive_write_set_bytes_per_block(value.get(), 0), value.get(), work);
      requireArchive(
          archive_write_open(
              value.get(), this, nullptr,
              [](struct archive* a, void* data, const void* buffer, std::size_t size) -> la_ssize_t {
                auto& writer = *static_cast<Writer*>(data);
                try {
                  writer.work.check();
                  if (size > writer.work.limits.maxArchiveBytes - writer.bytes) {
                    throwFileError(FileErrorKind::NoSpace, "Compressed archive exceeds the staging-size limit.");
                  }
                  std::size_t done = 0;
                  while (done < size) {
                    const auto count =
                        ::write(writer.output.get(), static_cast<const char*>(buffer) + done, size - done);
                    if (count < 0 && errno == EINTR) {
                      continue;
                    }
                    if (count <= 0) {
                      systemError("Could not write compressed archive");
                    }
                    done += static_cast<std::size_t>(count);
                  }
                  writer.bytes += size;
                  return static_cast<la_ssize_t>(size);
                } catch (...) {
                  writer.work.failure = std::current_exception();
                  archive_set_error(a, EIO, "Archive write failed");
                  return -1;
                }
              },
              nullptr
          ),
          value.get(), work
      );
    }
  };
  void compressItem(
      int parent, const std::string& name, const std::string& path, Writer& writer, std::set<std::string>& names,
      unsigned depth
  ) {
    auto& work = writer.work;
    work.item();
    if (depth > work.limits.maxDepth || path.size() > 4096 || !names.insert(path).second) {
      throwFileError(FileErrorKind::Unsupported, "Archive paths are duplicated or too deep.");
    }
    (void)safePath(path, work);
    if (path.size() > work.limits.maxMetadataBytes - writer.metadataBytes) {
      throwFileError(FileErrorKind::Unsupported, "Archive source names exceed the metadata limit.");
    }
    writer.metadataBytes += path.size();
    const auto before = statAt(parent, name);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), path.c_str());
    archive_entry_set_perm(entry.get(), before.st_mode & 0777);
    archive_entry_set_mtime(entry.get(), before.st_mtim.tv_sec, before.st_mtim.tv_nsec);
    if (S_ISDIR(before.st_mode)) {
      archive_entry_set_filetype(entry.get(), AE_IFDIR);
      archive_entry_set_size(entry.get(), 0);
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
      const auto folder = opened(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (stamp(statFd(folder.get())) != stamp(before)) {
        throwFileError(FileErrorKind::Changed, "An archive source changed.");
      }
      forEachChild(folder.get(), work, [&](const std::string& child) {
        auto childPath = path;
        childPath += '/';
        childPath += child;
        compressItem(folder.get(), child, childPath, writer, names, depth + 1);
      });
      if (stamp(statFd(folder.get())) != stamp(before)) {
        throwFileError(FileErrorKind::Changed, "An archive source folder changed.");
      }
    } else if (S_ISREG(before.st_mode)) {
      if (before.st_size < 0) {
        throwFileError(FileErrorKind::InvalidArgument, "An archive source has an invalid size.");
      }
      work.account(static_cast<std::uint64_t>(before.st_size));
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_size(entry.get(), before.st_size);
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
      const auto input = opened(::openat(parent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      if (stamp(statFd(input.get())) != stamp(before)) {
        throwFileError(FileErrorKind::Changed, "An archive source changed.");
      }
      auto& buffer = writer.buffer;
      auto remaining = static_cast<std::uint64_t>(before.st_size);
      while (remaining) {
        work.check();
        ssize_t count;
        do {
          count = ::read(input.get(), buffer.data(), std::min<std::uint64_t>(remaining, buffer.size()));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
          throwFileError(FileErrorKind::Changed, "An archive source could not be read completely.");
        }
        std::size_t done = 0;
        while (done < static_cast<std::size_t>(count)) {
          const auto written =
              archive_write_data(writer.value.get(), buffer.data() + done, static_cast<std::size_t>(count) - done);
          if (written <= 0) {
            archiveError(writer.value.get(), work);
          }
          done += static_cast<std::size_t>(written);
        }
        remaining -= static_cast<std::uint64_t>(count);
        work.progress.bytes.fetch_add(static_cast<std::uint64_t>(count));
      }
      if (stamp(statFd(input.get())) != stamp(before)) {
        throwFileError(FileErrorKind::Changed, "An archive source changed while reading.");
      }
    } else if (S_ISLNK(before.st_mode)) {
      auto& target = writer.buffer;
      constexpr std::size_t maximum = 4097;
      const auto size = ::readlinkat(parent, name.c_str(), target.data(), maximum);
      if (size < 0 || size >= static_cast<ssize_t>(maximum)) {
        throwFileError(FileErrorKind::Other, "Could not read archive source link.");
      }
      const std::string link(target.data(), static_cast<std::size_t>(size));
      // Creation stores the link itself. Only reading/extraction resolves and validates its target.
      if (link.size() > work.limits.maxMetadataBytes - writer.metadataBytes) {
        throwFileError(FileErrorKind::Unsupported, "Archive link names exceed the metadata limit.");
      }
      writer.metadataBytes += link.size();
      archive_entry_set_filetype(entry.get(), AE_IFLNK);
      archive_entry_set_size(entry.get(), 0);
      archive_entry_set_symlink(entry.get(), link.c_str());
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
    } else {
      throwFileError(FileErrorKind::Unsupported, "Special files cannot be compressed.");
    }
    if (stamp(statAt(parent, name)) != stamp(before)) {
      throwFileError(FileErrorKind::Changed, "An archive source was replaced.");
    }
  }
} // namespace

bool luciole::archiveEncryptionAvailable() {
  static const bool available = [] {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    return writer
        && archive_write_set_format_zip(writer.get()) == ARCHIVE_OK
        && archive_write_set_options(writer.get(), "zip:encryption=aes256") == ARCHIVE_OK;
  }();
  return available;
}

std::optional<std::string> luciole::archivePasswordProblem(std::string_view password, ArchiveFormat format) {
  if (password.size() > 1024 || password.contains('\0')) {
    return "Use a password of at most 1024 bytes without NUL characters.";
  }
  if (format == ArchiveFormat::SevenZip
      && (password.find_first_of("\r\n") != std::string_view::npos
          || !g_utf8_validate(password.data(), static_cast<gssize>(password.size()), nullptr))) {
    return "7z passwords must be UTF-8 text without line breaks.";
  }
  return std::nullopt;
}

std::span<const luciole::ArchiveFormatInfo> luciole::archiveFormats() {
  static const auto formats = [] {
    std::vector<ArchiveFormatInfo> result;
    for (const auto& item : std::array{
             ArchiveFormatInfo{ArchiveFormat::Zip, "ZIP", ".zip"}, ArchiveFormatInfo{ArchiveFormat::Tar, "TAR", ".tar"},
             ArchiveFormatInfo{ArchiveFormat::TarGzip, "TAR gzip", ".tar.gz", ArchiveCompression::Gzip},
             ArchiveFormatInfo{ArchiveFormat::TarBzip2, "TAR bz2", ".tar.bz2", ArchiveCompression::Bzip2},
             ArchiveFormatInfo{ArchiveFormat::TarZstd, "TAR zstd", ".tar.zst", ArchiveCompression::Zstd},
             ArchiveFormatInfo{ArchiveFormat::TarXz, "TAR xz", ".tar.xz", ArchiveCompression::Xz},
             ArchiveFormatInfo{ArchiveFormat::SevenZip, "7z", ".7z"}
         }) {
      if (!archiveCompressionAvailable(item.compression)) {
        continue;
      }
      const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
          archive_write_new(), &archive_write_free
      );
      const auto format = setFormat(writer.get(), item.format);
      const auto filter = addFilter(writer.get(), item.compression);
      if (format == ARCHIVE_OK && filter == ARCHIVE_OK) {
        result.push_back(item);
      }
    }
    return result;
  }();
  return formats;
}
luciole::archive_work::Prepared luciole::archive_work::compress(
    const std::vector<Location>& sources, const Location& destination, ArchiveFormat format, ArchiveLimits limits,
    const CancelToken& cancel, Progress& progress, const Passwords& passwords
) {
  const ArchiveLocale locale;
  auto work = workFor(limits, cancel, progress);
  if (!passwords.values.empty() && (format != ArchiveFormat::Zip || !archiveEncryptionAvailable())) {
    throwFileError(FileErrorKind::Unsupported, "AES-256 ZIP encryption is unavailable for this format.");
  }
  const auto formats = archiveFormats();
  const auto descriptor = std::ranges::find(formats, format, &ArchiveFormatInfo::format);
  if (descriptor == formats.end()) {
    throwFileError(FileErrorKind::Unsupported, "Archive write format is unavailable.");
  }
  const auto [parent, name] = destinationParts(destination);
  (void)parent;
  Stage stage;
  const auto output = fs::path(stage.path) / name;
  Writer writer(output.native(), *descriptor, work, passwords);
  std::set<std::string> names;
  for (const auto& source : sources) {
    if (source == destination) {
      throwFileError(FileErrorKind::InvalidArgument, "An archive cannot replace one of its inputs.");
    }
    const fs::path path(native(source));
    const auto inputParent = opened(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    compressItem(inputParent.get(), path.filename().native(), path.filename().native(), writer, names, 0);
  }
  requireArchive(archive_write_close(writer.value.get()), writer.value.get(), work);
  if (::fsync(writer.output.get()) != 0) {
    systemError("Could not synchronize the archive");
  }
  work.check();
  auto location = Location::fromPath(output.native());
  return {.stage = stage.release(), .catalog = {}, .output = std::move(location), .expandedBytes = work.expanded};
}
