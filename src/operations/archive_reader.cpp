#include "fs/filename.h"
#include "operations/archive_rar_input.h"
#include "operations/archive_sevenzip.h"
#include "operations/archive_support.h"
#include "operations/archive_xz_input.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <set>
#include <unistd.h>

namespace {
  using namespace luciole;
  using namespace luciole::archive_work;
  namespace fs = std::filesystem;
  struct Reader {
    Work& work;
    const Passwords* keys;
    Input input;
    std::int64_t headerOffset = 0;
    std::optional<XzInput> xz;
    std::string filename, original;
    std::unique_ptr<struct archive, decltype(&archive_read_free)> value{archive_read_new(), &archive_read_free};
    std::array<char, 65536> buffer{};
    std::optional<std::vector<SevenZipMember>> sevenzip;
    std::size_t memberPosition = 0;
    std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> nativeEntry{
        archive_entry_new(), &archive_entry_free
    };
    bool nativeSevenZip() const { return sevenzip.has_value(); }
    Reader(const Location& source, Work& request, const Passwords* passwords = nullptr, bool raw = false)
        : work(request), keys(passwords), input(source, request), filename(native(source)),
          original(input.fingerprint()) {
      if (work.progress.sourceStamp.empty()) {
        work.progress.sourceStamp = original;
      } else if (work.progress.sourceStamp != original) {
        throwFileError(FileErrorKind::Changed, "The archive changed while waiting for its password. Start again.");
      }
      initialize(passwords, raw);
    }
    void initialize(const Passwords* passwords, bool raw) {
      if (!value) {
        throwFileError(FileErrorKind::Other, "Could not initialize the archive reader.");
      }
      if (XzInput::recognizes(input)) {
        xz.emplace(input, work);
      }
      const auto rar = raw || xz ? RarInput::None : validateRarInput(input);
      if (raw) {
        requireArchive(archive_read_support_format_raw(value.get()), value.get(), work);
      } else if (rar != RarInput::None) {
        requireArchive(
            rar == RarInput::Rar4 ? archive_read_support_format_rar(value.get())
                                  : archive_read_support_format_rar5(value.get()),
            value.get(), work
        );
      } else if (xz) {
        requireArchive(archive_read_support_format_tar(value.get()), value.get(), work);
      } else {
        // Split input needs the final central directory; never recover a partial
        // set with the streaming ZIP reader after a missing or corrupt last part.
        requireArchive(
            input.split() ? archive_read_support_format_zip_seekable(value.get())
                          : archive_read_support_format_zip(value.get()),
            value.get(), work
        );
        if (!input.split()) {
          requireArchive(archive_read_support_format_tar(value.get()), value.get(), work);
          requireArchive(archive_read_support_format_7zip(value.get()), value.get(), work);
        }
      }
      requireArchive(archive_read_support_filter_none(value.get()), value.get(), work);
      if (!input.split() && !xz && rar == RarInput::None) {
        if (archiveCompressionAvailable(ArchiveCompression::Gzip)) {
          requireArchive(archive_read_support_filter_gzip(value.get()), value.get(), work);
        }
        if (archiveCompressionAvailable(ArchiveCompression::Bzip2)) {
          requireArchive(archive_read_support_filter_bzip2(value.get()), value.get(), work);
        }
        if (archiveCompressionAvailable(ArchiveCompression::Zstd)) {
          requireArchive(archive_read_support_filter_zstd(value.get()), value.get(), work);
        }
      }
      if (passwords) {
        for (const auto& password : passwords->values) {
          requireArchive(archive_read_add_passphrase(value.get(), password.c_str()), value.get(), work);
        }
      }
      requireArchive(
          archive_read_set_passphrase_callback(
              value.get(), this,
              [](struct archive*, void* data) -> const char* {
                auto& reader = *static_cast<Reader*>(data);
                // These providers cannot decrypt; never offer a futile password loop.
                const auto format = archive_format(reader.value.get()) & ARCHIVE_FORMAT_BASE_MASK;
                if (format != ARCHIVE_FORMAT_7ZIP && format != ARCHIVE_FORMAT_RAR && format != ARCHIVE_FORMAT_RAR_V5) {
                  reader.work.progress.passwordNeeded.store(true);
                }
                return nullptr;
              }
          ),
          value.get(), work
      );
      if (!xz) {
        requireArchive(
            archive_read_set_seek_callback(
                value.get(),
                [](struct archive* a, void* data, la_int64_t offset, int whence) -> la_int64_t {
                  auto& reader = *static_cast<Reader*>(data);
                  try {
                    reader.work.check();
                    return reader.input.seek(offset, whence);
                  } catch (...) {
                    reader.work.failure = std::current_exception();
                    archive_set_error(a, EIO, "Archive seek failed");
                    return ARCHIVE_FATAL;
                  }
                }
            ),
            value.get(), work
        );
      }
      requireArchive(
          archive_read_open2(
              value.get(), this, nullptr,
              [](struct archive* a, void* data, const void** block) -> la_ssize_t {
                auto& reader = *static_cast<Reader*>(data);
                try {
                  reader.work.check();
                  const auto count = reader.xz ? reader.xz->read(reader.buffer) : reader.input.read(reader.buffer);
                  *block = reader.buffer.data();
                  return count;
                } catch (...) {
                  reader.work.failure = std::current_exception();
                  archive_set_error(a, EIO, "Archive read failed");
                  return -1;
                }
              },
              nullptr, nullptr
          ),
          value.get(), work
      );
    }
    void unchanged() {
      if (xz) {
        xz->finish();
      }
      input.unchanged();
    }
    bool next(struct archive_entry*& entry) {
      work.check();
      if (sevenzip) {
        if (memberPosition == sevenzip->size()) {
          return false;
        }
        const auto& member = sevenzip->at(memberPosition++);
        archive_entry_clear(nativeEntry.get());
        archive_entry_set_pathname(nativeEntry.get(), member.path.c_str());
        archive_entry_set_filetype(nativeEntry.get(), member.directory ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(nativeEntry.get(), static_cast<mode_t>(member.mode));
        archive_entry_set_size(nativeEntry.get(), static_cast<la_int64_t>(member.size));
        archive_entry_set_is_data_encrypted(nativeEntry.get(), member.encrypted);
        entry = nativeEntry.get();
        return true;
      }
      const auto before = archive_filter_bytes(value.get(), -1);
      const auto status = archive_read_next_header(value.get(), &entry);
      const auto format = archive_format(value.get()) & ARCHIVE_FORMAT_BASE_MASK;
      if (archive_read_has_encrypted_entries(value.get()) > 0) {
        if (format == ARCHIVE_FORMAT_7ZIP) {
          const Passwords empty;
          sevenzip = sevenZipCatalog(input, work, keys ? *keys : empty);
          return next(entry);
        }
        if (format == ARCHIVE_FORMAT_RAR || format == ARCHIVE_FORMAT_RAR_V5) {
          throwFileError(
              FileErrorKind::Unsupported, "Encrypted RAR archives are not supported by the archive provider."
          );
        }
      }
      if (status == ARCHIVE_EOF) {
        if (format == ARCHIVE_FORMAT_RAR_V5) {
          validateRar5End(input, before, work);
        }
        // Legacy RAR permits exact EOF without an end marker. Libarchive also
        // treats a partial next header as EOF; do not accept those leftover bytes.
        if (format == ARCHIVE_FORMAT_RAR
            && archive_filter_bytes(value.get(), -1) == before
            && input.seek(0, SEEK_END) != before) {
          throwFileError(FileErrorKind::InvalidArgument, "RAR archive has a truncated trailing header.");
        }
        return false;
      }
      requireArchive(status, value.get(), work);
      headerOffset = before;
      if (format == ARCHIVE_FORMAT_RAR && archive_entry_filetype(entry) == AE_IFLNK) {
        verifyRar4Stored(input, headerOffset, work);
      }
      return true;
    }
    void skip() {
      if (!sevenzip) {
        requireArchive(archive_read_data_skip(value.get()), value.get(), work);
      }
    }
  };
  void validateCompression(Reader& archive) {
    bool needsValidation = false;
    for (int i = 0; i < archive_filter_count(archive.value.get()); ++i) {
      const auto code = archive_filter_code(archive.value.get(), i);
      needsValidation = needsValidation || code == ARCHIVE_FILTER_BZIP2 || code == ARCHIVE_FILTER_ZSTD;
    }
    if (!needsValidation) {
      return;
    }
    auto& work = archive.work;
    // TAR may stop at its end markers before a filter checks its trailer. A raw
    // pass drains the framing with cancellation and a decoded-container budget.
    const auto budget = decodedContainerBudget(work.limits);
    Reader raw(Location::fromPath(archive.filename), work, nullptr, true);
    struct archive_entry* header = nullptr;
    if (!raw.next(header)) {
      throwFileError(FileErrorKind::InvalidArgument, "Compressed archive has no decoded container.");
    }
    std::uint64_t total = 0;
    for (;;) {
      work.check();
      const void* bytes = nullptr;
      std::size_t size = 0;
      la_int64_t offset = 0;
      const auto status = archive_read_data_block(raw.value.get(), &bytes, &size, &offset);
      if (status == ARCHIVE_EOF) {
        break;
      }
      requireArchive(status, raw.value.get(), work);
      if (offset < 0 || static_cast<std::uint64_t>(offset) != total || size > budget - total) {
        throwFileError(FileErrorKind::Unsupported, "Compressed archive exceeds its decoded-container limit.");
      }
      total += size;
    }
    raw.unchanged();
  }
  std::shared_ptr<ArchiveCatalog> catalog(Reader& reader) {
    auto result = std::make_shared<ArchiveCatalog>(
        ArchiveCatalog{Location::fromPath(reader.filename), reader.original, {}, {}, 0}
    );
    std::map<std::string, std::size_t> known;
    std::set<std::string> explicitNames;
    std::size_t metadata = 0;
    const auto initialExpanded = reader.work.expanded;
    const auto initialCount = reader.work.count;
    bool nativeCatalog = false;
    const auto add = [&](ArchiveEntry entry) {
      const auto bytes = entry.path.size() + entry.link.size() + entry.name.size();
      if (bytes > reader.work.limits.maxMetadataBytes - metadata
          || result->entries.size() >= reader.work.limits.maxEntries) {
        throwFileError(FileErrorKind::Unsupported, "Archive contents exceed the metadata limit.");
      }
      metadata += bytes;
      entry.displayName = filenameDisplayText(entry.name);
      known.emplace(entry.path, result->entries.size());
      result->entries.push_back(std::move(entry));
    };
    struct archive_entry* header = nullptr;
    while (reader.next(header)) {
      if (reader.nativeSevenZip() && !nativeCatalog) {
        // Libarchive can discover encryption after yielding earlier clear entries.
        // Validate the complete typed catalog once, without duplicate accounting.
        nativeCatalog = true;
        result->entries.clear();
        known.clear();
        explicitNames.clear();
        metadata = 0;
        reader.work.expanded = initialExpanded;
        reader.work.count = initialCount;
      }
      result->encrypted = result->encrypted || archive_entry_is_encrypted(header) > 0;
      reader.work.item();
      const auto path = safePath(bounded(archive_entry_pathname(header)), reader.work);
      if (path.empty()) {
        if (archive_entry_filetype(header) != AE_IFDIR) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive has an empty file name.");
        }
        reader.skip();
        continue;
      }
      ArchiveEntry entry{
          .path = path,
          .parent = parentName(path),
          .name = leaf(path),
          .displayName = {},
          .size = std::nullopt,
          .link = {}
      };
      entry.encrypted = archive_entry_is_data_encrypted(header) > 0;
      const auto type = archive_entry_filetype(header);
      const bool unsupportedType = type != AE_IFREG || archive_entry_hardlink(header);
      const bool hiddenLink = reader.nativeSevenZip() && type != AE_IFDIR && unsupportedType;
      if ((hiddenLink || (entry.encrypted && unsupportedType))
          && (archive_format(reader.value.get()) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_7ZIP) {
        throwFileError(FileErrorKind::Unsupported, "Encrypted 7z link metadata is not supported yet.");
      }
      if (const char* target = archive_entry_hardlink(header)) {
        entry.kind = ArchiveEntryKind::Hardlink;
        entry.link = safePath(bounded(target), reader.work);
        if (entry.link.empty()) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive has an invalid hard-link target.");
        }
      } else if (type == AE_IFREG) {
        entry.kind = ArchiveEntryKind::File;
      } else if (type == AE_IFDIR) {
        entry.kind = ArchiveEntryKind::Directory;
      } else if (type == AE_IFLNK) {
        entry.kind = ArchiveEntryKind::Symlink;
        entry.link = bounded(archive_entry_symlink(header));
        (void)linkTarget(path, entry.link, reader.work);
      } else {
        throwFileError(FileErrorKind::Unsupported, "Archive contains an unsupported special file.");
      }
      entry.mode = static_cast<unsigned>(archive_entry_perm(header)) & 0777U;
      if (entry.kind == ArchiveEntryKind::File && archive_entry_size_is_set(header)) {
        const auto size = archive_entry_size(header);
        if (size < 0) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive contains a negative file size.");
        }
        entry.size = static_cast<std::uint64_t>(size);
        reader.work.account(*entry.size);
        const auto extents = archive_entry_sparse_reset(header);
        if (extents > 0) {
          entry.dataBytes = 0;
          std::uint64_t end = 0;
          for (int i = 0; i < extents; ++i) {
            la_int64_t offset = 0, length = 0;
            if (archive_entry_sparse_next(header, &offset, &length) != ARCHIVE_OK
                || offset < 0
                || length < 0
                || static_cast<std::uint64_t>(offset) < end
                || static_cast<std::uint64_t>(offset) > *entry.size
                || static_cast<std::uint64_t>(length) > *entry.size - static_cast<std::uint64_t>(offset)) {
              throwFileError(FileErrorKind::InvalidArgument, "Archive contains invalid sparse file extents.");
            }
            *entry.dataBytes += static_cast<std::uint64_t>(length);
            end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(length);
          }
        }
      }
      if (explicitNames.contains(path) && entry.kind != ArchiveEntryKind::Directory) {
        throwFileError(FileErrorKind::InvalidArgument, "Archive contains duplicate file names.");
      }
      explicitNames.insert(path);
      auto components = parts(entry.parent);
      std::string prefix;
      for (const auto& component : components) {
        if (!prefix.empty()) {
          prefix += '/';
        }
        prefix += component;
        const auto found = known.find(prefix);
        if (found != known.end() && result->entries[found->second].kind != ArchiveEntryKind::Directory) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive entry traverses a file or link.");
        }
        if (found == known.end()) {
          add(
              {.path = prefix,
               .parent = parentName(prefix),
               .name = component,
               .displayName = {},
               .kind = ArchiveEntryKind::Directory,
               .size = std::nullopt,
               .link = {}}
          );
        }
      }
      if (const auto found = known.find(path); found != known.end()) {
        if (entry.kind != ArchiveEntryKind::Directory
            || result->entries[found->second].kind != ArchiveEntryKind::Directory) {
          throwFileError(FileErrorKind::InvalidArgument, "Archive entries have conflicting types.");
        }
        result->entries[found->second].mode = entry.mode;
      } else {
        add(std::move(entry));
      }
      reader.skip();
    }
    std::map<std::string, std::string> links;
    for (const auto& entry : result->entries) {
      if (entry.kind == ArchiveEntryKind::Symlink) {
        links.emplace(entry.path, entry.link);
      }
    }
    validateLinks(links, reader.work);
    result->encrypted = result->encrypted || reader.nativeSevenZip();
    result->expandedBytes = reader.work.expanded;
    result->format = archive_format_name(reader.value.get()) ? archive_format_name(reader.value.get()) : "Archive";
    validateCompression(reader);
    reader.unchanged();
    return result;
  }
  Fd directoryAt(int root, const std::string& path) {
    auto current = opened(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    for (const auto& name : parts(path)) {
      if (::mkdirat(current.get(), name.c_str(), 0700) != 0 && errno != EEXIST) {
        systemError("Could not create extracted folder");
      }
      current = opened(::openat(current.get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    }
    return current;
  }
  void extractData(Reader& reader, int output, std::uint64_t& total, std::optional<std::uint64_t> expected) {
    if ((archive_format(reader.value.get()) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_RAR) {
      verifyRar4Stored(reader.input, reader.headerOffset, reader.work);
    }
    std::uint64_t end = 0;
    for (;;) {
      reader.work.check();
      const void* data = nullptr;
      std::size_t size = 0;
      la_int64_t offset = 0;
      const auto status = archive_read_data_block(reader.value.get(), &data, &size, &offset);
      if (status == ARCHIVE_EOF) {
        break;
      }
      requireArchive(status, reader.value.get(), reader.work);
      if (offset < 0
          || static_cast<std::uint64_t>(offset) < end
          || size > static_cast<std::uint64_t>(INT64_MAX) - static_cast<std::uint64_t>(offset)
          || static_cast<std::uint64_t>(offset) > reader.work.limits.maxExpandedBytes
          || size > reader.work.limits.maxExpandedBytes - static_cast<std::uint64_t>(offset)) {
        throwFileError(FileErrorKind::Unsupported, "Archive data exceeds its output limit.");
      }
      const auto next = static_cast<std::uint64_t>(offset) + size;
      if (next - end > reader.work.limits.maxExpandedBytes - total) {
        throwFileError(FileErrorKind::Unsupported, "Archive exceeds its expanded-size limit.");
      }
      total += next - end;
      end = next;
      std::size_t done = 0;
      while (done < size) {
        const auto count = ::pwrite(
            output, static_cast<const char*>(data) + done, size - done, offset + static_cast<la_int64_t>(done)
        );
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          systemError("Could not write extracted data");
        }
        done += static_cast<std::size_t>(count);
      }
      reader.work.progress.bytes.store(total);
    }
    const auto length = expected.value_or(end);
    if (length < end
        || length > static_cast<std::uint64_t>(INT64_MAX)
        || length > reader.work.limits.maxExpandedBytes
        || length - end > reader.work.limits.maxExpandedBytes - total) {
      throwFileError(FileErrorKind::Unsupported, "Archive file size is inconsistent or exceeds the output limit.");
    }
    total += length - end;
    // Private extraction data uses buffered writes; the caller checks close before publication.
    if (::ftruncate(output, static_cast<off_t>(length)) != 0) {
      systemError("Could not finish extracted data");
    }
  }
  // One 7-Zip run unpacks every selected body; each produced file is then checked against
  // the validated catalog and moved, never followed through links, into the private data folder.
  void unpackSevenZip(
      Reader& reader, const std::vector<ArchiveEntry>& catalog, const std::set<std::string>& bodies,
      const std::map<std::string, std::size_t>& index, const std::string& stage, int data, const Passwords& passwords,
      std::uint64_t& total
  ) {
    auto& work = reader.work;
    if (bodies.empty()) {
      return;
    }
    std::vector<std::string> names;
    std::size_t nameBytes = 0;
    std::uint64_t largest = 0;
    bool encrypted = false;
    for (const auto& path : bodies) {
      const auto& entry = catalog[index.at(path)];
      if (!entry.size || *entry.size > work.limits.maxExpandedBytes - total) {
        throwFileError(FileErrorKind::Unsupported, "7z member size is unknown or exceeds the output limit.");
      }
      total += *entry.size;
      largest = std::max(largest, *entry.size);
      encrypted = encrypted || entry.encrypted;
      nameBytes += path.size() + 1;
      names.push_back(path);
    }
    // Huge selections would exceed argv limits, so the provider temporarily extracts the whole archive.
    if (nameBytes > 128U * 1024U) {
      names.clear();
      std::uint64_t temporaryBytes = 0;
      for (const auto& entry : catalog) {
        const auto bytes = entry.size.value_or(0);
        if (bytes > UINT64_MAX - temporaryBytes) {
          throwFileError(FileErrorKind::Unsupported, "Archive storage size exceeds the supported range.");
        }
        temporaryBytes += bytes;
        largest = std::max(largest, entry.size.value_or(0));
        encrypted = encrypted || entry.encrypted;
      }
      requireSpace(data, temporaryBytes);
    }
    const auto directory = (fs::path(stage) / ".7z").native();
    extractSevenZip(reader.input, work, passwords, names, encrypted, largest, directory);
    const auto missing = [] {
      throwFileError(FileErrorKind::InvalidArgument, "The 7-Zip command did not produce an expected member.");
    };
    const auto root = opened(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    for (const auto& path : bodies) {
      work.check();
      const auto& entry = catalog[index.at(path)];
      auto parent = opened(::fcntl(root.get(), F_DUPFD_CLOEXEC, 0));
      for (const auto& name : parts(entry.parent)) {
        Fd next(::openat(parent.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (next.get() < 0) {
          missing();
        }
        parent = std::move(next);
      }
      struct stat info{};
      if (::fstatat(parent.get(), entry.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0
          || !S_ISREG(info.st_mode)
          || static_cast<std::uint64_t>(info.st_size) != *entry.size) {
        missing();
      }
      if (::renameat(parent.get(), entry.name.c_str(), data, std::to_string(index.at(path)).c_str()) != 0) {
        systemError("Could not stage extracted 7z data");
      }
    }
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    work.progress.bytes.store(total);
  }
} // namespace

luciole::archive_work::Prepared luciole::archive_work::inspect(
    const Location& source, ArchiveLimits limits, const CancelToken& cancel, Progress& progress,
    const Passwords& passwords
) {
  const ArchiveLocale locale;
  auto work = workFor(limits, cancel, progress);
  Reader reader(source, work, &passwords);
  return {.stage = {}, .catalog = ::catalog(reader), .output = std::nullopt};
}
luciole::archive_work::Prepared luciole::archive_work::extract(
    const Location& source, const Location& destination, const std::vector<std::string>& selected,
    const std::shared_ptr<const ArchiveCatalog>& expected, bool open, ArchiveLimits limits, const CancelToken& cancel,
    Progress& progress, const Passwords& passwords
) {
  const ArchiveLocale locale;
  auto work = workFor(limits, cancel, progress);
  Reader scan(source, work, &passwords);
  auto contents = ::catalog(scan);
  if (expected && (expected->source != source || expected->stamp != contents->stamp)) {
    throwFileError(FileErrorKind::Changed, "The archive changed. Reload its contents before extracting.");
  }
  // Capture the container's decode budget before applying the smaller limit
  // to the selected member's staged output. Other TAR members still stream by.
  Reader reader(source, work, &passwords);
  reader.sevenzip = scan.sevenzip;
  if (reader.original != contents->stamp) {
    throwFileError(FileErrorKind::Changed, "Archive changed before extraction.");
  }
  if (open) {
    work.limits.maxExpandedBytes = std::min<std::uint64_t>(work.limits.maxExpandedBytes, 128U * 1024U * 1024U);
  }
  const auto destinationName = open ? "member" : destinationParts(destination).second;
  Stage stage;
  const auto containerPath = fs::path(stage.path) / "payload";
  if (::mkdir(containerPath.c_str(), 0700) != 0) {
    systemError("Could not prepare archive staging");
  }
  const auto payloadPath = containerPath / destinationName;
  const auto dataPath = fs::path(stage.path) / ".data";
  if (::mkdir(payloadPath.c_str(), 0700) != 0 || ::mkdir(dataPath.c_str(), 0700) != 0) {
    systemError("Could not prepare archive staging");
  }
  const auto payload = opened(::open(payloadPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  const auto data = opened(::open(dataPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  std::map<std::string, std::size_t> entries;
  for (std::size_t i = 0; i < contents->entries.size(); ++i) {
    entries.emplace(contents->entries[i].path, i);
  }
  std::set<std::string> wanted;
  const std::set<std::string> chosen(selected.begin(), selected.end());
  for (const auto& path : chosen) {
    if (!entries.contains(path)) {
      throwFileError(FileErrorKind::Changed, "An archive selection no longer exists.");
    }
  }
  for (const auto& entry : contents->entries) {
    work.check();
    bool include = chosen.empty() || chosen.contains(entry.path);
    for (auto parent = entry.parent; !include && !parent.empty(); parent = parentName(parent)) {
      include = chosen.contains(parent);
    }
    if (include) {
      wanted.insert(entry.path);
    }
  }
  if (open
      && (wanted.size() != 1
          || contents->entries[entries.at(*wanted.begin())].kind == ArchiveEntryKind::Directory
          || contents->entries[entries.at(*wanted.begin())].kind == ArchiveEntryKind::Symlink)) {
    throwFileError(FileErrorKind::InvalidArgument, "Select one regular archive member to open.");
  }
  const auto body = [&](const std::string& path) {
    std::set<std::string> seen;
    auto current = path;
    for (unsigned depth = 0; depth <= work.limits.maxDepth; ++depth) {
      work.check();
      if (!seen.insert(current).second || !entries.contains(current)) {
        throwFileError(FileErrorKind::InvalidArgument, "Archive has a missing or cyclic hard-link target.");
      }
      const auto& entry = contents->entries[entries.at(current)];
      if (entry.kind == ArchiveEntryKind::File) {
        return current;
      }
      if (entry.kind != ArchiveEntryKind::Hardlink) {
        throwFileError(FileErrorKind::InvalidArgument, "Archive hard link does not target a regular file.");
      }
      current = entry.link;
    }
    throwFileError(FileErrorKind::Unsupported, "Archive hard-link chain is too deep.");
  };
  std::set<std::string> bodies;
  for (const auto& path : wanted) {
    const auto& entry = contents->entries[entries.at(path)];
    if (entry.kind == ArchiveEntryKind::File || entry.kind == ArchiveEntryKind::Hardlink) {
      bodies.insert(body(path));
    }
  }
  const auto dataSize = [&](const std::string& path) {
    const auto& entry = contents->entries[entries.at(path)];
    return entry.dataBytes.value_or(entry.size.value_or(0));
  };
  const auto addSize = [](std::uint64_t& total, std::uint64_t bytes) {
    if (bytes > UINT64_MAX - total) {
      throwFileError(FileErrorKind::Unsupported, "Archive storage size exceeds the supported range.");
    }
    total += bytes;
  };
  std::uint64_t stagingBytes = 0;
  for (const auto& path : bodies) {
    addSize(stagingBytes, dataSize(path));
  }
  requireSpace(data.get(), stagingBytes);
  if (!open) {
    const auto parent = opened(::open(destinationParts(destination).first.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    auto destinationBytes = stagingBytes;
    if (statFd(parent.get()).st_dev != statFd(data.get()).st_dev) {
      // Cross-filesystem copies may expand holes and copy hard-link aliases independently.
      destinationBytes = 0;
      for (const auto& path : wanted) {
        const auto& entry = contents->entries[entries.at(path)];
        if (entry.kind == ArchiveEntryKind::File || entry.kind == ArchiveEntryKind::Hardlink) {
          addSize(destinationBytes, contents->entries[entries.at(body(path))].size.value_or(0));
        }
      }
    }
    requireSpace(parent.get(), destinationBytes);
  }
  std::uint64_t total = 0;
  std::map<std::string, std::uint64_t> bodySizes;
  const auto publish = [&](const std::string& path, int output) {
    const auto& entry = contents->entries[entries.at(path)];
    const auto mode = open ? 0600U : 0600U | (entry.mode & 0155U);
    if (::fchmod(output, static_cast<mode_t>(mode)) != 0) {
      systemError("Could not set extracted permissions");
    }
  };
  if (reader.nativeSevenZip()) {
    unpackSevenZip(reader, contents->entries, bodies, entries, stage.path, data.get(), passwords, total);
    for (const auto& path : bodies) {
      const auto& entry = contents->entries[entries.at(path)];
      const auto output =
          opened(::openat(data.get(), std::to_string(entries.at(path)).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
      bodySizes.emplace(path, *entry.size);
      publish(path, output.get());
    }
  } else {
    struct archive_entry* header = nullptr;
    while (reader.next(header)) {
      const auto path = safePath(bounded(archive_entry_pathname(header)), work);
      if (bodies.contains(path)) {
        const auto& entry = contents->entries[entries.at(path)];
        auto output = opened(
            ::openat(
                data.get(), std::to_string(entries.at(path)).c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600
            )
        );
        extractData(reader, output.get(), total, entry.size);
        bodySizes.emplace(path, static_cast<std::uint64_t>(statFd(output.get()).st_size));
        publish(path, output.get());
        if (output.close() != 0) {
          systemError("Could not close extracted data");
        }
      } else {
        reader.skip();
      }
    }
  }
  reader.unchanged();
  std::uint64_t logicalBytes = 0;
  for (const auto& path : wanted) {
    work.check();
    const auto& entry = contents->entries[entries.at(path)];
    if (entry.kind == ArchiveEntryKind::File || entry.kind == ArchiveEntryKind::Hardlink) {
      const auto bytes = bodySizes.at(body(path));
      if (bytes > work.limits.maxExpandedBytes - logicalBytes) {
        throwFileError(FileErrorKind::Unsupported, "Archive hard links exceed the expanded-size limit.");
      }
      logicalBytes += bytes;
    }
  }
  for (const auto& path : wanted) {
    work.check();
    const auto& entry = contents->entries[entries.at(path)];
    auto parent = directoryAt(payload.get(), entry.parent);
    if (entry.kind == ArchiveEntryKind::Directory) {
      auto folder = directoryAt(payload.get(), path);
      if (::fchmod(folder.get(), static_cast<mode_t>(0700U | (entry.mode & 0055U))) != 0) {
        systemError("Could not set folder permissions");
      }
    } else if (entry.kind == ArchiveEntryKind::File || entry.kind == ArchiveEntryKind::Hardlink) {
      const auto id = std::to_string(entries.at(body(path)));
      if (::linkat(data.get(), id.c_str(), parent.get(), entry.name.c_str(), 0) != 0) {
        systemError("Could not publish extracted file");
      }
    }
  }
  for (const auto& path : wanted) {
    const auto& entry = contents->entries[entries.at(path)];
    if (entry.kind == ArchiveEntryKind::Symlink) {
      (void)linkTarget(path, entry.link, work);
      auto parent = directoryAt(payload.get(), entry.parent);
      if (::symlinkat(entry.link.c_str(), parent.get(), entry.name.c_str()) != 0) {
        systemError("Could not create extracted link");
      }
    }
  }
  work.check();
  const auto output =
      open ? Location::fromPath((payloadPath / *wanted.begin()).native()) : Location::fromPath(payloadPath.native());
  return {.stage = stage.release(), .catalog = std::move(contents), .output = output, .expandedBytes = logicalBytes};
}
