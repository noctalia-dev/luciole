#include "archive_rar_fixtures.h"
#include "archive_sevenzip_fixtures.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "operations/archive_input.h"
#include "operations/archive_service.h"
#include "operations/archive_sevenzip.h"
#include "operations/archive_support.h"
#include "operations/archive_work.h"
#include "operations/archive_xz_input.h"
#include "test_check.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <chrono>
#include <clocale>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <gio/gio.h>
#include <map>
#include <print>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string pattern = "/tmp/luciole-archive-test-XXXXXX";
      const char* path = ::mkdtemp(pattern.data());
      TEST_CHECK(path);
      root = path;
      fs::create_directory(root / "tmp");
      ::setenv("TMPDIR", (root / "tmp").c_str(), 1);
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 20s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        auto start = m_source.addPollFds(fds);
        const int timeout = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  void write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    TEST_CHECK(out.good());
  }
  std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }
  struct Entry {
    std::string name, data, link;
    mode_t kind = AE_IFREG;
    mode_t mode = 0644;
  };
  void
  makeArchive(const fs::path& path, const std::vector<Entry>& entries, int format, int filter = ARCHIVE_FILTER_NONE) {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format(writer.get(), format) == ARCHIVE_OK);
    TEST_CHECK(archive_write_add_filter(writer.get(), filter) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    for (const auto& item : entries) {
      const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), &archive_entry_free
      );
      archive_entry_set_pathname(entry.get(), item.name.c_str());
      archive_entry_set_filetype(entry.get(), item.kind);
      archive_entry_set_perm(entry.get(), item.mode);
      archive_entry_set_size(
          entry.get(), item.kind == AE_IFREG && item.link.empty() ? static_cast<la_int64_t>(item.data.size()) : 0
      );
      if (!item.link.empty()) {
        if (item.kind == AE_IFLNK) {
          archive_entry_set_symlink(entry.get(), item.link.c_str());
        } else {
          archive_entry_set_hardlink(entry.get(), item.link.c_str());
        }
      }
      TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      if (item.kind == AE_IFREG && item.link.empty() && !item.data.empty()) {
        TEST_CHECK(
            archive_write_data(writer.get(), item.data.data(), item.data.size())
            == static_cast<la_ssize_t>(item.data.size())
        );
      }
    }
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }
  void tar(const fs::path& path, const std::vector<Entry>& entries, int filter = ARCHIVE_FILTER_NONE) {
    makeArchive(path, entries, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, filter);
  }
  void wait(luciole::ArchiveService& service, Pump& pump) {
    pump.until([&] { return !service.busy(); });
  }
  void testRoundTrips(const Fixture& fixture, Pump& pump) {
    const auto src = fixture.root / "source";
    fs::create_directories(src / "empty");
    write(src / "hello.txt", "archive content");
    write(src / "café\nnotes.txt", "Unicode and newline");
    fs::create_symlink("hello.txt", src / "link");
    TEST_CHECK(!luciole::archiveFormats().empty());
    for (const auto& format : luciole::archiveFormats()) {
      const auto archive = fixture.root / ("roundtrip" + std::string(format.suffix));
      luciole::ArchiveService service;
      TEST_CHECK(service.compress({location(src)}, location(archive), format.format));
      wait(service, pump);
      if (service.error()) {
        throw std::runtime_error(service.error()->message);
      }
      TEST_CHECK(!service.error() && fs::exists(archive));
      TEST_CHECK(service.inspect(location(archive)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog() && service.catalog()->entries.size() >= 5);
      const auto dest = fixture.root / ("unpacked" + std::to_string(static_cast<int>(format.format)));
      TEST_CHECK(service.extract(location(archive), location(dest)));
      wait(service, pump);
      TEST_CHECK(
          !service.error()
          && read(dest / "source/hello.txt") == "archive content"
          && read(dest / "source/café\nnotes.txt") == "Unicode and newline"
          && fs::is_directory(dest / "source/empty")
      );
      TEST_CHECK(fs::is_symlink(dest / "source/link") && fs::read_symlink(dest / "source/link") == "hello.txt");
      TEST_CHECK(service.openMember("source/hello.txt"));
      wait(service, pump);
      const auto opened = service.takeOpenedFile();
      TEST_CHECK(opened && read(*opened->nativePath()) == "archive content");
      const auto temporary = *opened->nativePath();
      service.shutdown();
      wait(service, pump);
      TEST_CHECK(!fs::exists(temporary));
    }
  }
  void testLiteralSourceLinks(const Fixture& fixture, Pump& pump) {
    struct ReadLocale {
      locale_t value = ::newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
      locale_t previous = nullptr;
      ReadLocale() {
        TEST_CHECK(value);
        previous = ::uselocale(value);
        TEST_CHECK(previous);
      }
      ~ReadLocale() {
        ::uselocale(previous);
        ::freelocale(value);
      }
      ReadLocale(const ReadLocale&) = delete;
      ReadLocale& operator=(const ReadLocale&) = delete;
    } readLocale;
    const auto source = fixture.root / "literal-links", outside = fixture.root / "outside-links";
    fs::create_directory(source);
    fs::create_directory(outside);
    const std::string payload = "selected source bytes", secret(8192, 'S');
    write(source / "payload", payload);
    write(outside / "canary", secret);
    TEST_CHECK(::mkfifo((outside / "pipe").c_str(), 0600) == 0);
    const std::map<std::string, std::string> targets{
        {"absolute-file", (outside / "canary").native()},
        {"absolute-folder", outside.native()},
        {"parent-target", "../outside-links/canary"},
        {"missing-target", (outside / "missing").native()},
        {"chain", "absolute-file"},
        {"cycle-a", "cycle-b"},
        {"cycle-b", "cycle-a"},
        {"literal-target", "../literal \\ ' $() café\n文"}
    };
    for (const auto& [name, target] : targets) {
      fs::create_symlink(target, source / name);
    }
    for (const auto& format : luciole::archiveFormats()) {
      const auto path = fixture.root / ("literal-source" + std::string(format.suffix));
      luciole::ArchiveLimits limits;
      // Following either the outside file or directory would violate this bound or encounter the FIFO.
      limits.maxExpandedBytes = payload.size();
      luciole::ArchiveService service(limits);
      TEST_CHECK(service.compress({location(source)}, location(path), format.format));
      wait(service, pump);
      if (service.error()) {
        throw std::runtime_error(service.error()->message);
      }

      // Read the actual archive independently of Luciole's restrictive browsing/extraction catalog.
      const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(
          archive_read_new(), &archive_read_free
      );
      TEST_CHECK(archive_read_support_filter_all(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_support_format_all(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_open_filename(reader.get(), path.c_str(), 65536) == ARCHIVE_OK);
      std::map<std::string, std::string> stored;
      std::size_t entries = 0;
      struct archive_entry* item = nullptr;
      int status = ARCHIVE_OK;
      while ((status = archive_read_next_header(reader.get(), &item)) == ARCHIVE_OK) {
        ++entries;
        const std::string name(archive_entry_pathname(item));
        if (archive_entry_filetype(item) == AE_IFLNK) {
          TEST_CHECK(archive_entry_symlink(item));
          TEST_CHECK(stored.emplace(fs::path(name).filename().native(), archive_entry_symlink(item)).second);
        } else if (archive_entry_filetype(item) == AE_IFREG) {
          TEST_CHECK(name == "literal-links/payload");
          std::array<char, 64> data{};
          TEST_CHECK(
              archive_read_data(reader.get(), data.data(), data.size()) == static_cast<la_ssize_t>(payload.size())
          );
          TEST_CHECK(std::string(data.data(), payload.size()) == payload);
          TEST_CHECK(archive_read_data(reader.get(), data.data(), data.size()) == 0);
        } else {
          TEST_CHECK(archive_entry_filetype(item) == AE_IFDIR && (name == "literal-links/" || name == "literal-links"));
        }
      }
      if (status != ARCHIVE_EOF || stored != targets || entries != targets.size() + 2) {
        std::println(
            stderr, "literal-link format {}: status {}, entries {}, error {}", format.label, status, entries,
            archive_error_string(reader.get()) ? archive_error_string(reader.get()) : "none"
        );
        for (const auto& [name, target] : stored) {
          std::println(stderr, "stored {} -> {}", name, target);
        }
      }
      TEST_CHECK(status == ARCHIVE_EOF && stored == targets && entries == targets.size() + 2);
      TEST_CHECK(archive_read_close(reader.get()) == ARCHIVE_OK);
      const auto bytes = read(path);
      luciole::ArchiveService guarded;
      TEST_CHECK(guarded.inspect(location(path)));
      wait(guarded, pump);
      TEST_CHECK(
          guarded.error() && guarded.error()->kind == luciole::FileErrorKind::InvalidArgument && !guarded.catalog()
      );
      const auto destination = fixture.root / ("literal-rejected" + std::to_string(static_cast<int>(format.format)));
      TEST_CHECK(guarded.extract(location(path), location(destination)));
      wait(guarded, pump);
      TEST_CHECK(
          guarded.error()
          && guarded.error()->kind == luciole::FileErrorKind::InvalidArgument
          && !fs::exists(destination)
      );
      TEST_CHECK(read(path) == bytes && read(outside / "canary") == secret);
      for (const auto& [name, target] : targets) {
        TEST_CHECK(fs::is_symlink(source / name) && fs::read_symlink(source / name).native() == target);
      }
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
    const auto longLink = fixture.root / "long-link";
    fs::create_symlink(std::string(128, 'x'), longLink);
    luciole::ArchiveLimits limits;
    limits.maxMetadataBytes = 64;
    luciole::ArchiveService limited(limits);
    const auto output = fixture.root / "limited-link.zip";
    TEST_CHECK(limited.compress({location(longLink)}, location(output), luciole::ArchiveFormat::Zip));
    wait(limited, pump);
    TEST_CHECK(limited.error() && !fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
  }
  void testConflicts(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "conflict.txt", archive = fixture.root / "conflict.zip";
    write(source, "first");
    luciole::ArchiveService service;
    TEST_CHECK(service.compress({location(source)}, location(archive), luciole::ArchiveFormat::Zip));
    wait(service, pump);
    const auto original = read(archive);
    write(source, "second content");
    for (const auto choice :
         {luciole::TransferConflictChoice::Skip, luciole::TransferConflictChoice::KeepBoth,
          luciole::TransferConflictChoice::Replace}) {
      TEST_CHECK(service.compress({location(source)}, location(archive), luciole::ArchiveFormat::Zip));
      pump.until([&] { return service.conflict().has_value(); });
      const auto conflict = service.conflict();
      TEST_CHECK(service.resolve(conflict->id, choice));
      wait(service, pump);
      if (choice != luciole::TransferConflictChoice::Replace) {
        TEST_CHECK(read(archive) == original);
      } else {
        TEST_CHECK(!service.error() && read(archive) != original);
      }
    }
    TEST_CHECK(fs::exists(fixture.root / "conflict (1).zip"));
  }
  void testHostile(const Fixture& fixture, Pump& pump) {
    const auto outside = fixture.root / "hostile-outside";
    fs::create_directory(outside);
    write(outside / "canary", "unchanged outside bytes");
    const std::vector<std::vector<Entry>> cases{
        {{"../escaped", "bad", {}}},
        {{"/absolute", "bad", {}}},
        {{"C:/absolute", "bad", {}}},
        {{"..\\escaped", "bad", {}}},
        {{"link", {}, "../outside", AE_IFLNK}},
        {{"link", {}, "/outside", AE_IFLNK}},
        {{"redirect", {}, ".", AE_IFLNK}, {"escape", {}, "redirect/../outside", AE_IFLNK}},
        {{"dir", {}, "target", AE_IFLNK}, {"dir/file", "bad", {}}},
        {{"dir/file", "bad", {}}, {"dir", {}, "target", AE_IFLNK}},
        {{"same", "one", {}}, {"same", "two", {}}},
        {{"pipe", {}, {}, AE_IFIFO}},
        {{"hard", {}, "../outside", AE_IFREG}},
        {{"redirect", {}, outside.native(), AE_IFLNK}, {"redirect/canary", "overwrite attempt", {}}},
        {{"cycle-a", {}, "cycle-b", AE_IFLNK}, {"cycle-b", {}, "cycle-a", AE_IFLNK}},
        {{"hard", {}, (outside / "canary").native(), AE_IFREG}}
    };
    std::size_t index = 0;
    for (const auto& entries : cases) {
      const auto source = fixture.root / ("hostile" + std::to_string(index) + ".tar"),
                 dest = fixture.root / ("rejected" + std::to_string(index++));
      tar(source, entries);
      luciole::ArchiveService service;
      TEST_CHECK(service.extract(location(source), location(dest)));
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(dest));
      TEST_CHECK(read(outside / "canary") == "unchanged outside bytes");
    }
    TEST_CHECK(!fs::exists(fixture.root / "escaped"));
  }
  void testNativeJobCapacity(const Fixture& fixture) {
    using namespace luciole::archive_work;
    Progress progress;
    const luciole::CancelToken cancel;
    auto work = workFor({}, cancel, progress);
    TEST_CHECK(work.deadline == Clock::time_point::max());
    TEST_CHECK(decodedContainerBudget(work.limits) == UINT64_MAX);
    work.check();
    const auto folder = opened(::open(fixture.root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    requireSpace(folder.get(), 0);
    bool noSpace = false;
    try {
      requireSpace(folder.get(), UINT64_MAX);
    } catch (const luciole::FileErrorException& error) {
      noSpace = error.error().kind == luciole::FileErrorKind::NoSpace;
    }
    TEST_CHECK(noSpace);
    work.account(UINT64_MAX);
    bool overflow = false;
    try {
      work.account(1);
    } catch (const luciole::FileErrorException&) {
      overflow = true;
    }
    TEST_CHECK(overflow);

    // A valid TAR can have more than 4 GiB of trailing zero padding without allocating it.
    const auto source = fixture.root / "large-input.tar";
    tar(source, {{"payload", "large archive payload", {}}});
    fs::resize_file(source, 4ULL * 1024 * 1024 * 1024 + 4096);
    auto known = inspect(location(source), {}, cancel, progress);
    TEST_CHECK(known.catalog && known.catalog->entries.size() == 1);
    auto extracted = extract(
        location(source), location(fixture.root / "large-input"), {}, known.catalog, false, {}, cancel, progress
    );
    TEST_CHECK(read(fs::path(*extracted.output->nativePath()) / "payload") == "large archive payload");
    TEST_CHECK(!cleanup({extracted.stage}) && fs::is_empty(fixture.root / "tmp"));
    fs::remove(source);
  }

  int makeLargeSparseFixture(const fs::path& source) {
    std::signal(SIGXFSZ, SIG_IGN);
    rlimit limit{};
    TEST_CHECK(::getrlimit(RLIMIT_FSIZE, &limit) == 0);
    limit.rlim_cur = std::min<rlim_t>(limit.rlim_max, 1024U * 1024U);
    TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
    constexpr std::uint64_t size = 9ULL * 1024 * 1024 * 1024;
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), source.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), "sparse");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
    archive_entry_sparse_add_entry(entry.get(), 0, 4);
    archive_entry_sparse_add_entry(entry.get(), static_cast<la_int64_t>(size - 4), 4);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_data(writer.get(), "HEAD", 4) == 4);
    const std::vector<char> zeros(1024U * 1024U);
    for (std::uint64_t remaining = size - 8; remaining;) {
      const auto bytes = std::min<std::uint64_t>(remaining, zeros.size());
      TEST_CHECK(archive_write_data(writer.get(), zeros.data(), bytes) == static_cast<la_ssize_t>(bytes));
      remaining -= bytes;
    }
    TEST_CHECK(archive_write_data(writer.get(), "TAIL", 4) == 4);
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(fs::file_size(source) < 65536);
    return 0;
  }

  void testLargeSparseMember(const Fixture& fixture) {
    const auto source = fixture.root / "large-sparse.tar";
    constexpr std::uint64_t size = 9ULL * 1024 * 1024 * 1024;
    const luciole::GioPtr<GSubprocess> encoder(g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, nullptr, "/proc/self/exe", "--large-sparse-fixture", source.c_str(), nullptr
    ));
    TEST_CHECK(encoder && g_subprocess_wait_check(encoder.get(), nullptr, nullptr));
    luciole::archive_work::Progress progress;
    const luciole::CancelToken cancel;
    const auto known = luciole::archive_work::inspect(location(source), {}, cancel, progress);
    TEST_CHECK(known.catalog->entries[0].size == size && known.catalog->entries[0].dataBytes == 8);
    auto result = luciole::archive_work::extract(
        location(source), location(fixture.root / "large-sparse"), {}, known.catalog, false, {}, cancel, progress
    );
    const auto path = fs::path(*result.output->nativePath()) / "sparse";
    const auto data = luciole::checkedFd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    const auto facts = luciole::statFd(data.get());
    TEST_CHECK(static_cast<std::uint64_t>(facts.st_size) == size && facts.st_blocks * 512 < 65536);
    std::array<char, 4> bytes{};
    TEST_CHECK(::pread(data.get(), bytes.data(), bytes.size(), 0) == 4 && std::string_view(bytes.data(), 4) == "HEAD");
    TEST_CHECK(
        ::pread(data.get(), bytes.data(), bytes.size(), static_cast<off_t>(size - 4)) == 4
        && std::string_view(bytes.data(), 4) == "TAIL"
    );
    TEST_CHECK(!luciole::archive_work::cleanup({result.stage}) && fs::is_empty(fixture.root / "tmp"));
  }

  void testLargeCatalogue(const Fixture& fixture) {
    std::vector<Entry> entries;
    entries.reserve(50002);
    for (unsigned i = 0; i < 50001; ++i) {
      entries.push_back({"many/" + std::to_string(i) + std::string(180, 'n'), {}, {}});
    }
    std::string deep;
    for (unsigned i = 0; i < 140; ++i) {
      deep += "d/";
    }
    deep += "payload";
    entries.push_back({deep, "deep archive payload", {}});
    const auto source = fixture.root / "large-catalogue.tar";
    tar(source, entries);
    luciole::archive_work::Progress progress;
    const luciole::CancelToken cancel;
    auto known = luciole::archive_work::inspect(location(source), {}, cancel, progress);
    TEST_CHECK(known.catalog->entries.size() > 50000);
    std::size_t metadata = 0;
    for (const auto& item : known.catalog->entries) {
      metadata += item.path.size() + item.name.size() + item.link.size();
    }
    TEST_CHECK(metadata > 16U * 1024U * 1024U);
    const auto output = fixture.root / "large-catalogue";
    auto result = luciole::archive_work::extract(
        location(source), location(output), {}, known.catalog, false, {}, cancel, progress
    );
    const auto root = fs::path(*result.output->nativePath());
    TEST_CHECK(fs::file_size(root / entries.front().name) == 0 && read(root / deep) == "deep archive payload");
    luciole::archive_work::Progress createdProgress;
    auto created = luciole::archive_work::compress(
        {location(root / "many"), location(root / "d")}, location(fixture.root / "large-created.zip"),
        luciole::ArchiveFormat::Zip, {}, cancel, createdProgress
    );
    const auto recreated = luciole::archive_work::inspect(*created.output, {}, cancel, createdProgress);
    TEST_CHECK(recreated.catalog->entries.size() > 50000);
    TEST_CHECK(std::ranges::any_of(recreated.catalog->entries, [&](const auto& item) { return item.path == deep; }));
    TEST_CHECK(!luciole::archive_work::cleanup({created.stage}));
    TEST_CHECK(!luciole::archive_work::cleanup({result.stage}) && fs::is_empty(fixture.root / "tmp"));
    fs::remove(source);
  }

  void testLimitsAndTruncation(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "limits.tar";
    tar(source, {{"a/b/file", std::string(4096, 'x'), {}}});
    std::vector<luciole::ArchiveLimits> limits(5);
    limits[0].maxExpandedBytes = 100;
    limits[1].maxEntries = 1;
    limits[2].maxDepth = 1;
    limits[3].maxMetadataBytes = 3;
    limits[4].maxArchiveBytes = 8;
    for (std::size_t i = 0; i < limits.size(); ++i) {
      luciole::ArchiveService service(limits[i]);
      const auto dest = fixture.root / ("limited" + std::to_string(i));
      TEST_CHECK(service.extract(location(source), location(dest)));
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(dest));
    }
    const auto truncated = fixture.root / "truncated.tar";
    fs::copy_file(source, truncated);
    fs::resize_file(truncated, 515);
    luciole::ArchiveService service;
    const auto dest = fixture.root / "truncated-output";
    TEST_CHECK(service.extract(location(truncated), location(dest)));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(dest));
  }
  void testLinksAndSelection(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "links.tar";
    tar(source,
        {{"dir/real", "linked data", {}, AE_IFREG, 04777},
         {"alias", {}, "dir/real", AE_IFREG},
         {"safe", {}, "dir/../dir/real", AE_IFLNK}});
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    wait(service, pump);
    TEST_CHECK(!service.error());
    const auto dest = fixture.root / "selected";
    TEST_CHECK(service.extract(location(source), location(dest), {"alias"}));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(dest / "alias") == "linked data" && !fs::exists(dest / "dir/real"));
    struct stat info{};
    TEST_CHECK(
        ::stat((dest / "alias").c_str(), &info) == 0 && (info.st_mode & S_ISUID) == 0 && info.st_uid == ::getuid()
    );
    write(source, read(source) + "changed");
    TEST_CHECK(service.extract(location(source), location(fixture.root / "changed"), {"alias"}));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "changed"));
    TEST_CHECK(service.extract(location(source), location(fixture.root / "changed-all")));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "changed-all"));
  }
  void testHardlinkOutputLimit(const Fixture& fixture, Pump& pump) {
    const auto path = fixture.root / "link-limit.tar";
    tar(path, {{"body", "12345", {}}, {"alias", {}, "body"}});
    luciole::ArchiveLimits limits;
    limits.maxExpandedBytes = 9;
    luciole::ArchiveService service(limits);
    TEST_CHECK(service.extract(location(path), location(fixture.root / "link-limit")));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "link-limit"));
  }
  void
  encryptedZip(const fs::path& path, std::string_view encryption = "aes256", const std::string& name = "secret.txt") {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(
        archive_write_set_options(writer.get(), ("zip:encryption=" + std::string(encryption)).c_str()) == ARCHIVE_OK
    );
    TEST_CHECK(archive_write_set_passphrase(writer.get(), "test password") == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), name.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0600);
    archive_entry_set_size(entry.get(), 6);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_data(writer.get(), "secret", 6) == 6);
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }
  luciole::ArchivePasswordRequest passwordRequest(luciole::ArchiveService& service, Pump& pump) {
    pump.until([&] { return service.passwordRequest().has_value() || !service.busy(); });
    TEST_CHECK(service.busy() && service.phase() == luciole::ArchivePhase::WaitingPassword && !service.error());
    TEST_CHECK(service.passwordRequest());
    return *service.passwordRequest();
  }
  void testEncryptedRead(const Fixture& fixture, Pump& pump) {
    for (const std::string encryption : {"aes256", "zipcrypt"}) {
      const auto path = fixture.root / (encryption + ".zip");
      encryptedZip(path, encryption);
      luciole::ArchiveService service;
      TEST_CHECK(service.inspect(location(path)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog() && service.catalog()->encrypted);
      TEST_CHECK(service.catalog()->entries.size() == 1 && !service.passwordRequest());
      TEST_CHECK(service.openMember("secret.txt"));
      const auto first = passwordRequest(service, pump);
      TEST_CHECK(first.source == location(path) && !first.retry);
      TEST_CHECK(!service.providePassword(first.id + 1, "test password"));
      TEST_CHECK(!service.providePassword(first.id, ""));
      TEST_CHECK(!service.providePassword(first.id, std::string(1025, 'x')));
      TEST_CHECK(!service.providePassword(first.id, std::string("x\0y", 3)));
      TEST_CHECK(!service.inspect(location(path)));
      TEST_CHECK(service.providePassword(first.id, "wrong password"));
      const auto retry = passwordRequest(service, pump);
      TEST_CHECK(retry.id != first.id && retry.retry);
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
      TEST_CHECK(!service.providePassword(first.id, "test password"));
      TEST_CHECK(service.providePassword(retry.id, "test password"));
      wait(service, pump);
      TEST_CHECK(!service.error() && !service.passwordRequest());
      const auto opened = service.takeOpenedFile();
      TEST_CHECK(opened && read(*opened->nativePath()) == "secret");
      struct stat mode{};
      TEST_CHECK(::stat(opened->nativePath()->c_str(), &mode) == 0 && (mode.st_mode & 0111) == 0);
      service.shutdown();
      wait(service, pump);
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
  }
  void testEncryptedCreation(const Fixture& fixture, Pump& pump) {
    TEST_CHECK(luciole::archiveEncryptionAvailable());
    const auto source = fixture.root / "encrypted-source.txt";
    const auto output = fixture.root / "created-encrypted.zip";
    write(source, "encrypted payload");
    luciole::ArchiveService service;
    TEST_CHECK(!service.compress({location(source)}, location(output), luciole::ArchiveFormat::Tar, "test password"));
    TEST_CHECK(
        !service.compress({location(source)}, location(output), luciole::ArchiveFormat::Zip, std::string(1025, 'x'))
    );
    TEST_CHECK(!fs::exists(output));
    TEST_CHECK(service.compress({location(source)}, location(output), luciole::ArchiveFormat::Zip, "test password"));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(source) == "encrypted payload");
    // Read independently through the system provider; the writer must not emit plaintext ZIP data.
    const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
    TEST_CHECK(archive_read_support_format_zip(reader.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_read_open_filename(reader.get(), output.c_str(), 65536) == ARCHIVE_OK);
    struct archive_entry* entry = nullptr;
    TEST_CHECK(archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK && archive_entry_is_encrypted(entry) > 0);
    char bytes[32]{};
    TEST_CHECK(archive_read_data(reader.get(), bytes, sizeof(bytes)) < 0);
    const auto destination = fixture.root / "decrypted";
    TEST_CHECK(service.extract(location(output), location(destination)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "test password"));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(destination / source.filename()) == "encrypted payload");
    const auto file = fixture.root / "not-a-folder";
    write(file, "untouched");
    TEST_CHECK(service.compress({location(file)}, location(file / "failure.tar"), luciole::ArchiveFormat::Tar));
    wait(service, pump);
    TEST_CHECK(service.error() && read(file) == "untouched");
  }
  void testPasswordCancellationAndChanges(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "cancel-encrypted.zip";
    encryptedZip(source);
    const auto output = fixture.root / "cancel-encrypted-output";
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp") && !fs::exists(output));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(
        service.error() && service.error()->kind == luciole::FileErrorKind::Cancelled && !service.passwordRequest()
    );
    TEST_CHECK(!service.providePassword(request.id, "test password"));
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto next = passwordRequest(service, pump);
    TEST_CHECK(next.id != request.id && !next.retry);
    const auto replacement = fixture.root / "replacement-encrypted.zip";
    encryptedZip(replacement);
    fs::rename(replacement, source);
    TEST_CHECK(service.providePassword(next.id, "test password"));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Changed);
    TEST_CHECK(!fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto cancelAfterAnswer = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(cancelAfterAnswer.id, "test password"));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
    {
      auto owner = std::make_unique<luciole::ArchiveService>();
      TEST_CHECK(owner->extract(location(source), location(output)));
      (void)passwordRequest(*owner, pump);
      owner.reset();
    }
    TEST_CHECK(!fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
  }
  void testEncryptedBoundaries(const Fixture& fixture, Pump& pump) {
    const auto hostile = fixture.root / "encrypted-hostile.zip";
    encryptedZip(hostile, "aes256", "../escape");
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(hostile), location(fixture.root / "hostile-output")));
    wait(service, pump);
    TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(fixture.root / "escape"));
    const auto source = fixture.root / "encrypted-limited.zip";
    encryptedZip(source);
    luciole::ArchiveLimits limits;
    limits.maxExpandedBytes = 1;
    luciole::ArchiveService limited(limits);
    TEST_CHECK(limited.extract(location(source), location(fixture.root / "limited-output")));
    wait(limited, pump);
    TEST_CHECK(limited.error() && !limited.passwordRequest() && !fs::exists(fixture.root / "limited-output"));
    const auto output = fixture.root / "encrypted-conflict";
    fs::create_directory(output);
    write(output / "existing", "preserved");
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "test password"));
    pump.until([&] { return service.conflict().has_value() || !service.busy(); });
    TEST_CHECK(service.conflict());
    TEST_CHECK(service.resolve(service.conflict()->id, luciole::TransferConflictChoice::Cancel));
    wait(service, pump);
    TEST_CHECK(read(output / "existing") == "preserved" && !fs::exists(output / "secret.txt"));
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testEncryptedCorruption(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "corrupt-encrypted.zip";
    encryptedZip(source);
    auto bytes = read(source);
    const auto u16 = [&](std::size_t offset) {
      return static_cast<unsigned char>(bytes.at(offset)) | (static_cast<unsigned char>(bytes.at(offset + 1)) << 8);
    };
    const std::size_t data = 30 + u16(26) + u16(28);
    // AES-256's 16-byte salt and 2-byte password verifier precede ciphertext.
    bytes.at(data + 18) = static_cast<char>(static_cast<unsigned char>(bytes.at(data + 18)) ^ 1U);
    write(source, bytes);
    const auto output = fixture.root / "corrupt-encrypted-output";
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "test password"));
    wait(service, pump);
    TEST_CHECK(service.error() && !service.passwordRequest());
    TEST_CHECK(!fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
  }
  std::vector<fs::path>
  splitZip(const fs::path& folder, const std::string& name, const std::string& bytes, std::size_t chunk) {
    fs::create_directories(folder);
    std::vector<fs::path> result;
    for (std::size_t offset = 0; offset < bytes.size(); offset += chunk) {
      const auto path = folder / (name + std::format(".{:03}", result.size() + 1));
      result.push_back(path);
    }
    // Discovery must use volume numbers, independently of directory insertion order.
    for (std::size_t i = result.size(); i > 0; --i) {
      write(result[i - 1], bytes.substr((i - 1) * chunk, chunk));
    }
    return result;
  }
  template <typename Action> void rejects(Action action) {
    bool failed = false;
    try {
      action();
    } catch (const luciole::FileErrorException&) {
      failed = true;
    }
    TEST_CHECK(failed);
  }
  void testSplitInput(const Fixture& fixture) {
    std::string bytes;
    for (std::size_t i = 0; i < 131072; ++i) {
      bytes += static_cast<char>(i % 251);
    }
    const auto parts = splitZip(fixture.root / "seek-parts", "virtual.zip", bytes, 1024);
    luciole::archive_work::Progress progress;
    luciole::CancelToken cancel;
    auto work = luciole::archive_work::workFor({}, cancel, progress);
    luciole::archive_work::Input input(location(parts.front()), work);
    TEST_CHECK(input.split() && !input.fingerprint().empty());
    std::array<char, 65536> buffer{};
    TEST_CHECK(input.seek(4093, SEEK_SET) == 4093);
    TEST_CHECK(input.read(buffer) == buffer.size());
    TEST_CHECK(std::string_view(buffer.data(), buffer.size()) == std::string_view(bytes).substr(4093, buffer.size()));
    TEST_CHECK(input.seek(-4, SEEK_CUR) == 4093 + 65536 - 4);
    TEST_CHECK(input.seek(-7, SEEK_END) == static_cast<std::int64_t>(bytes.size() - 7));
    TEST_CHECK(
        input.read(buffer) == 7
        && std::string_view(buffer.data(), 7) == std::string_view(bytes).substr(bytes.size() - 7)
    );
    TEST_CHECK(input.read(buffer) == 0);
    rejects([&] { (void)input.seek(INT64_MIN, SEEK_END); });
    TEST_CHECK(input.seek(INT64_MAX, SEEK_SET) == INT64_MAX);
    rejects([&] { (void)input.seek(1, SEEK_CUR); });
    rejects([&] { (void)input.seek(0, -99); });
    TEST_CHECK(input.seek(0, SEEK_SET) == 0);
    input.unchanged();
    write(parts[1], "replacement");
    rejects([&] { input.unchanged(); });
    cancel.cancel();
    rejects([&] { (void)input.read(buffer); });
  }
  void testSplitZip(const Fixture& fixture, Pump& pump) {
    const auto plain = fixture.root / "split-source.txt";
    write(plain, "payload spanning split ZIP parts");
    const auto archive = fixture.root / "before-splitting.zip";
    luciole::ArchiveService service;
    TEST_CHECK(service.compress({location(plain)}, location(archive), luciole::ArchiveFormat::Zip));
    wait(service, pump);
    TEST_CHECK(!service.error());
    if (::geteuid() != 0) {
      const auto restricted = fixture.root / "unlistable-parent";
      fs::create_directory(restricted);
      fs::copy_file(archive, restricted / "known.zip");
      TEST_CHECK(::chmod(restricted.c_str(), 0111) == 0);
      TEST_CHECK(service.inspect(location(restricted / "known.zip")));
      wait(service, pump);
      TEST_CHECK(!service.error());
      TEST_CHECK(::chmod(restricted.c_str(), 0700) == 0);
    }
    const auto parts = splitZip(fixture.root / "zip-parts", "café.ZIP", read(archive), 17);
    TEST_CHECK(parts.size() > 2 && luciole::isSupportedArchive(location(parts[0]), {}));
    TEST_CHECK(!luciole::isSupportedArchive(location(parts[1]), "application/zip"));
    TEST_CHECK(luciole::archiveFolderName(location(parts[0])) == "café");
    TEST_CHECK(service.inspect(location(parts.front())));
    wait(service, pump);
    TEST_CHECK(!service.error() && service.catalog() && service.catalog()->source == location(parts.front()));
    const auto known = service.catalog();
    const auto output = fixture.root / "split-output";
    TEST_CHECK(service.extract(location(parts.front()), location(output), {plain.filename().native()}));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(output / plain.filename()) == read(plain));
    TEST_CHECK(service.openMember(plain.filename().native()));
    wait(service, pump);
    const auto opened = service.takeOpenedFile();
    TEST_CHECK(opened && read(*opened->nativePath()) == read(plain));
    service.shutdown();
    wait(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    const auto manyParts = splitZip(fixture.root / "many-zip-parts", "test.zip", read(archive), 1);
    TEST_CHECK(manyParts.size() > 128 && manyParts.size() <= 999);
    luciole::ArchiveService manyVolumes;
    const auto manyOutput = fixture.root / "many-zip-output";
    TEST_CHECK(manyVolumes.extract(location(manyParts.front()), location(manyOutput)));
    wait(manyVolumes, pump);
    TEST_CHECK(!manyVolumes.error() && read(manyOutput / plain.filename()) == read(plain));
    write(parts[1], "changed second volume");
    luciole::ArchiveService changed;
    TEST_CHECK(changed.extract(location(parts.front()), location(fixture.root / "stale-split"), {}, known));
    wait(changed, pump);
    TEST_CHECK(changed.error() && !fs::exists(fixture.root / "stale-split"));
  }
  void testSplitFailures(const Fixture& fixture, Pump& pump) {
    const auto archive = fixture.root / "full-encrypted.zip";
    encryptedZip(archive);
    const auto bytes = read(archive);
    for (unsigned failure = 0; failure < 9; ++failure) {
      const auto folder = fixture.root / ("invalid-parts-" + std::to_string(failure));
      const auto parts = splitZip(folder, "test.zip", bytes, 17);
      if (failure == 0) {
        fs::remove(parts[1]);
      } else if (failure == 1) {
        fs::remove(parts.back());
      } else if (failure == 2) {
        fs::resize_file(parts[1], 0);
      } else if (failure == 3) {
        fs::remove(parts[1]);
        fs::create_symlink(parts[0], parts[1]);
      } else if (failure == 4) {
        fs::remove(parts[1]);
        fs::create_hard_link(parts[0], parts[1]);
      } else if (failure == 5) {
        fs::remove(parts[1]);
        TEST_CHECK(::mkfifo(parts[1].c_str(), 0600) == 0);
      } else if (failure == 6) {
        write(folder / "test.zip.129", "unsupported extra volume");
      } else if (failure == 7) {
        auto damaged = read(parts.back());
        damaged.pop_back();
        write(parts.back(), damaged);
      }
      luciole::ArchiveLimits limits;
      if (failure == 8) {
        limits.maxArchiveBytes = bytes.size() - 1;
      }
      luciole::ArchiveService service(limits);
      const auto output = folder / "output";
      TEST_CHECK(service.extract(location(parts.front()), location(output)));
      wait(service, pump);
      TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(output));
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
    // A middle part must never be treated as an independently recoverable archive.
    const auto parts = splitZip(fixture.root / "middle-part", "test.zip", bytes, 17);
    luciole::ArchiveService middle;
    TEST_CHECK(middle.inspect(location(parts[1])));
    wait(middle, pump);
    TEST_CHECK(middle.error() && !middle.catalog());
    // Numbered parts do not opt TAR into the split ZIP reader.
    const auto tarFile = fixture.root / "not-a-zip.tar";
    tar(tarFile, {{"file", "payload", {}}});
    const auto fake = splitZip(fixture.root / "fake-zip", "fake.zip", read(tarFile), 1024);
    luciole::ArchiveService fakeZip;
    TEST_CHECK(fakeZip.inspect(location(fake.front())));
    wait(fakeZip, pump);
    TEST_CHECK(fakeZip.error() && !fakeZip.catalog());
  }
  void testSplitPasswordAndChanges(const Fixture& fixture, Pump& pump) {
    const auto archive = fixture.root / "split-encrypted.zip";
    encryptedZip(archive);
    const auto bytes = read(archive);
    const auto parts = splitZip(fixture.root / "encrypted-parts", "secret.zip", bytes, 17);
    const auto output = fixture.root / "unlocked-parts";
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(parts.front()), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "wrong"));
    const auto retry = passwordRequest(service, pump);
    TEST_CHECK(retry.retry && service.providePassword(retry.id, "test password"));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(output / "secret.txt") == "secret");
    TEST_CHECK(service.extract(location(parts.front()), location(fixture.root / "split-cancelled")));
    (void)passwordRequest(service, pump);
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "split-cancelled"));
    TEST_CHECK(service.extract(location(parts.front()), location(fixture.root / "split-replaced")));
    const auto replacement = passwordRequest(service, pump);
    const auto original = read(parts[1]);
    // Same bytes and pathname, a different inode: all volumes participate in the retry stamp.
    fs::remove(parts[1]);
    write(parts[1], original);
    TEST_CHECK(service.providePassword(replacement.id, "test password"));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Changed);
    TEST_CHECK(!fs::exists(fixture.root / "split-replaced") && fs::is_empty(fixture.root / "tmp"));
    const auto newSet = splitZip(fixture.root / "extended-parts", "secret.zip", bytes, 17);
    luciole::ArchiveService extended;
    TEST_CHECK(extended.extract(location(newSet.front()), location(fixture.root / "split-extended")));
    const auto pending = passwordRequest(extended, pump);
    write(newSet.back().parent_path() / std::format("secret.zip.{:03}", newSet.size() + 1), "extra data");
    TEST_CHECK(extended.providePassword(pending.id, "test password"));
    wait(extended, pump);
    TEST_CHECK(extended.error() && !fs::exists(fixture.root / "split-extended"));
  }
  struct CompressedTarCase {
    luciole::ArchiveFormat format;
    luciole::ArchiveCompression compression;
    int filter;
    std::string_view suffix, shortSuffix, mime;
  };
  constexpr std::array compressedTars{
      CompressedTarCase{
          luciole::ArchiveFormat::TarBzip2, luciole::ArchiveCompression::Bzip2, ARCHIVE_FILTER_BZIP2, ".tar.bz2",
          ".tbz2", "application/x-bzip2-compressed-tar"
      },
      CompressedTarCase{
          luciole::ArchiveFormat::TarZstd, luciole::ArchiveCompression::Zstd, ARCHIVE_FILTER_ZSTD, ".tar.zst", ".tzst",
          "application/x-zstd-compressed-tar"
      }
  };
  void testCompressedTarProviders(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "compressed-payload.txt";
    write(source, "compressed TAR payload");
    for (const auto& item : compressedTars) {
      const bool available = luciole::archiveCompressionAvailable(item.compression);
      TEST_CHECK(
          available
          == (item.filter == ARCHIVE_FILTER_BZIP2 ? archive_bzlib_version() != nullptr
                                                  : archive_libzstd_version() != nullptr)
      );
      const auto formats = luciole::archiveFormats();
      const auto format = std::ranges::find(formats, item.format, &luciole::ArchiveFormatInfo::format);
      TEST_CHECK(available == (format != formats.end()));
      const auto path = fixture.root / ("native" + std::string(item.suffix));
      TEST_CHECK(luciole::isSupportedArchive(location(path), {}) == available);
      if (!available) {
        continue;
      }
      TEST_CHECK(format->compression == item.compression && format->suffix == item.suffix);
      luciole::ArchiveService service;
      TEST_CHECK(!service.compress({location(source)}, location(path), item.format, "not supported"));
      TEST_CHECK(service.compress({location(source)}, location(path), item.format));
      wait(service, pump);
      TEST_CHECK(!service.error());
      const auto original = read(path);
      const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(
          archive_read_new(), &archive_read_free
      );
      TEST_CHECK(archive_read_support_filter_by_code(reader.get(), item.filter) == ARCHIVE_OK);
      TEST_CHECK(archive_read_support_format_tar(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_open_filename(reader.get(), path.c_str(), 65536) == ARCHIVE_OK);
      struct archive_entry* header = nullptr;
      TEST_CHECK(archive_read_next_header(reader.get(), &header) == ARCHIVE_OK);
      TEST_CHECK(archive_filter_code(reader.get(), 0) == item.filter);
      std::array<char, 64> bytes{};
      const auto count = archive_read_data(reader.get(), bytes.data(), bytes.size());
      TEST_CHECK(count == 22 && std::string_view(bytes.data(), static_cast<std::size_t>(count)) == read(source));
      const auto renamed = fixture.root / ("aliased" + std::string(item.shortSuffix));
      fs::copy_file(path, renamed);
      TEST_CHECK(luciole::archiveFolderName(location(renamed)) == "aliased");
      TEST_CHECK(service.inspect(location(renamed)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog());
      const auto known = service.catalog();
      write(renamed, original);
      const auto destination = fixture.root / ("stale" + std::string(item.shortSuffix));
      TEST_CHECK(service.extract(location(renamed), location(destination), {}, known));
      wait(service, pump);
      TEST_CHECK(
          service.error() && service.error()->kind == luciole::FileErrorKind::Changed && !fs::exists(destination)
      );
      TEST_CHECK(service.compress({location(source)}, location(path), item.format));
      pump.until([&] { return service.conflict().has_value() || !service.busy(); });
      TEST_CHECK(service.conflict() && service.resolve(service.conflict()->id, luciole::TransferConflictChoice::Skip));
      wait(service, pump);
      TEST_CHECK(read(path) == original && fs::is_empty(fixture.root / "tmp"));
      const auto cancelled = fixture.root / ("cancelled" + std::string(item.suffix));
      TEST_CHECK(service.compress({location(source)}, location(cancelled), item.format));
      service.cancel();
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(cancelled) && fs::is_empty(fixture.root / "tmp"));
    }
  }
  void testCompressedTarFailures(const Fixture& fixture, Pump& pump) {
    for (const auto& item : compressedTars) {
      if (!luciole::archiveCompressionAvailable(item.compression)) {
        continue;
      }
      for (unsigned failure = 0; failure < 5; ++failure) {
        const auto name = std::to_string(item.filter) + "-" + std::to_string(failure);
        const auto source = fixture.root / (name + std::string(item.suffix));
        const auto destination = fixture.root / ("rejected-compressed-" + name);
        const auto entry = failure == 0 ? Entry{"../escaped", "bad", {}}
            : failure == 1              ? Entry{"link", {}, "../escaped", AE_IFLNK}
                                        : Entry{"payload", std::string(129536, 'x'), {}};
        tar(source, {entry}, item.filter);
        luciole::ArchiveLimits limits;
        if (failure == 2) {
          limits.maxExpandedBytes = 1024;
        } else if (failure == 3) {
          fs::resize_file(source, fs::file_size(source) / 2);
        } else if (failure == 4) {
          // Incomplete framing after TAR end markers must not publish a prefix.
          fs::resize_file(source, fs::file_size(source) - 1);
        }
        luciole::ArchiveService service(limits);
        TEST_CHECK(service.extract(location(source), location(destination)));
        wait(service, pump);
        if (!service.error()) {
          std::println(stderr, "Compressed TAR failure fixture accepted: filter={}, failure={}", item.filter, failure);
        }
        TEST_CHECK(service.error() && !fs::exists(destination) && fs::is_empty(fixture.root / "tmp"));
      }
    }
    TEST_CHECK(!fs::exists(fixture.root / "escaped"));
  }
  void testCompressedPaddingLimit(const Fixture& fixture, Pump& pump) {
    const auto plain = fixture.root / "padded-container.tar";
    tar(plain, {{"file", "abc", {}}});
    const auto data = read(plain) + std::string(4U * 1024U * 1024U, '\0');
    for (const auto& item : compressedTars) {
      if (!luciole::archiveCompressionAvailable(item.compression)) {
        continue;
      }
      const auto source = fixture.root / ("padded" + std::string(item.suffix));
      const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
          archive_write_new(), &archive_write_free
      );
      TEST_CHECK(archive_write_set_format_raw(writer.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_write_add_filter(writer.get(), item.filter) == ARCHIVE_OK);
      TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
      TEST_CHECK(archive_write_open_filename(writer.get(), source.c_str()) == ARCHIVE_OK);
      const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), &archive_entry_free
      );
      archive_entry_set_pathname(entry.get(), "data");
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_size(entry.get(), static_cast<la_int64_t>(data.size()));
      TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_write_data(writer.get(), data.data(), data.size()) == static_cast<la_ssize_t>(data.size()));
      TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
      luciole::ArchiveLimits limits;
      limits.maxExpandedBytes = 128;
      limits.maxMetadataBytes = 256;
      limits.maxEntries = 4;
      luciole::ArchiveService service(limits);
      const auto output = fixture.root / ("padding-rejected" + std::to_string(item.filter));
      TEST_CHECK(service.extract(location(source), location(output)));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Unsupported);
      TEST_CHECK(service.error()->message.contains("decoded-container") && !fs::exists(output));
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
  }
  std::string base64Bytes(std::string_view encoded) {
    gsize size = 0;
    const std::unique_ptr<guchar, decltype(&g_free)> bytes(g_base64_decode(encoded.data(), &size), &g_free);
    TEST_CHECK(bytes && size > 32);
    return {reinterpret_cast<const char*>(bytes.get()), size};
  }
  std::string fixtureBytes(const auto& fixture) { return base64Bytes(fixture.base64); }
  std::string sevenZipPayload(std::string_view marker) {
    std::string result;
    result.reserve(marker.size() * 32);
    for (unsigned i = 0; i < 32; ++i) {
      result += marker;
    }
    return result;
  }
  std::string digest(std::string_view bytes) {
    const luciole::GString value(
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(bytes.data()), bytes.size())
    );
    TEST_CHECK(value);
    return value.get();
  }
  void testRarProviders(const Fixture& fixture, Pump& pump) {
    TEST_CHECK(std::ranges::none_of(luciole::archiveFormats(), [](const auto& format) {
      return format.suffix == ".rar";
    }));
    for (const auto& reference : rar_fixtures::archives) {
      const auto source = fixture.root / (std::string(reference.name) + ".rar");
      const auto output = fixture.root / ("rar-extracted-" + std::string(reference.name));
      const auto bytes = fixtureBytes(reference);
      write(source, bytes);
      luciole::ArchiveService service;
      TEST_CHECK(service.inspect(location(source)));
      wait(service, pump);
      if (!reference.error.empty()) {
        TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Unsupported);
        TEST_CHECK(
            service.error()->message.contains(reference.error) && !service.passwordRequest() && !service.catalog()
        );
        TEST_CHECK(service.extract(location(source), location(output)));
        wait(service, pump);
        TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(output));
        TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
        continue;
      }
      if (service.error()) {
        throw std::runtime_error(std::string(reference.name) + ": " + service.error()->message);
      }
      const auto known = service.catalog();
      TEST_CHECK(known && known->format.starts_with("RAR") && !known->entries.empty() && !known->encrypted);
      TEST_CHECK(service.extract(location(source), location(output), {}, known));
      wait(service, pump);
      if (service.error()) {
        throw std::runtime_error(std::string(reference.name) + ": " + service.error()->message);
      }
      TEST_CHECK(!service.error() && fs::file_size(output / reference.member) == reference.size);
      TEST_CHECK(digest(read(output / reference.member)) == reference.sha256);
      if (reference.name == "rar4-stored") {
        TEST_CHECK(fs::is_symlink(output / "testlink") && fs::read_symlink(output / "testlink") == "test.txt");
      } else if (reference.name == "rar5-hardlink") {
        TEST_CHECK(fs::equivalent(output / "file.txt", output / "hardlink.txt"));
      } else if (reference.name == "rar5-symlink") {
        TEST_CHECK(fs::is_symlink(output / "symlink.txt") && fs::read_symlink(output / "symlink.txt") == "file.txt");
      }
      const auto selected = fixture.root / ("rar-selected-" + std::string(reference.name));
      TEST_CHECK(service.extract(location(source), location(selected), {std::string(reference.member)}, known));
      wait(service, pump);
      TEST_CHECK(!service.error() && digest(read(selected / reference.member)) == reference.sha256);
      TEST_CHECK(service.openMember(std::string(reference.member)));
      wait(service, pump);
      const auto opened = service.takeOpenedFile();
      TEST_CHECK(!service.error() && opened && digest(read(*opened->nativePath())) == reference.sha256);
      TEST_CHECK((fs::status(*opened->nativePath()).permissions() & fs::perms::owner_exec) == fs::perms::none);
      write(source, bytes);
      const auto stale = fixture.root / ("rar-stale-" + std::string(reference.name));
      TEST_CHECK(service.extract(location(source), location(stale), {}, known));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Changed && !fs::exists(stale));
      service.shutdown();
      wait(service, pump);
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
  }
  void testRarStoredChunks(const Fixture& fixture, Pump& pump) {
    auto bytes = fixtureBytes(rar_fixtures::archives[0]);
    const auto size =
        static_cast<unsigned char>(bytes[25]) | (static_cast<unsigned>(static_cast<unsigned char>(bytes[26])) << 8);
    const auto header = base64Bytes(rar_fixtures::largeStoredHeader);
    const std::string payload(131072 + 17, 'x');
    bytes.replace(20, size + 20, header + payload);
    const auto source = fixture.root / "large-stored.rar";
    write(source, bytes);
    luciole::ArchiveService service;
    const auto output = fixture.root / "large-rar-output";
    TEST_CHECK(service.extract(location(source), location(output), {"test.txt"}));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(output / "test.txt") == payload);
    bytes[20 + header.size() + 65536] ^= 1;
    write(source, bytes);
    TEST_CHECK(service.extract(location(source), location(fixture.root / "large-rar-corrupt")));
    wait(service, pump);
    TEST_CHECK(
        service.error() && !fs::exists(fixture.root / "large-rar-corrupt") && fs::is_empty(fixture.root / "tmp")
    );
  }
  void testRarFailures(const Fixture& fixture, Pump& pump) {
    unsigned index = 0;
    for (const auto encoded : rar_fixtures::rejected) {
      const auto source = fixture.root / ("rar-hostile-" + std::to_string(index) + ".rar");
      const auto output = fixture.root / ("rar-hostile-output-" + std::to_string(index++));
      write(source, base64Bytes(encoded));
      luciole::ArchiveService service;
      TEST_CHECK(service.extract(location(source), location(output)));
      wait(service, pump);
      if (!service.error()) {
        std::println(stderr, "RAR rejected fixture accepted: {}", index - 1);
      }
      TEST_CHECK(service.error() && !fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
    }
    TEST_CHECK(!fs::exists(fixture.root / "x.txt"));
    for (const auto index : {0U, 1U, 2U, 3U}) {
      const auto bytes = fixtureBytes(rar_fixtures::archives[index]);
      for (unsigned failure = 0; failure < 6; ++failure) {
        const auto name = std::to_string(index) + "-" + std::to_string(failure);
        auto bad = bytes;
        luciole::ArchiveLimits limits;
        if (failure == 0) {
          bad.resize(bad.size() / 2);
        } else if (failure == 1) {
          bad[index < 2 ? 7 : 8] ^= 1;
        } else if (failure == 2) {
          limits.maxExpandedBytes = 1;
        } else if (failure == 3) {
          limits.maxMetadataBytes = 1;
        } else if (failure == 4) {
          limits.maxArchiveBytes = bad.size() - 1;
        } else {
          bad.resize(bad.size() - 1);
        }
        const auto source = fixture.root / ("bad-rar-" + name + ".rar");
        const auto output = fixture.root / ("rar-rejected-" + name);
        write(source, bad);
        luciole::ArchiveService service(limits);
        TEST_CHECK(service.extract(location(source), location(output)));
        wait(service, pump);
        if (!service.error()) {
          std::println(stderr, "RAR failure fixture accepted: {}", name);
        }
        TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(output));
        TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
      }
    }
    const auto bytes = fixtureBytes(rar_fixtures::archives[3]);
    const auto source = fixture.root / "rar-cancel.rar";
    write(source, bytes);
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(fixture.root / "rar-cancelled")));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "rar-cancelled") && fs::is_empty(fixture.root / "tmp"));
    const auto split = splitZip(fixture.root / "rar-fake-split", "fake.zip", bytes, bytes.size() / 2);
    TEST_CHECK(service.inspect(location(split.front())));
    wait(service, pump);
    TEST_CHECK(service.error() && !service.catalog());
  }
  std::string rar5Header(std::string_view bytes) {
    auto prefix = fixtureBytes(rar_fixtures::archives[2]).substr(0, 8);
    prefix += std::string(4, '\0'); // Main-envelope rejection happens before native CRC validation.
    prefix.append(bytes);
    return prefix;
  }
  template <std::size_t Size> std::string rar5Header(const char (&bytes)[Size]) {
    return rar5Header(std::string(bytes, Size - 1));
  }
  void testRarMainHeaders(const Fixture& fixture, Pump& pump) {
    const auto legacy = fixture.root / "rar-without-end-marker.rar";
    auto legacyBytes = fixtureBytes(rar_fixtures::archives[0]);
    legacyBytes.resize(legacyBytes.size() - 7);
    write(legacy, legacyBytes);
    luciole::ArchiveService legacyService;
    TEST_CHECK(legacyService.extract(location(legacy), location(fixture.root / "rar-legacy-eof")));
    wait(legacyService, pump);
    TEST_CHECK(
        !legacyService.error()
        && digest(read(fixture.root / "rar-legacy-eof/test.txt")) == rar_fixtures::archives[0].sha256
    );
    auto encrypted4 = fixtureBytes(rar_fixtures::archives[0]);
    encrypted4[10] |= static_cast<char>(0x80);
    const std::vector<std::string> invalid{
        rar5Header(std::string(11, static_cast<char>(0x80))),
        rar5Header(std::string(9, static_cast<char>(0xFF)) + "\x02"),
        rar5Header("\x01\x01\x00\x00"),
        rar5Header("\x03\x01\x01"),
        rar5Header("\x04\x01\x01\x7f\x00"),
        rar5Header("\x03\x01\x00\x00").substr(0, 13)
    };
    for (std::size_t i = 0; i < invalid.size(); ++i) {
      const auto source = fixture.root / ("invalid-rar-main-" + std::to_string(i) + ".rar");
      write(source, invalid[i]);
      luciole::ArchiveService service;
      TEST_CHECK(service.inspect(location(source)));
      wait(service, pump);
      TEST_CHECK(
          service.error()
          && service.error()->kind == luciole::FileErrorKind::InvalidArgument
          && !service.passwordRequest()
      );
    }
    for (const auto& bytes : {encrypted4, rar5Header("\x03\x04\x00\x00")}) {
      const auto source = fixture.root / "encrypted-rar-main.rar";
      write(source, bytes);
      luciole::ArchiveService service;
      TEST_CHECK(service.inspect(location(source)));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Unsupported);
      TEST_CHECK(service.error()->message.contains("Encrypted RAR") && !service.passwordRequest());
    }
  }
  void testEncryptedSevenZip(
      const fs::path& source, const fs::path& destination, luciole::ArchiveService& service, Pump& pump
  ) {
    const auto known = service.catalog();
    TEST_CHECK(!service.error() && known && known->encrypted && known->entries.size() == 3);
    TEST_CHECK(std::ranges::count(known->entries, true, &luciole::ArchiveEntry::encrypted) == 2);
    TEST_CHECK(service.extract(location(source), location(destination), {}, known));
    const auto first = passwordRequest(service, pump);
    TEST_CHECK(first.format == luciole::ArchiveFormat::SevenZip && !first.retry && !fs::exists(destination));
    for (const auto& invalid : {std::string("line\nbreak"), std::string("line\rbreak"), std::string("\xff")}) {
      TEST_CHECK(luciole::archivePasswordProblem(invalid, first.format));
      TEST_CHECK(!service.providePassword(first.id, invalid));
      TEST_CHECK(service.passwordRequest()->id == first.id);
    }
    TEST_CHECK(service.providePassword(first.id, "wrong password"));
    const auto retry = passwordRequest(service, pump);
    TEST_CHECK(retry.retry && !fs::exists(destination));
    TEST_CHECK(service.providePassword(retry.id, "fixture-pass"));
    wait(service, pump);
    if (service.error()) {
      throw std::runtime_error(service.error()->message);
    }
    TEST_CHECK(read(destination / "folder/first.txt") == sevenZipPayload("first payload\n"));
    TEST_CHECK(read(destination / "second.txt") == sevenZipPayload("second payload\n"));
    const auto selected = fs::path(destination.string() + "-selected");
    TEST_CHECK(service.extract(location(source), location(selected), {"second.txt"}, known));
    auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    wait(service, pump);
    TEST_CHECK(!service.error() && !fs::exists(selected / "folder"));
    TEST_CHECK(read(selected / "second.txt") == sevenZipPayload("second payload\n"));
    TEST_CHECK(service.openMember("second.txt"));
    request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    wait(service, pump);
    const auto opened = service.takeOpenedFile();
    TEST_CHECK(!service.error() && opened && read(*opened->nativePath()) == sevenZipPayload("second payload\n"));
    TEST_CHECK((fs::status(*opened->nativePath()).permissions() & fs::perms::owner_exec) == fs::perms::none);
    const auto cancelled = fs::path(destination.string() + "-cancelled");
    TEST_CHECK(service.extract(location(source), location(cancelled), {}, known));
    request = passwordRequest(service, pump);
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(cancelled));
    TEST_CHECK(!service.providePassword(request.id, "fixture-pass"));
    TEST_CHECK(service.extract(location(source), location(cancelled), {}, known));
    request = passwordRequest(service, pump);
    const auto replacement = fs::path(source.string() + "-replacement");
    write(replacement, read(source));
    fs::rename(replacement, source);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Changed);
    TEST_CHECK(!service.passwordRequest() && !fs::exists(cancelled));
    service.shutdown();
    wait(service, pump);
    TEST_CHECK(!fs::exists(*opened->nativePath()));
  }
  void testSevenZipLiteralNames(const Fixture& fixture, Pump& pump, bool encryptedHeader = false) {
    const auto source = fixture.root / "literal.7z",
               output = fixture.root / (encryptedHeader ? "literal-header-output" : "literal-output");
    write(source, fixtureBytes(encryptedHeader ? sevenzip_fixtures::literalHeader : sevenzip_fixtures::literal));
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    if (encryptedHeader) {
      const auto request = passwordRequest(service, pump);
      TEST_CHECK(service.providePassword(request.id, " café secret "));
    }
    wait(service, pump);
    if (service.error()) {
      std::println(stderr, "Literal 7z inspection: {}", service.error()->message);
    }
    TEST_CHECK(!service.error() && service.catalog()->encrypted);
    if (!encryptedHeader) {
      const auto emptyOutput = fixture.root / "clear-empty-member";
      TEST_CHECK(service.extract(location(source), location(emptyOutput), {"empty"}));
      wait(service, pump);
      TEST_CHECK(!service.error() && !service.passwordRequest() && fs::file_size(emptyOutput / "empty") == 0);
    }
    // 7-Zip lists control characters as "_", so such a name cannot be addressed; it fails closed.
    const auto unlisted = fixture.root / (encryptedHeader ? "literal-header-unlisted" : "literal-unlisted");
    TEST_CHECK(
        service.catalog()->entries.end()
        != std::ranges::find(service.catalog()->entries, "line_break", &luciole::ArchiveEntry::path)
    );
    TEST_CHECK(service.extract(location(source), location(unlisted), {"line_break"}));
    auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, " café secret "));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::InvalidArgument);
    TEST_CHECK(!fs::exists(unlisted));
    TEST_CHECK(service.extract(
        location(source), location(output), {"same.txt", "nested", "@list", "wild*card", "-poption", "café", "empty"}
    ));
    request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, " café secret "));
    wait(service, pump);
    if (service.error()) {
      throw std::runtime_error(service.error()->message);
    }
    for (const auto& [name, contents] : std::array<std::pair<const char*, const char*>, 7>{
             {{"same.txt", "root payload"},
              {"nested/same.txt", "nested payload"},
              {"@list", "literal at"},
              {"wild*card", "literal wildcard"},
              {"-poption", "literal dash"},
              {"café", "literal unicode"},
              {"empty", ""}}
         }) {
      TEST_CHECK(fs::is_regular_file(output / name) && read(output / name) == contents);
    }
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testEncryptedSevenZipBoundaries(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "encrypted-boundary.7z", output = fixture.root / "encrypted-boundary-output";
    const auto bytes = fixtureBytes(sevenzip_fixtures::literal);
    write(source, bytes);
    luciole::ArchiveLimits limits;
    limits.maxExpandedBytes = 1;
    luciole::ArchiveService limited(limits);
    TEST_CHECK(limited.extract(location(source), location(output)));
    wait(limited, pump);
    TEST_CHECK(limited.error() && !limited.passwordRequest() && !fs::exists(output));
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(output)));
    auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, " café secret "));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(output));
    {
      luciole::ArchiveService closing;
      TEST_CHECK(closing.extract(location(source), location(output)));
      (void)passwordRequest(closing, pump);
      closing.shutdown();
      wait(closing, pump);
      TEST_CHECK(!closing.passwordRequest() && !fs::exists(output));
    }
    auto corrupt = bytes;
    corrupt[32] ^= 0x20;
    write(source, corrupt);
    TEST_CHECK(service.extract(location(source), location(output)));
    request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, " café secret "));
    const auto retry = passwordRequest(service, pump);
    TEST_CHECK(retry.retry && !fs::exists(output));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  int boundedEncryptedWriteFailure() {
    Fixture fixture;
    const auto source = fixture.root / "source.7z", output = fixture.root / "output";
    write(source, fixtureBytes(sevenzip_fixtures::literal));
    luciole::archive_work::Progress progress;
    luciole::archive_work::Passwords passwords;
    passwords.values.emplace_back(" café secret ");
    const luciole::CancelToken cancel;
    rlimit previous{};
    TEST_CHECK(::getrlimit(RLIMIT_FSIZE, &previous) == 0);
    const rlimit limit{0, previous.rlim_max};
    std::signal(SIGXFSZ, SIG_IGN);
    TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
    bool failed = false;
    try {
      auto prepared = luciole::archive_work::extract(
          location(source), location(output), {}, {}, false, {}, cancel, progress, passwords
      );
      (void)luciole::archive_work::cleanup({prepared.stage});
    } catch (const luciole::FileErrorException& error) {
      failed = error.error().kind != luciole::FileErrorKind::Denied;
    }
    TEST_CHECK(failed && !progress.passwordNeeded.load() && !fs::exists(output));
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    return 0;
  }
  const sevenzip_fixtures::Fixture& encryptedHeaderFixture() {
    const auto* result =
        std::ranges::find(sevenzip_fixtures::archives, "encrypted-header", &sevenzip_fixtures::Fixture::name);
    TEST_CHECK(result != sevenzip_fixtures::archives.end());
    return *result;
  }
  void testEncryptedHeaderFailures(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "header-boundary.7z", output = fixture.root / "header-boundary-output";
    for (const auto* reference :
         {&sevenzip_fixtures::traversalHeader, &sevenzip_fixtures::duplicateHeader,
          &sevenzip_fixtures::ancestorHeader}) {
      write(source, fixtureBytes(*reference));
      luciole::ArchiveService service;
      TEST_CHECK(service.extract(location(source), location(output)));
      const auto request = passwordRequest(service, pump);
      TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::InvalidArgument);
      TEST_CHECK(!service.passwordRequest() && !fs::exists(output) && !fs::exists(fixture.root / "escape"));
    }
    write(source, fixtureBytes(encryptedHeaderFixture()));
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    auto request = passwordRequest(service, pump);
    service.cancel();
    wait(service, pump);
    TEST_CHECK(!service.catalog() && !service.passwordRequest());
    TEST_CHECK(service.inspect(location(source)));
    request = passwordRequest(service, pump);
    const auto replacement = fixture.root / "header-replacement";
    write(replacement, read(source));
    fs::rename(replacement, source);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Changed && !service.catalog());
    TEST_CHECK(service.inspect(location(source)));
    (void)passwordRequest(service, pump);
    service.shutdown();
    wait(service, pump);
    TEST_CHECK(!service.passwordRequest() && fs::is_empty(fixture.root / "tmp"));
    auto corrupt = fixtureBytes(encryptedHeaderFixture());
    corrupt[80] ^= 0x20;
    write(source, corrupt);
    luciole::ArchiveService damaged;
    TEST_CHECK(damaged.inspect(location(source)));
    request = passwordRequest(damaged, pump);
    TEST_CHECK(damaged.providePassword(request.id, "fixture-pass"));
    const auto retry = passwordRequest(damaged, pump);
    TEST_CHECK(retry.retry && !damaged.catalog());
    damaged.cancel();
    wait(damaged, pump);
    luciole::ArchiveLimits limits;
    limits.maxMetadataBytes = 1;
    luciole::ArchiveService limited(limits);
    TEST_CHECK(limited.inspect(location(source)));
    wait(limited, pump);
    TEST_CHECK(limited.error() && !limited.passwordRequest() && !limited.catalog());
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testSevenZipCompressedHeader(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "compressed-header.7z", output = fixture.root / "compressed-header-output";
    write(source, fixtureBytes(sevenzip_fixtures::compressedHeader));
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    wait(service, pump);
    TEST_CHECK(!service.error() && std::distance(fs::directory_iterator(output), fs::directory_iterator{}) == 24);
    TEST_CHECK(read(output / "00-long-metadata-name-for-header-compression.txt") == "payload");
    write(source, fixtureBytes(sevenzip_fixtures::linkHeader));
    TEST_CHECK(service.inspect(location(source)));
    const auto link = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(link.id, "fixture-pass"));
    wait(service, pump);
    TEST_CHECK(
        service.error() && service.error()->kind == luciole::FileErrorKind::Unsupported && !service.passwordRequest()
    );
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testSevenZipClearDataHeader(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "clear-data-header.7z", output = fixture.root / "clear-data-header-output";
    write(source, fixtureBytes(sevenzip_fixtures::clearDataHeader));
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    wait(service, pump);
    TEST_CHECK(!service.error() && service.catalog()->encrypted && service.catalog()->entries.size() == 1);
    TEST_CHECK(!service.catalog()->entries.front().encrypted);
    TEST_CHECK(service.extract(location(source), location(output)));
    request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(output / "plain.txt") == "clear file data with encrypted metadata");
    write(source, fixtureBytes(sevenzip_fixtures::invalidPackHeader));
    const auto invalid = fixture.root / "invalid-pack-output";
    TEST_CHECK(service.extract(location(source), location(invalid)));
    request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "fixture-pass"));
    pump.until([&] { return service.passwordRequest() || !service.busy(); });
    TEST_CHECK(!fs::exists(invalid));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testSevenZipCatalogSource(const Fixture& fixture) {
    const auto source = fixture.root / "native-catalog.7z";
    const auto original = fixtureBytes(encryptedHeaderFixture());
    write(source, original);
    luciole::archive_work::Progress progress;
    luciole::archive_work::Work work{{}, {}, progress, luciole::archive_work::Clock::now() + 5s, {}};
    luciole::archive_work::Input input(location(source), work);
    luciole::archive_work::Passwords passwords;
    passwords.values.emplace_back("fixture-pass");
    const auto stamp = input.fingerprint();
    const auto catalog = luciole::archive_work::sevenZipCatalog(input, work, passwords);
    TEST_CHECK(catalog.size() == 3 && catalog[1].path == "folder/first.txt" && catalog[1].encrypted);
    TEST_CHECK(input.seek(0, SEEK_END) == static_cast<std::int64_t>(original.size()));
    const auto fd = input.duplicateSingle();
    std::array<char, 32> signature{};
    TEST_CHECK(::pread(fd.get(), signature.data(), signature.size(), 0) == static_cast<ssize_t>(signature.size()));
    TEST_CHECK(std::string_view(signature.data(), signature.size()) == std::string_view(original).substr(0, 32));
    input.unchanged();
    TEST_CHECK(input.fingerprint() == stamp && read(source) == original && fs::is_empty(fixture.root / "tmp"));
  }
  void testSevenZipAstral(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "astral.7z", output = fixture.root / "astral-output";
    write(source, fixtureBytes(sevenzip_fixtures::astralHeader));
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, " fixture-🌙 "));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(output / "📁/astral-🌙.txt") == "astral payload\n");
  }
  int headerWithoutTools() {
    Fixture fixture;
    Pump pump;
    const auto source = fixture.root / "header.7z";
    write(source, fixtureBytes(encryptedHeaderFixture()));
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Unavailable);
    TEST_CHECK(!service.passwordRequest() && !service.catalog() && fs::is_empty(fixture.root / "tmp"));
    return 0;
  }
  int sevenZipProviderFailures() {
    Fixture fixture;
    const auto tools = fixture.root / "tools", source = fixture.root / "source.7z", marker = fixture.root / "marker";
    fs::create_directory(tools);
    write(source, fixtureBytes(sevenzip_fixtures::literal));
    // This fake 7-Zip has no subprocesses of its own. Secrets are public test data.
    write(tools / "7z", R"sh(#!/bin/sh
for argument do
  case "$argument" in *fixture-pass*) exit 7;; esac
done
[ -r /proc/self/fd/3 ] || exit 7
case "$NF_SEVENZIP_TEST_MODE" in
  early) exit 8;;
esac
IFS= read -r secret
[ "$secret" = fixture-pass ] || exit 7
case "$NF_SEVENZIP_TEST_MODE" in
  resources)
    [ "$(ulimit -v)" = "$NF_SEVENZIP_TEST_AS" ] || exit 8
    [ "$(ulimit -t)" = "$NF_SEVENZIP_TEST_CPU" ] || exit 8
    printf 'Path = member\nSize = 0\nAttributes = A -rw-r--r--\n';;
  corrupt) echo 'ERROR: Data Error in encrypted file. Wrong password? : same.txt'; exit 2;;
  fatal) echo 'ERROR: Unsupported Method : same.txt'; exit 2;;
  memory) exit 8;;
  noise) i=0; while [ $i -lt 40000 ]; do echo 0123456789012345678901234567890123456789012345678; i=$((i+1)); done;;
  listing-garbage) printf 'Path = x\nnonsense\n';;
  listing-size) printf 'Path = x\nSize = -1\nAttributes = A -rw-r--r--\n';;
  listing-duplicate) printf 'Path = x\nPath = y\nSize = 1\n';;
  listing-mode) printf 'Path = x\nSize = 1\nAttributes = D -rw-r--r--\n';;
  listing-link) printf 'Path = x\nSize = 1\nAttributes = A lrwxrwxrwx\n';;
  hang) printf '%s' "$$" > "$NF_SEVENZIP_TEST_MARKER"; while :; do :; done;;
  *) exit 7;;
esac
)sh");
    fs::permissions(tools / "7z", fs::perms::owner_all);
    TEST_CHECK(::setenv("LUCIOLE_7Z", (tools / "7z").c_str(), 1) == 0);
    TEST_CHECK(::setenv("NF_SEVENZIP_TEST_MARKER", marker.c_str(), 1) == 0);
    for (const std::string mode : {"corrupt", "fatal", "memory", "noise", "early", "hang", "cancel", "changed"}) {
      const auto output = fixture.root / (mode + "-output");
      const bool waiting = mode == "hang" || mode == "cancel" || mode == "changed";
      TEST_CHECK(::setenv("NF_SEVENZIP_TEST_MODE", waiting ? "hang" : mode.c_str(), 1) == 0);
      fs::remove(marker);
      luciole::archive_work::Progress progress;
      luciole::CancelToken cancel;
      luciole::archive_work::Passwords passwords;
      passwords.values.emplace_back("fixture-pass");
      luciole::archive_work::Work work{{}, cancel, progress, luciole::archive_work::Clock::now() + 2s, {}};
      if (mode == "hang") {
        work.deadline = luciole::archive_work::Clock::now() + 150ms;
      }
      luciole::archive_work::Input input(location(source), work);
      std::thread interrupt;
      if (mode == "cancel" || mode == "changed") {
        interrupt = std::thread([&] {
          const auto timeout = std::chrono::steady_clock::now() + 1s;
          while ((!fs::exists(marker) || read(marker).empty()) && std::chrono::steady_clock::now() < timeout) {
            std::this_thread::sleep_for(1ms);
          }
          if (mode == "changed") {
            const auto replacement = fixture.root / "replacement";
            write(replacement, read(source));
            fs::rename(replacement, source);
            // Finish the helper; source identity must be checked before interpreting its result.
            if (fs::exists(marker) && !read(marker).empty()) {
              ::kill(std::stoi(read(marker)), SIGTERM);
            }
          } else {
            cancel.cancel();
          }
        });
      }
      bool failed = false;
      try {
        luciole::archive_work::extractSevenZip(input, work, passwords, {"same.txt"}, true, 12, output.native());
      } catch (const std::exception&) {
        failed = true;
      }
      if (interrupt.joinable()) {
        interrupt.join();
      }
      TEST_CHECK(failed && fs::is_empty(fixture.root / "tmp"));
      TEST_CHECK(progress.passwordNeeded.load() == (mode == "corrupt"));
      if (waiting && fs::exists(marker) && !read(marker).empty()) {
        errno = 0;
        TEST_CHECK(::kill(std::stoi(read(marker)), 0) == -1 && errno == ESRCH);
      }
    }
    for (const auto* mode :
         {"fatal", "listing-garbage", "listing-size", "listing-duplicate", "listing-mode", "listing-link"}) {
      TEST_CHECK(::setenv("NF_SEVENZIP_TEST_MODE", mode, 1) == 0);
      luciole::archive_work::Progress progress;
      luciole::archive_work::Work work{{}, {}, progress, luciole::archive_work::Clock::now() + 2s, {}};
      luciole::archive_work::Input input(location(source), work);
      luciole::archive_work::Passwords passwords;
      passwords.values.emplace_back("fixture-pass");
      std::optional<luciole::FileErrorKind> rejected;
      try {
        (void)luciole::archive_work::sevenZipCatalog(input, work, passwords);
      } catch (const luciole::FileErrorException& error) {
        rejected = error.error().kind;
      }
      const auto expected = std::string_view(mode) == "listing-link" ? luciole::FileErrorKind::Unsupported
                                                                     : luciole::FileErrorKind::InvalidArgument;
      TEST_CHECK(rejected == expected && !progress.passwordNeeded.load() && fs::is_empty(fixture.root / "tmp"));
    }
    rlimit memory{}, cpu{};
    TEST_CHECK(::getrlimit(RLIMIT_AS, &memory) == 0 && ::getrlimit(RLIMIT_CPU, &cpu) == 0);
    const auto addressSpace = memory.rlim_cur == RLIM_INFINITY ? "unlimited" : std::to_string(memory.rlim_cur / 1024);
    const auto cpuTime = cpu.rlim_cur == RLIM_INFINITY ? "unlimited" : std::to_string(cpu.rlim_cur);
    TEST_CHECK(::setenv("NF_SEVENZIP_TEST_AS", addressSpace.c_str(), 1) == 0);
    TEST_CHECK(::setenv("NF_SEVENZIP_TEST_CPU", cpuTime.c_str(), 1) == 0);
    TEST_CHECK(::setenv("NF_SEVENZIP_TEST_MODE", "resources", 1) == 0);
    luciole::archive_work::Progress progress;
    auto work = luciole::archive_work::workFor({}, {}, progress);
    luciole::archive_work::Input input(location(source), work);
    luciole::archive_work::Passwords passwords;
    passwords.values.emplace_back("fixture-pass");
    const auto members = luciole::archive_work::sevenZipCatalog(input, work, passwords);
    TEST_CHECK(members.size() == 1 && members[0].path == "member");
    return 0;
  }
  void testSevenZipProviders(const Fixture& fixture, Pump& pump) {
    for (const auto& reference : sevenzip_fixtures::archives) {
      if ((reference.name == "bzip2" && !archive_bzlib_version())
          || (reference.name == "deflate" && !archive_zlib_version())
          || ((reference.name == "lzma1" || reference.name == "lzma2" || reference.name == "nonsolid")
              && !archive_liblzma_version())) {
        continue;
      }
      const auto source = fixture.root / (std::string(reference.name) + ".7z");
      const auto destination = fixture.root / ("sevenzip-" + std::string(reference.name));
      const auto bytes = fixtureBytes(reference);
      write(source, bytes);
      luciole::ArchiveService service;
      TEST_CHECK(service.inspect(location(source)));
      if (reference.name == "encrypted-header") {
        const auto request = passwordRequest(service, pump);
        TEST_CHECK(request.format == luciole::ArchiveFormat::SevenZip && !service.catalog());
        TEST_CHECK(service.providePassword(request.id, "wrong"));
        const auto retry = passwordRequest(service, pump);
        TEST_CHECK(retry.retry && service.providePassword(retry.id, "fixture-pass"));
      }
      wait(service, pump);
      if (reference.encrypted) {
        testEncryptedSevenZip(source, destination, service, pump);
        continue;
      }
      if (service.error()) {
        throw std::runtime_error(std::string(reference.name) + ": " + service.error()->message);
      }
      const auto known = service.catalog();
      TEST_CHECK(known && known->entries.size() == 3 && known->format == "7-Zip" && !known->encrypted);
      TEST_CHECK(service.extract(location(source), location(destination), {}, known));
      wait(service, pump);
      TEST_CHECK(!service.error() && read(destination / "folder/first.txt") == sevenZipPayload("first payload\n"));
      TEST_CHECK(read(destination / "second.txt") == sevenZipPayload("second payload\n"));
      const auto selected = fixture.root / ("sevenzip-selected-" + std::string(reference.name));
      TEST_CHECK(service.extract(location(source), location(selected), {"second.txt"}, known));
      wait(service, pump);
      TEST_CHECK(
          !service.error()
          && read(selected / "second.txt") == sevenZipPayload("second payload\n")
          && !fs::exists(selected / "folder")
      );
      TEST_CHECK(service.openMember("second.txt"));
      wait(service, pump);
      const auto opened = service.takeOpenedFile();
      TEST_CHECK(!service.error() && opened && read(*opened->nativePath()) == sevenZipPayload("second payload\n"));
      TEST_CHECK((fs::status(*opened->nativePath()).permissions() & fs::perms::owner_exec) == fs::perms::none);
      write(source, bytes);
      const auto stale = fixture.root / ("sevenzip-stale-" + std::string(reference.name));
      TEST_CHECK(service.extract(location(source), location(stale), {}, known));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Changed && !fs::exists(stale));
      service.shutdown();
      wait(service, pump);
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
  }
  void testSevenZipFailures(const Fixture& fixture, Pump& pump) {
    const auto bytes = fixtureBytes(sevenzip_fixtures::archives.front());
    for (unsigned failure = 0; failure < 9; ++failure) {
      const auto source = fixture.root / ("bad-sevenzip-" + std::to_string(failure) + ".7z");
      auto bad = bytes;
      luciole::ArchiveLimits limits;
      if (failure == 0) {
        bad.resize(bad.size() - 1);
      } else if (failure == 1) {
        bad[8] ^= 1;
      } else if (failure == 2) {
        bad[32] ^= 1;
      } else if (failure == 3) {
        limits.maxExpandedBytes = 4;
      } else if (failure == 4) {
        limits.maxEntries = 1;
      } else if (failure == 5) {
        limits.maxMetadataBytes = 4;
      } else if (failure == 6) {
        limits.maxArchiveBytes = bad.size() - 1;
      } else if (failure == 7) {
        bad.resize(16);
      } else {
        bad[bad.size() - 1] ^= 1;
      }
      write(source, bad);
      luciole::ArchiveService service(limits);
      const auto destination = fixture.root / ("sevenzip-rejected-" + std::to_string(failure));
      TEST_CHECK(service.extract(location(source), location(destination)));
      wait(service, pump);
      if (!service.error()) {
        std::println(stderr, "7z failure fixture accepted: {}", failure);
      }
      TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(destination));
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
    const auto parts = splitZip(fixture.root / "sevenzip-fake-split", "fake.zip", bytes, bytes.size() / 2);
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(parts.front())));
    wait(service, pump);
    TEST_CHECK(service.error() && !service.catalog());
    const auto source = fixture.root / "sevenzip-cancel.7z";
    write(source, bytes);
    const auto output = fixture.root / "sevenzip-cancelled";
    TEST_CHECK(service.extract(location(source), location(output)));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
  }
  void testSevenZipBoundaries(const Fixture& fixture, Pump& pump) {
    if (!archive_liblzma_version()) {
      return;
    }
    const auto formats = luciole::archiveFormats();
    const auto format =
        std::ranges::find(formats, luciole::ArchiveFormat::SevenZip, &luciole::ArchiveFormatInfo::format);
    TEST_CHECK(
        format != formats.end() && format->suffix == ".7z" && format->compression == luciole::ArchiveCompression::None
    );
    luciole::ArchiveService passwordService;
    TEST_CHECK(!passwordService.compress(
        {location(fixture.root / "unused")}, location(fixture.root / "password.7z"), format->format, "unsupported"
    ));
    const std::vector<std::vector<Entry>> rejected{
        {{"../sevenzip-escaped", "bad", {}}},
        {{"/sevenzip-absolute", "bad", {}}},
        {{"link", {}, "../sevenzip-escaped", AE_IFLNK}},
        {{"a", {}, "b", AE_IFLNK}, {"b", {}, "a", AE_IFLNK}},
        {{"same", "first", {}}, {"same", "second", {}}},
        {{"link", {}, "file", AE_IFLNK}, {"link/child", "bad", {}}}
    };
    for (std::size_t i = 0; i < rejected.size(); ++i) {
      const auto source = fixture.root / ("hostile-sevenzip-" + std::to_string(i) + ".7z");
      makeArchive(source, rejected[i], ARCHIVE_FORMAT_7ZIP);
      const auto output = fixture.root / ("hostile-sevenzip-output-" + std::to_string(i));
      luciole::ArchiveService service;
      TEST_CHECK(service.extract(location(source), location(output)));
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
    }
    TEST_CHECK(!fs::exists(fixture.root / "sevenzip-escaped"));
  }
  std::string xz(std::string_view bytes) {
    std::string encoded(lzma_stream_buffer_bound(bytes.size()), '\0');
    std::size_t position = 0;
    TEST_CHECK(
        lzma_easy_buffer_encode(
            0, LZMA_CHECK_CRC64, nullptr, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
            reinterpret_cast<std::uint8_t*>(encoded.data()), &position, encoded.size()
        )
        == LZMA_OK
    );
    encoded.resize(position);
    return encoded;
  }
  std::string largeXzDictionary(std::string encoded) {
    TEST_CHECK(encoded.size() > 16);
    lzma_block block{};
    std::array<lzma_filter, LZMA_FILTERS_MAX + 1> filters{};
    block.version = 0;
    block.filters = filters.data();
    block.header_size = lzma_block_header_size_decode(static_cast<unsigned char>(encoded[12]));
    TEST_CHECK(block.header_size <= encoded.size() - 12);
    TEST_CHECK(
        lzma_block_header_decode(&block, nullptr, reinterpret_cast<const std::uint8_t*>(encoded.data() + 12)) == LZMA_OK
    );
    TEST_CHECK(filters[0].id == LZMA_FILTER_LZMA2 && filters[1].id == LZMA_VLI_UNKNOWN);
    auto* options = static_cast<lzma_options_lzma*>(filters[0].options);
    options->dict_size = 256U * 1024U * 1024U;
    TEST_CHECK(lzma_raw_decoder_memusage(filters.data()) > 128U * 1024U * 1024U);
    TEST_CHECK(lzma_block_header_encode(&block, reinterpret_cast<std::uint8_t*>(encoded.data() + 12)) == LZMA_OK);
    lzma_filters_free(filters.data(), nullptr);
    return encoded;
  }
  void testXzTar(const Fixture& fixture, Pump& pump) {
    TEST_CHECK(luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Xz));
    const auto formats = luciole::archiveFormats();
    const auto format = std::ranges::find(formats, luciole::ArchiveFormat::TarXz, &luciole::ArchiveFormatInfo::format);
    TEST_CHECK((format != formats.end()) == (archive_liblzma_version() != nullptr));
    if (format != formats.end()) {
      TEST_CHECK(format->compression == luciole::ArchiveCompression::Xz && format->suffix == ".tar.xz");
    }
    const auto plain = fixture.root / "xz-source.tar";
    tar(plain, {{"file", "XZ payload", {}}, {"alias", {}, "file", AE_IFREG}, {"link", {}, "file", AE_IFLNK}});
    const auto data = read(plain);
    const auto encoded = xz(data);
    if (format != formats.end()) {
      const auto source = fixture.root / "xz-writer-payload";
      const auto archive = fixture.root / "xz-writer.tar.xz";
      write(source, "independent reader");
      luciole::ArchiveService service;
      TEST_CHECK(!service.compress({location(source)}, location(archive), format->format, "unsupported"));
      TEST_CHECK(service.compress({location(source)}, location(archive), format->format));
      wait(service, pump);
      TEST_CHECK(!service.error());
      const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(
          archive_read_new(), &archive_read_free
      );
      TEST_CHECK(archive_read_support_filter_xz(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_support_format_tar(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_open_filename(reader.get(), archive.c_str(), 65536) == ARCHIVE_OK);
      struct archive_entry* header = nullptr;
      TEST_CHECK(archive_read_next_header(reader.get(), &header) == ARCHIVE_OK);
      TEST_CHECK(archive_filter_code(reader.get(), 0) == ARCHIVE_FILTER_XZ);
      std::array<char, 64> bytes{};
      const auto count = archive_read_data(reader.get(), bytes.data(), bytes.size());
      TEST_CHECK(count == 18 && std::string_view(bytes.data(), static_cast<std::size_t>(count)) == read(source));
    }
    for (unsigned variant = 0; variant < 6; ++variant) {
      const auto source = fixture.root / ("independent-xz-" + std::to_string(variant) + ".txz");
      // Independent encoder, legal padding and multiple streams, including a
      // frame boundary inside a TAR header rather than between TAR members.
      const auto bytes = variant == 0 ? encoded
          : variant == 1              ? encoded + std::string(4, '\0')
          : variant == 2 ? xz(std::string_view(data).substr(0, 700)) + xz(std::string_view(data).substr(700))
          : variant == 3 ? xz({}) + encoded + xz({})
          : variant == 4 ? largeXzDictionary(encoded)
                         : xz({}) + largeXzDictionary(encoded) + xz({});
      write(source, bytes);
      luciole::ArchiveService service;
      TEST_CHECK(service.inspect(location(source)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog() && service.catalog()->entries.size() == 3);
      const auto known = service.catalog();
      const auto output = fixture.root / ("xz-extracted-" + std::to_string(variant));
      TEST_CHECK(service.extract(location(source), location(output), {}, known));
      wait(service, pump);
      TEST_CHECK(!service.error() && read(output / "file") == "XZ payload" && read(output / "alias") == "XZ payload");
      TEST_CHECK(fs::is_symlink(output / "link") && fs::read_symlink(output / "link") == "file");
      const auto selected = fixture.root / ("xz-selected-" + std::to_string(variant));
      TEST_CHECK(service.extract(location(source), location(selected), {"alias"}, known));
      wait(service, pump);
      TEST_CHECK(!service.error() && read(selected / "alias") == "XZ payload" && !fs::exists(selected / "file"));
      write(source, bytes);
      const auto stale = fixture.root / ("xz-stale-" + std::to_string(variant));
      TEST_CHECK(service.extract(location(source), location(stale), {}, known));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Changed && !fs::exists(stale));
    }
  }
  void testXzFailures(const Fixture& fixture, Pump& pump) {
    const auto plain = fixture.root / "xz-boundary.tar";
    tar(plain, {{"payload", std::string(129536, 'x'), {}}});
    const auto data = read(plain);
    const auto valid = xz(data);
    const auto hostile = fixture.root / "xz-hostile.tar";
    tar(hostile, {{"../escaped-xz", "bad", {}}});
    const auto link = fixture.root / "xz-hostile-link.tar";
    tar(link, {{"link", {}, "../escaped-xz", AE_IFLNK}});
    auto corruptLargeDictionary = largeXzDictionary(valid);
    corruptLargeDictionary[corruptLargeDictionary.size() - 13] ^= 0x01;
    std::vector<std::string> rejected{
        valid.substr(0, valid.size() - 1),
        valid.substr(0, valid.size() / 2),
        valid + std::string(1, '\0'),
        valid + "garbage",
        corruptLargeDictionary,
        xz(data + std::string(4U * 1024U * 1024U, '\0')),
        xz(read(hostile)),
        xz(read(link)),
        valid
    };
    auto badCheck = valid;
    badCheck[badCheck.size() - 13] ^= 0x01;
    rejected.push_back(std::move(badCheck));
    std::uint32_t backward = 0;
    for (unsigned i = 0; i < 4; ++i) {
      backward |= static_cast<std::uint32_t>(static_cast<unsigned char>(valid[valid.size() - 8 + i])) << (8 * i);
    }
    auto badDataCheck = valid;
    const auto indexSize = (static_cast<std::size_t>(backward) + 1) * 4;
    TEST_CHECK(indexSize + 13 < valid.size());
    badDataCheck[valid.size() - indexSize - 13] ^= 0x01;
    rejected.push_back(std::move(badDataCheck));
    rejected.push_back(valid + corruptLargeDictionary);
    rejected.push_back(valid + valid.substr(0, valid.size() - 1));
    for (std::size_t i = 0; i < rejected.size(); ++i) {
      const auto source = fixture.root / ("bad-xz-" + std::to_string(i) + ".tar.xz");
      write(source, rejected[i]);
      luciole::ArchiveLimits limits;
      if (i == 5) {
        limits.maxExpandedBytes = data.size();
        limits.maxMetadataBytes = 256;
        limits.maxEntries = 4;
      } else if (i == 8) {
        limits.maxExpandedBytes = 1024;
      }
      luciole::ArchiveService service(limits);
      TEST_CHECK(service.inspect(location(source)));
      wait(service, pump);
      TEST_CHECK(service.error() && !service.catalog());
      if (i == 4) {
        TEST_CHECK(service.error()->kind == luciole::FileErrorKind::InvalidArgument);
      } else if (i == 5) {
        TEST_CHECK(service.error()->kind == luciole::FileErrorKind::Unsupported);
        TEST_CHECK(service.error()->message.contains("decoded-container"));
      }
      const auto destination = fixture.root / ("xz-rejected-" + std::to_string(i));
      TEST_CHECK(service.extract(location(source), location(destination)));
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(destination) && fs::is_empty(fixture.root / "tmp"));
    }
    TEST_CHECK(!fs::exists(fixture.root / "escaped-xz"));
    const auto split = splitZip(fixture.root / "xz-fake-split", "fake.zip", valid, valid.size() / 2);
    luciole::ArchiveService splitService;
    TEST_CHECK(splitService.inspect(location(split.front())));
    wait(splitService, pump);
    TEST_CHECK(splitService.error() && !splitService.catalog());
    const auto source = fixture.root / "xz-cancel.tar.xz";
    write(source, valid);
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(fixture.root / "xz-cancelled")));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "xz-cancelled") && fs::is_empty(fixture.root / "tmp"));
  }
  void testXzMemberFromLargeArchive(const Fixture& fixture, Pump& pump) {
    if (!archive_liblzma_version()) {
      return;
    }
    const auto source = fixture.root / "large-xz.tar.xz";
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_add_filter_xz(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_filter_option(writer.get(), "xz", "compression-level", "0") == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), source.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), "large");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_size(entry.get(), 130U * 1024U * 1024U);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    const std::array<char, 65536> chunk{};
    for (unsigned i = 0; i < 130U * 16U; ++i) {
      TEST_CHECK(archive_write_data(writer.get(), chunk.data(), chunk.size()) == static_cast<la_ssize_t>(chunk.size()));
    }
    archive_entry_set_pathname(entry.get(), "small");
    archive_entry_set_size(entry.get(), 3);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_data(writer.get(), "abc", 3) == 3);
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
    luciole::ArchiveLimits limits;
    limits.maxExpandedBytes = 256U * 1024U * 1024U;
    limits.maxMetadataBytes = 256;
    limits.maxEntries = 4;
    luciole::ArchiveService service(limits);
    TEST_CHECK(service.inspect(location(source)));
    wait(service, pump);
    TEST_CHECK(!service.error() && service.catalog());
    TEST_CHECK(service.openMember("small"));
    wait(service, pump);
    const auto opened = service.takeOpenedFile();
    TEST_CHECK(!service.error() && opened && read(*opened->nativePath()) == "abc");
    service.shutdown();
    wait(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testXzWorkerBoundaries(const Fixture& fixture) {
    const auto plain = fixture.root / "xz-worker.tar";
    tar(plain, {{"file", std::string(256U * 1024U, 'x'), {}}});
    const auto source = fixture.root / "xz-worker.tar.xz";
    write(source, xz(read(plain)));
    luciole::ArchiveLimits limits;
    luciole::CancelToken cancel;
    luciole::archive_work::Progress progress;
    auto work = luciole::archive_work::workFor(limits, cancel, progress);
    luciole::archive_work::Input input(location(source), work);
    TEST_CHECK(luciole::archive_work::XzInput::recognizes(input));
    luciole::archive_work::XzInput decoder(input, work);
    std::array<char, 512> bytes{};
    TEST_CHECK(decoder.read(bytes) == bytes.size());
    cancel.cancel();
    bool cancelled = false;
    try {
      decoder.finish();
    } catch (const luciole::FileErrorException& error) {
      cancelled = error.error().kind == luciole::FileErrorKind::Cancelled;
    }
    TEST_CHECK(cancelled);
  }
  int compressedWithoutTools() {
    Fixture fixture;
    Pump pump;
    const auto source = fixture.root / "payload";
    write(source, "no helper program");
    for (const auto& item : luciole::archiveFormats()) {
      const auto archive = fixture.root / ("no-tools" + std::string(item.suffix));
      const auto destination = fixture.root / ("no-tools" + std::to_string(static_cast<int>(item.format)));
      luciole::ArchiveService service;
      TEST_CHECK(service.compress({location(source)}, location(archive), item.format));
      wait(service, pump);
      TEST_CHECK(!service.error());
      TEST_CHECK(service.extract(location(archive), location(destination)));
      wait(service, pump);
      TEST_CHECK(!service.error() && read(destination / "payload") == "no helper program");
    }
    return 0;
  }
  int encryptedWithoutTools() {
    Fixture fixture;
    Pump pump;
    const auto source = fixture.root / "source.7z", destination = fixture.root / "output";
    const auto* reference =
        std::ranges::find(sevenzip_fixtures::archives, "encrypted-data", &sevenzip_fixtures::Fixture::name);
    TEST_CHECK(reference != sevenzip_fixtures::archives.end());
    write(source, fixtureBytes(*reference));
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Unavailable && !service.catalog());
    TEST_CHECK(service.extract(location(source), location(destination)));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == luciole::FileErrorKind::Unavailable);
    TEST_CHECK(!service.passwordRequest() && !fs::exists(destination) && fs::is_empty(fixture.root / "tmp"));
    return 0;
  }
  int boundedWriteFailure() {
    Fixture fixture;
    const auto source = fixture.root / "large.txt";
    std::string data(8192, '\0');
    std::uint32_t seed = 17;
    for (auto& byte : data) {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      byte = static_cast<char>(seed & 255U);
    }
    write(source, data);
    std::signal(SIGXFSZ, SIG_IGN);
    const rlimit limit{1024, 1024};
    TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
    const luciole::CancelToken cancel;
    for (const auto& format : luciole::archiveFormats()) {
      luciole::archive_work::Progress progress;
      luciole::archive_work::Passwords passwords;
      if (format.format == luciole::ArchiveFormat::Zip) {
        passwords.values.emplace_back("test password");
      }
      bool failed = false;
      try {
        auto prepared = luciole::archive_work::compress(
            {location(source)}, location(fixture.root / "failed-output"), format.format, {}, cancel, progress, passwords
        );
        (void)luciole::archive_work::cleanup({prepared.stage});
      } catch (const std::exception&) {
        failed = true;
      }
      TEST_CHECK(failed && !fs::exists(fixture.root / "failed-output") && fs::is_empty(fixture.root / "tmp"));
    }
    return 0;
  }
  void testCancelAndDestruction(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "cancel.txt", output = fixture.root / "cancel.zip";
    write(source, std::string(1024 * 1024, 'x'));
    luciole::ArchiveService service;
    TEST_CHECK(service.compress({location(source)}, location(output), luciole::ArchiveFormat::Zip));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(!fs::exists(output) && service.error());
    write(output, "keep me");
    auto owner = std::make_unique<luciole::ArchiveService>();
    TEST_CHECK(owner->compress({location(source)}, location(output), luciole::ArchiveFormat::Zip));
    pump.until([&] { return owner->conflict().has_value(); });
    owner.reset();
    pump.until([&] { return fs::is_empty(fixture.root / "tmp"); });
    TEST_CHECK(read(output) == "keep me");
  }
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--large-sparse-fixture") {
      return makeLargeSparseFixture(argv[2]);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--bounded-write-failure") {
      return boundedWriteFailure();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--compressed-without-tools") {
      return compressedWithoutTools();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--encrypted-without-tools") {
      return encryptedWithoutTools();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--bounded-encrypted-write-failure") {
      return boundedEncryptedWriteFailure();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--sevenzip-provider-failures") {
      return sevenZipProviderFailures();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--header-without-tools") {
      return headerWithoutTools();
    }
    Fixture fixture;
    Pump pump;
    testLiteralSourceLinks(fixture, pump);
    testRarProviders(fixture, pump);
    testRarStoredChunks(fixture, pump);
    testRarFailures(fixture, pump);
    testRarMainHeaders(fixture, pump);
    testSevenZipProviders(fixture, pump);
    testSevenZipLiteralNames(fixture, pump);
    testSevenZipLiteralNames(fixture, pump, true);
    testEncryptedHeaderFailures(fixture, pump);
    testSevenZipCatalogSource(fixture);
    testSevenZipAstral(fixture, pump);
    testSevenZipCompressedHeader(fixture, pump);
    testSevenZipClearDataHeader(fixture, pump);
    testEncryptedSevenZipBoundaries(fixture, pump);
    testSevenZipFailures(fixture, pump);
    testSevenZipBoundaries(fixture, pump);
    testXzTar(fixture, pump);
    testXzFailures(fixture, pump);
    testXzWorkerBoundaries(fixture);
    testXzMemberFromLargeArchive(fixture, pump);
    testCompressedTarProviders(fixture, pump);
    testCompressedTarFailures(fixture, pump);
    testCompressedPaddingLimit(fixture, pump);
    testRoundTrips(fixture, pump);
    testNativeJobCapacity(fixture);
    testLargeSparseMember(fixture);
    testLargeCatalogue(fixture);
    testConflicts(fixture, pump);
    testHostile(fixture, pump);
    testLimitsAndTruncation(fixture, pump);
    testLinksAndSelection(fixture, pump);
    testHardlinkOutputLimit(fixture, pump);
    testEncryptedRead(fixture, pump);
    testEncryptedCreation(fixture, pump);
    testPasswordCancellationAndChanges(fixture, pump);
    testEncryptedBoundaries(fixture, pump);
    testEncryptedCorruption(fixture, pump);
    testSplitInput(fixture);
    testSplitZip(fixture, pump);
    testSplitFailures(fixture, pump);
    testSplitPasswordAndChanges(fixture, pump);
    testCancelAndDestruction(fixture, pump);
    luciole::GioPtr<GSubprocess> child(
        g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "/proc/self/exe", "--bounded-write-failure", nullptr)
    );
    TEST_CHECK(child && g_subprocess_wait_check(child.get(), nullptr, nullptr));
    const luciole::GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE));
    g_subprocess_launcher_setenv(launcher.get(), "PATH", "/luciole-no-helper-tools", TRUE);
    luciole::GioPtr<GSubprocess> native(
        g_subprocess_launcher_spawn(launcher.get(), nullptr, "/proc/self/exe", "--compressed-without-tools", nullptr)
    );
    TEST_CHECK(native && g_subprocess_wait_check(native.get(), nullptr, nullptr));
    luciole::GioPtr<GSubprocess> missing(
        g_subprocess_launcher_spawn(launcher.get(), nullptr, "/proc/self/exe", "--encrypted-without-tools", nullptr)
    );
    TEST_CHECK(missing && g_subprocess_wait_check(missing.get(), nullptr, nullptr));
    luciole::GioPtr<GSubprocess> missingHeader(
        g_subprocess_launcher_spawn(launcher.get(), nullptr, "/proc/self/exe", "--header-without-tools", nullptr)
    );
    TEST_CHECK(missingHeader && g_subprocess_wait_check(missingHeader.get(), nullptr, nullptr));
    luciole::GioPtr<GSubprocess> failures(
        g_subprocess_launcher_spawn(launcher.get(), nullptr, "/proc/self/exe", "--sevenzip-provider-failures", nullptr)
    );
    TEST_CHECK(failures && g_subprocess_wait_check(failures.get(), nullptr, nullptr));
    luciole::GioPtr<GSubprocess> writeFailure(g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, nullptr, "/proc/self/exe", "--bounded-encrypted-write-failure", nullptr
    ));
    TEST_CHECK(writeFailure && g_subprocess_wait_check(writeFailure.get(), nullptr, nullptr));
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    std::println("archive checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "archive test: {}", error.what());
    return 1;
  }
}
