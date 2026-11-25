#include "browser/directory_model.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "operations/create_service.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/fs.h>
#include <print>
#include <stdexcept>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/luciole-create-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()) != nullptr);
      root = path;
    }
    ~Fixture() {
      std::error_code error;
      fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, error);
      fs::remove_all(root, error);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        m_source.dispatch(fds, first);
      }
    }
    void prepare(
        luciole::CreateService& service, const fs::path& path, luciole::CreateKind kind = luciole::CreateKind::Folder
    ) {
      TEST_CHECK(service.prepare(location(path), kind));
      until([&] { return !service.busy(); });
      TEST_CHECK(service.canCreate());
    }
    void create(luciole::CreateService& service, const std::string& name) {
      TEST_CHECK(service.create(name));
      until([&] { return !service.busy(); });
    }
    void replay(luciole::CreateService& service) {
      TEST_CHECK(service.snapshot().receipt);
      TEST_CHECK(service.replay(service.snapshot().receipt));
      until([&] { return !service.busy(); });
    }
    luciole::TemplateListing templates(luciole::TemplateCatalog& catalog) {
      std::optional<luciole::TemplateListing> result;
      catalog.load([&](auto listing) { result = std::move(listing); });
      until([&] { return result.has_value(); });
      return std::move(*result);
    }

  private:
    GlibPollSource m_source;
  };
  // Where renameat2 flags are rejected (NFS, many FUSE filesystems), no-replace falls back to a check and a plain
  // rename, so racing creates of one name can both succeed.
  bool atomicNoReplace(const fs::path& directory) {
    const auto from = directory / ".rename-probe-from";
    const auto to = directory / ".rename-probe-to";
    std::ofstream(from) << "";
    const bool supported =
        ::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0;
    std::error_code ignored;
    fs::remove(from, ignored);
    fs::remove(to, ignored);
    return supported;
  }
  std::string contents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  luciole::DirectoryEntry linkEntry(const fs::path& path) {
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(location(path)).get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info);
    const luciole::GString display(g_filename_display_name(path.filename().c_str()));
    luciole::DirectoryEntry result{
        .id = 1,
        .place = location(path),
        .name = path.filename().native(),
        .type = luciole::fileType(g_file_info_get_file_type(info.get())),
        .symlink = g_file_info_get_is_symlink(info.get()) != FALSE,
        .size = {},
        .modifiedSeconds = {}
    };
    result.setDisplayName(display.get());
    result.setIdentity(luciole::readFileIdentity(info.get()));
    return result;
  }
  void prepareLink(Pump& pump, luciole::CreateService& service, const fs::path& path) {
    TEST_CHECK(service.prepareLink(linkEntry(path)));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canCreate());
  }
  void createLink(
      Pump& pump, luciole::CreateService& service, const fs::path& parent, const std::string& name,
      luciole::CreateKind kind = luciole::CreateKind::SymbolicLink
  ) {
    TEST_CHECK(service.createLink(location(parent), name, kind));
    pump.until([&] { return !service.busy(); });
  }
  void createsLinks(Pump& pump) {
    using K = luciole::CreateKind;
    Fixture fixture;
    const auto source = fixture.root / (std::string("source #%-") + static_cast<char>(0xff));
    std::ofstream(source) << "original";
    fs::create_directory(fixture.root / "output");
    const auto output = fixture.root / "output";
    luciole::CreateService service;
    prepareLink(pump, service, source);
    TEST_CHECK(!service.create("wrong API") && !service.createLink(location(output), "bad", K::Folder));
    createLink(pump, service, output, "Symbolic 文");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded
        && service.snapshot().destination == location(output / "Symbolic 文")
    );
    TEST_CHECK(fs::read_symlink(output / "Symbolic 文") == source && contents(output / "Symbolic 文") == "original");
    prepareLink(pump, service, source);
    std::ofstream(source) << "edited while choosing";
    createLink(pump, service, output, "Hard 文", K::HardLink);
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded && fs::equivalent(source, output / "Hard 文")
    );
    std::ofstream(output / "Hard 文") << "shared contents";
    TEST_CHECK(contents(source) == "shared contents");
    fs::remove(source);
    TEST_CHECK(
        contents(output / "Hard 文") == "shared contents"
        && fs::is_symlink(output / "Symbolic 文")
        && !fs::exists(output / "Symbolic 文")
    );
    fs::create_directory(fixture.root / "folder");
    prepareLink(pump, service, fixture.root / "folder");
    createLink(pump, service, output, "refuse folder", K::HardLink);
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && !fs::exists(output / "refuse folder"));
    createLink(pump, service, output, "folder link");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded && fs::is_directory(output / "folder link")
    );
    fs::create_symlink("missing", fixture.root / "broken");
    prepareLink(pump, service, fixture.root / "broken");
    createLink(pump, service, output, "refuse symlink", K::HardLink);
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && !fs::is_symlink(output / "refuse symlink"));
    createLink(pump, service, output, "link to broken");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded
        && fs::read_symlink(output / "link to broken") == fixture.root / "broken"
    );
    TEST_CHECK(::mkfifo((fixture.root / "pipe").c_str(), 0600) == 0);
    TEST_CHECK(!service.prepareLink(linkEntry(fixture.root / "pipe")));
  }
  void linkCollisionsAndErrors(Pump& pump) {
    using K = luciole::CreateKind;
    Fixture fixture;
    const auto source = fixture.root / "source";
    std::ofstream(source) << "keep source";
    fs::create_directory(fixture.root / "output");
    const auto output = fixture.root / "output";
    std::ofstream(output / "file") << "keep existing";
    fs::create_directory(output / "folder");
    fs::create_symlink("missing", output / "link");
    TEST_CHECK(::mkfifo((output / "pipe").c_str(), 0600) == 0);
    luciole::CreateService service;
    prepareLink(pump, service, source);
    for (const auto kind : {K::SymbolicLink, K::HardLink}) {
      for (const auto* name : {"file", "folder", "link", "pipe"}) {
        createLink(pump, service, output, name, kind);
        TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && service.canCreate());
      }
      for (const auto& destination : {fixture.root / "missing", output / "file"}) {
        createLink(pump, service, destination, "no effect", kind);
        TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && service.canCreate());
      }
    }
    TEST_CHECK(
        contents(source) == "keep source"
        && contents(output / "file") == "keep existing"
        && fs::is_directory(output / "folder")
        && fs::is_symlink(output / "link")
        && fs::is_fifo(output / "pipe")
    );
    TEST_CHECK(!service.createLink(location(output), "../escape", K::SymbolicLink));
    createLink(pump, service, output, std::string(10000, 'x'));
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error);
    TEST_CHECK(service.createLink(
        luciole::Location::fromUri("sftp://example.invalid/destination"), "native only", K::SymbolicLink
    ));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error);
    createLink(pump, service, output, "retry", K::HardLink);
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded && fs::equivalent(source, output / "retry")
    );
    if (::geteuid() != 0) {
      prepareLink(pump, service, source);
      fs::permissions(output, fs::perms::owner_read | fs::perms::owner_exec);
      createLink(pump, service, output, "denied");
      fs::permissions(output, fs::perms::owner_all);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && !fs::exists(output / "denied"));
    }
  }
  void staleLinksAndCancellation(Pump& pump) {
    using K = luciole::CreateKind;
    Fixture fixture;
    fs::create_directory(fixture.root / "parent");
    fs::create_directory(fixture.root / "output");
    const auto path = fixture.root / "parent/source";
    const auto output = fixture.root / "output";
    std::ofstream(path) << "original";
    const auto captured = linkEntry(path);
    fs::rename(path, fixture.root / "held");
    std::ofstream(path) << "replacement";
    luciole::CreateService service;
    TEST_CHECK(service.prepareLink(captured));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && !service.canCreate());
    for (const auto kind : {K::SymbolicLink, K::HardLink}) {
      prepareLink(pump, service, path);
      fs::rename(path, fixture.root / "saved");
      std::ofstream(path) << "changed again";
      createLink(pump, service, output, "refused", kind);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(output));
      fs::remove(fixture.root / "saved");
    }
    prepareLink(pump, service, path);
    fs::rename(fixture.root / "parent", fixture.root / "old parent");
    fs::create_directory(fixture.root / "parent");
    std::ofstream(path) << "new parent";
    createLink(pump, service, output, "refused parent");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(output));
    auto entry = linkEntry(path);
    TEST_CHECK(service.prepareLink(entry));
    TEST_CHECK(!service.prepareLink(entry));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Cancelled && !service.canCreate());
    prepareLink(pump, service, path);
    TEST_CHECK(service.createLink(location(output), "maybe", K::HardLink));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == luciole::CreateStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == luciole::CreateStatus::Cancelled)
        && fs::exists(output / "maybe") == committed
    );
    auto dying = std::make_unique<luciole::CreateService>();
    bool notified = false;
    dying->setChangedCallback([&] { notified = true; });
    TEST_CHECK(dying->prepareLink(entry));
    notified = false;
    dying.reset();
    prepareLink(pump, service, path);
    TEST_CHECK(!notified);
  }
  void linksAcrossFilesystems(Pump& pump) {
    Fixture fixture;
    const auto source = fixture.root / "source";
    std::ofstream(source) << "cross filesystem";
    std::string temporary = "/dev/shm/luciole-link-XXXXXX";
    if (!::mkdtemp(temporary.data())) {
      std::println("cross-filesystem link fixture unavailable");
      return;
    }
    struct Cleanup {
      fs::path path;
      ~Cleanup() {
        std::error_code error;
        fs::remove_all(path, error);
      }
    } cleanup{temporary};
    struct stat from{}, to{};
    TEST_CHECK(::stat(source.c_str(), &from) == 0 && ::stat(temporary.c_str(), &to) == 0);
    if (from.st_dev == to.st_dev) {
      return;
    }
    luciole::CreateService service;
    prepareLink(pump, service, source);
    createLink(pump, service, temporary, "hard", luciole::CreateKind::HardLink);
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(temporary));
    createLink(pump, service, temporary, "symbolic");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded
        && contents(fs::path(temporary) / "symbolic") == "cross filesystem"
    );
  }
  luciole::DocumentTemplate documentTemplate(const fs::path& path) {
    struct stat info{};
    TEST_CHECK(::lstat(path.c_str(), &info) == 0);
    return {location(path), path.filename().native(), path.filename().native(), luciole::templateRevision(info)};
  }
  void prepareDocument(Pump& pump, luciole::CreateService& service, const fs::path& parent, const fs::path& source) {
    TEST_CHECK(service.prepareTemplate(location(parent), documentTemplate(source)));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canCreate());
  }
  void templateCatalog(Pump& pump, const fs::path& home) {
    const auto root = home / "Templates";
    fs::create_directories(root / "Work");
    fs::create_directory(root / ".hidden");
    std::ofstream(root / "Letter 文.txt") << "letter";
    std::ofstream(root / "Work/report.txt") << "report";
    std::ofstream(root / ".hidden/secret") << "hidden";
    std::ofstream(root / ".hidden-file") << "hidden";
    fs::create_directory_symlink(root, root / "loop");
    fs::create_symlink("missing", root / "broken");
    TEST_CHECK(::mkfifo((root / "pipe").c_str(), 0600) == 0);
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    std::ofstream(root / raw) << "raw";
    luciole::TemplateCatalog catalog;
    auto listing = pump.templates(catalog);
    TEST_CHECK(!listing.error && !listing.truncated && listing.items.size() == 3);
    const auto report = std::ranges::find(listing.items, "Work/report.txt", &luciole::DocumentTemplate::label);
    TEST_CHECK(report != listing.items.end() && report->location == location(root / "Work/report.txt"));
    TEST_CHECK(std::ranges::find(listing.items, raw, &luciole::DocumentTemplate::name) != listing.items.end());
    std::ofstream(root / "added.txt") << "new";
    TEST_CHECK(pump.templates(catalog).items.size() == 4);
    bool obsolete = false;
    catalog.load([&](const auto&) { obsolete = true; });
    catalog.cancel();
    TEST_CHECK(pump.templates(catalog).items.size() == 4 && !obsolete);
    fs::rename(root, home / "Saved templates");
    TEST_CHECK(pump.templates(catalog).error.has_value());
    fs::create_directory_symlink(home, root);
    TEST_CHECK(pump.templates(catalog).items.empty() && pump.templates(catalog).error.has_value());
    fs::remove(root);
    fs::create_directory(root);
    TEST_CHECK(pump.templates(catalog).items.empty() && !pump.templates(catalog).error);
    for (int index = 0; index < 260; ++index) {
      std::ofstream(root / ("Document " + std::to_string(index))) << "bounded";
    }
    listing = pump.templates(catalog);
    TEST_CHECK(listing.truncated && listing.items.size() == 256);
  }
  void createsDocuments(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "output");
    const auto source = fixture.root / (std::string("template-") + static_cast<char>(0xff) + ".bin");
    const std::string binary("contents\0with bytes\xff", 20);
    std::ofstream(source, std::ios::binary).write(binary.data(), static_cast<std::streamsize>(binary.size()));
    luciole::CreateService service;
    prepareDocument(pump, service, fixture.root / "output", source);
    pump.create(service, "Document 文.bin");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded);
    TEST_CHECK(contents(source) == binary && contents(fixture.root / "output/Document 文.bin") == binary);
    TEST_CHECK(std::distance(fs::directory_iterator(fixture.root / "output"), fs::directory_iterator{}) == 1);
    prepareDocument(pump, service, fixture.root / "output", source);
    pump.create(service, "Document 文.bin");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && service.canCreate());
    TEST_CHECK(contents(fixture.root / "output/Document 文.bin") == binary);
    fs::create_symlink("missing", fixture.root / "output/link");
    fs::create_directory(fixture.root / "output/folder");
    TEST_CHECK(::mkfifo((fixture.root / "output/pipe").c_str(), 0600) == 0);
    for (const auto* name : {"link", "folder", "pipe"}) {
      pump.create(service, name);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && service.canCreate());
    }
    TEST_CHECK(fs::is_symlink(fixture.root / "output/link") && fs::is_fifo(fixture.root / "output/pipe"));
    TEST_CHECK(!service.create("../escaped"));
    pump.create(service, "retry.bin");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded);
    TEST_CHECK(contents(fixture.root / "output/retry.bin") == binary);
    std::ofstream(source, std::ios::trunc);
    prepareDocument(pump, service, fixture.root / "output", source);
    pump.create(service, "empty.txt");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded
        && fs::file_size(fixture.root / "output/empty.txt") == 0
    );
    for (const auto& entry : fs::directory_iterator(fixture.root / "output")) {
      TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-document-"));
    }
  }
  void staleTemplatesAndCancellation(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "output");
    const auto source = fixture.root / "source.txt";
    std::ofstream(source) << "original";
    const auto selected = documentTemplate(source);
    std::ofstream(source) << "changed";
    luciole::CreateService service;
    TEST_CHECK(service.prepareTemplate(location(fixture.root / "output"), selected));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!service.canCreate() && service.snapshot().status == luciole::CreateStatus::Error);
    prepareDocument(pump, service, fixture.root / "output", source);
    fs::rename(source, fixture.root / "held");
    fs::create_symlink("held", source);
    pump.create(service, "refuse-link.txt");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(fixture.root / "output"));
    fs::remove(source);
    fs::rename(fixture.root / "held", source);
    prepareDocument(pump, service, fixture.root / "output", source);
    fs::rename(source, fixture.root / "held");
    TEST_CHECK(::mkfifo(source.c_str(), 0600) == 0);
    pump.create(service, "refuse-pipe.txt");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(fixture.root / "output"));
    fs::remove(source);
    fs::rename(fixture.root / "held", source);
    prepareDocument(pump, service, fixture.root / "output", source);
    std::ofstream(source) << "modified after preparation";
    pump.create(service, "refuse-change.txt");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(fixture.root / "output"));
    prepareDocument(pump, service, fixture.root / "output", source);
    fs::rename(fixture.root / "output", fixture.root / "previous");
    fs::create_directory(fixture.root / "output");
    pump.create(service, "refuse-parent.txt");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(fixture.root / "previous"));
    prepareDocument(pump, service, fixture.root / "output", source);
    TEST_CHECK(service.create("maybe.txt"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool published = service.snapshot().status == luciole::CreateStatus::Succeeded;
    TEST_CHECK(published || service.snapshot().status == luciole::CreateStatus::Cancelled);
    TEST_CHECK(fs::exists(fixture.root / "output/maybe.txt") == published);
    TEST_CHECK(
        std::distance(fs::directory_iterator(fixture.root / "output"), fs::directory_iterator{}) == (published ? 1 : 0)
    );
    TEST_CHECK(!service.prepare(location(fixture.root), luciole::CreateKind::Document));
  }
  void createsFolders(Pump& pump) {
    Fixture fixture;
    luciole::CreateService service;
    pump.prepare(service, fixture.root);
    pump.create(service, "Documents 文");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded && fs::is_empty(fixture.root / "Documents 文")
    );
    TEST_CHECK(service.snapshot().destination == location(fixture.root / "Documents 文"));
    pump.prepare(service, fixture.root);
    pump.create(service, ".hidden");
    TEST_CHECK(fs::is_directory(fixture.root / ".hidden"));
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    fs::create_directory(fixture.root / raw);
    pump.prepare(service, fixture.root / raw);
    pump.create(service, "child");
    TEST_CHECK(fs::is_directory(fixture.root / raw / "child"));
    fs::create_directory_symlink(fixture.root, fixture.root / "alias");
    pump.prepare(service, fixture.root / "alias");
    pump.create(service, "through link");
    TEST_CHECK(fs::is_directory(fixture.root / "through link"));
  }
  void collisionsAndRetry(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "folder");
    std::ofstream(fixture.root / "folder/marker") << "preserve";
    std::ofstream(fixture.root / "file") << "data";
    fs::create_symlink("missing", fixture.root / "link");
    luciole::CreateService service;
    pump.prepare(service, fixture.root);
    for (const auto* name : {"folder", "file", "link"}) {
      pump.create(service, name);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && service.canCreate());
    }
    TEST_CHECK(
        fs::exists(fixture.root / "folder/marker")
        && fs::is_regular_file(fixture.root / "file")
        && fs::read_symlink(fixture.root / "link") == "missing"
    );
    pump.create(service, "available");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded);
    luciole::CreateService other;
    pump.prepare(service, fixture.root);
    pump.prepare(other, fixture.root);
    TEST_CHECK(service.create("racing") && other.create("racing"));
    pump.until([&] { return !service.busy() && !other.busy(); });
    const bool first = service.snapshot().status == luciole::CreateStatus::Succeeded;
    const bool second = other.snapshot().status == luciole::CreateStatus::Succeeded;
    TEST_CHECK(first != second && fs::is_directory(fixture.root / "racing"));
  }
  void staleDestinations(Pump& pump, luciole::CreateKind kind) {
    Fixture fixture;
    fs::create_directory(fixture.root / "parent");
    luciole::CreateService service;
    pump.prepare(service, fixture.root / "parent", kind);
    fs::rename(fixture.root / "parent", fixture.root / "original");
    fs::create_directory(fixture.root / "parent");
    pump.create(service, "blocked");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Error
        && !fs::exists(fixture.root / "parent/blocked")
        && !fs::exists(fixture.root / "original/blocked")
    );
    fs::create_directory_symlink(fixture.root / "parent", fixture.root / "alias");
    pump.prepare(service, fixture.root / "alias", kind);
    fs::remove(fixture.root / "alias");
    fs::create_directory_symlink(fixture.root / "original", fixture.root / "alias");
    pump.create(service, "blocked");
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(fixture.root / "original"));
  }
  void validationCancellationAndPermissions(Pump& pump) {
    Fixture fixture;
    luciole::CreateService service;
    pump.prepare(service, fixture.root);
    for (const auto& name : std::vector<std::string>{
             "", ".", "..", "nested/name", std::string("a\0b", 3), std::string(1, static_cast<char>(0xff))
         }) {
      TEST_CHECK(!service.create(name) && fs::is_empty(fixture.root));
    }
    pump.create(service, std::string(10000, 'a'));
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && fs::is_empty(fixture.root));
    service.cancel();
    TEST_CHECK(!service.canCreate() && fs::is_empty(fixture.root));
    TEST_CHECK(service.prepare(location(fixture.root), luciole::CreateKind::Folder));
    TEST_CHECK(!service.prepare(location(fixture.root), luciole::CreateKind::Folder) && !service.create("busy"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Cancelled && fs::is_empty(fixture.root));
    pump.prepare(service, fixture.root);
    TEST_CHECK(service.create("maybe-created"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == luciole::CreateStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == luciole::CreateStatus::Cancelled)
        && fs::exists(fixture.root / "maybe-created") == committed
    );
    for (const auto& parent :
         {location(fixture.root / "missing"), luciole::Location::fromUri("sftp://example.invalid/folder")}) {
      TEST_CHECK(service.prepare(parent, luciole::CreateKind::Folder));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && !service.canCreate());
    }
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root);
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.create(service, "denied");
      fs::permissions(fixture.root, fs::perms::owner_all);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && !fs::exists(fixture.root / "denied"));
    }
    struct MaskGuard {
      mode_t previous = ::umask(0027);
      ~MaskGuard() { ::umask(previous); }
    } guard;
    pump.prepare(service, fixture.root);
    pump.create(service, "permissions");
    struct stat info{};
    TEST_CHECK(::stat((fixture.root / "permissions").c_str(), &info) == 0 && (info.st_mode & 0777) == 0750);
  }
  void createsEmptyFiles(Pump& pump) {
    Fixture fixture;
    luciole::CreateService service;
    pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
    pump.create(service, "Document 文.txt");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded
        && service.snapshot().kind == luciole::CreateKind::EmptyFile
        && service.snapshot().destination == location(fixture.root / "Document 文.txt")
        && fs::is_regular_file(fixture.root / "Document 文.txt")
        && fs::file_size(fixture.root / "Document 文.txt") == 0
    );
    std::ofstream(fixture.root / "protected.txt") << "do not truncate";
    fs::create_directory(fixture.root / "directory");
    std::ofstream(fixture.root / "directory/marker") << "keep";
    fs::create_symlink("missing-target", fixture.root / "broken-link");
    fs::create_symlink("protected.txt", fixture.root / "link");
    TEST_CHECK(::mkfifo((fixture.root / "pipe").c_str(), 0600) == 0);
    pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
    for (const auto* name : {"protected.txt", "directory", "broken-link", "link", "pipe"}) {
      pump.create(service, name);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && service.canCreate());
    }
    std::ifstream content(fixture.root / "protected.txt");
    const std::string bytes{std::istreambuf_iterator<char>(content), {}};
    TEST_CHECK(
        bytes == "do not truncate"
        && fs::exists(fixture.root / "directory/marker")
        && fs::is_symlink(fixture.root / "broken-link")
        && fs::is_symlink(fixture.root / "link")
        && !fs::exists(fixture.root / "missing-target")
        && fs::is_fifo(fixture.root / "pipe")
    );
    TEST_CHECK(!service.create("../escaped"));
    pump.create(service, "retry.txt");
    TEST_CHECK(
        service.snapshot().status == luciole::CreateStatus::Succeeded && fs::file_size(fixture.root / "retry.txt") == 0
    );
    if (atomicNoReplace(fixture.root)) {
      luciole::CreateService other;
      pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
      pump.prepare(other, fixture.root, luciole::CreateKind::EmptyFile);
      TEST_CHECK(service.create("racing.txt") && other.create("racing.txt"));
      pump.until([&] { return !service.busy() && !other.busy(); });
      TEST_CHECK(
          (service.snapshot().status == luciole::CreateStatus::Succeeded)
              != (other.snapshot().status == luciole::CreateStatus::Succeeded)
          && fs::file_size(fixture.root / "racing.txt") == 0
      );
    }
    pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
    TEST_CHECK(service.create("cancelled.txt"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == luciole::CreateStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == luciole::CreateStatus::Cancelled)
        && fs::exists(fixture.root / "cancelled.txt") == committed
    );
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    fs::create_directory(fixture.root / raw);
    pump.prepare(service, fixture.root / raw, luciole::CreateKind::EmptyFile);
    pump.create(service, "child.txt");
    TEST_CHECK(fs::is_regular_file(fixture.root / raw / "child.txt"));
    struct MaskGuard {
      mode_t previous = ::umask(0027);
      ~MaskGuard() { ::umask(previous); }
    } guard;
    pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
    pump.create(service, "mode.txt");
    struct stat info{};
    TEST_CHECK(::stat((fixture.root / "mode.txt").c_str(), &info) == 0 && (info.st_mode & 0777) == 0640);
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.create(service, "denied.txt");
      fs::permissions(fixture.root, fs::perms::owner_all);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Error && !fs::exists(fixture.root / "denied.txt"));
    }
  }
  void creationHistory(Pump& pump) {
    using K = luciole::CreateKind;
    using S = luciole::CreateStatus;
    for (const auto kind : {K::Folder, K::EmptyFile}) {
      Fixture fixture;
      luciole::CreateService service;
      TEST_CHECK(!service.replay({}));
      pump.prepare(service, fixture.root, kind);
      pump.create(service, "created 文");
      TEST_CHECK(service.snapshot().status == S::Succeeded && service.snapshot().receipt);
      for (int cycle = 0; cycle < 3; ++cycle) {
        pump.replay(service);
        TEST_CHECK(service.snapshot().status == S::Succeeded && service.snapshot().removed);
        TEST_CHECK(service.snapshot().receipt && fs::is_empty(fixture.root));
        pump.replay(service);
        TEST_CHECK(service.snapshot().status == S::Succeeded && !service.snapshot().removed);
        TEST_CHECK(service.snapshot().receipt);
        TEST_CHECK(
            kind == K::Folder ? fs::is_directory(fixture.root / "created 文")
                              : fs::is_regular_file(fixture.root / "created 文")
        );
        TEST_CHECK(fs::is_empty(fixture.root / "created 文"));
        TEST_CHECK(std::distance(fs::directory_iterator(fixture.root), fs::directory_iterator{}) == 1);
      }
      pump.replay(service);
      const auto undone = service.snapshot().receipt;
      fs::create_symlink("missing", fixture.root / "created 文");
      TEST_CHECK(service.replay(undone));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(
          service.snapshot().status == S::Error && service.snapshot().error->kind == luciole::FileErrorKind::Exists
      );
      TEST_CHECK(fs::read_symlink(fixture.root / "created 文") == "missing");
      TEST_CHECK(fs::remove(fixture.root / "created 文"));
      TEST_CHECK(service.replay(undone));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == S::Succeeded && service.snapshot().receipt);
    }
  }

  void creationHistoryGuards(Pump& pump) {
    using S = luciole::CreateStatus;
    using E = luciole::FileErrorKind;
    for (int scenario = 0; scenario < 5; ++scenario) {
      Fixture fixture;
      const auto parent = fixture.root / "parent";
      fs::create_directory(parent);
      const auto created = parent / "created";
      luciole::CreateService service;
      pump.prepare(service, parent, scenario == 0 ? luciole::CreateKind::Folder : luciole::CreateKind::EmptyFile);
      pump.create(service, "created");
      const auto receipt = service.snapshot().receipt;
      if (scenario == 0) {
        std::ofstream(created / "keep") << "new data";
      } else if (scenario == 1) {
        std::ofstream(created) << "edited data";
      } else if (scenario == 2) {
        fs::rename(created, parent / "held");
        std::ofstream(created) << "foreign data";
      } else if (scenario == 3) {
        fs::rename(parent, fixture.root / "held-parent");
        fs::create_directory(parent);
        std::ofstream(created) << "foreign parent";
      } else {
        fs::remove(created);
      }
      TEST_CHECK(receipt && service.replay(receipt));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == S::Error && service.snapshot().error->kind == E::Changed);
      TEST_CHECK(!service.snapshot().receipt);
      if (scenario == 0) {
        TEST_CHECK(contents(created / "keep") == "new data");
      } else if (scenario == 1) {
        TEST_CHECK(contents(created) == "edited data");
      } else if (scenario == 2) {
        TEST_CHECK(contents(created) == "foreign data" && fs::is_empty(parent / "held"));
      } else if (scenario == 3) {
        TEST_CHECK(contents(created) == "foreign parent" && fs::is_empty(fixture.root / "held-parent/created"));
      } else {
        TEST_CHECK(!fs::exists(created));
      }
      TEST_CHECK(std::ranges::none_of(fs::directory_iterator(parent), [](const auto& item) {
        return item.path().filename().native().starts_with(".luciole-create-");
      }));
    }
  }

  void retainedDocumentHistory(Pump& pump) {
    using S = luciole::CreateStatus;
    Fixture fixture;
    fs::create_directory(fixture.root / "output");
    const auto source = fixture.root / "template";
    const std::string data("original\0binary\xff", 16);
    std::ofstream(source, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
    luciole::CreateService service;
    prepareDocument(pump, service, fixture.root / "output", source);
    pump.create(service, "created.txt");
    TEST_CHECK(service.snapshot().receipt && contents(fixture.root / "output/created.txt") == data);
    fs::remove(source);
    pump.replay(service);
    TEST_CHECK(service.snapshot().status == S::Succeeded && service.snapshot().removed);
    TEST_CHECK(fs::is_empty(fixture.root / "output"));
    pump.replay(service);
    TEST_CHECK(service.snapshot().status == S::Succeeded && contents(fixture.root / "output/created.txt") == data);
    struct stat before{};
    TEST_CHECK(::stat((fixture.root / "output/created.txt").c_str(), &before) == 0);
    std::ofstream(fixture.root / "output/created.txt") << std::string(data.size(), 'x');
    const struct timespec times[] = {before.st_atim, before.st_mtim};
    TEST_CHECK(::utimensat(AT_FDCWD, (fixture.root / "output/created.txt").c_str(), times, 0) == 0);
    pump.replay(service);
    TEST_CHECK(
        service.snapshot().status == S::Error && service.snapshot().error->kind == luciole::FileErrorKind::Changed
    );
    TEST_CHECK(contents(fixture.root / "output/created.txt") == std::string(data.size(), 'x'));
  }

  void imageHistory(Pump& pump) {
    using S = luciole::CreateStatus;
    Fixture fixture;
    const std::string data("\x89PNG\0binary\xff", 12);
    std::ofstream(fixture.root / "taken.png") << "existing";
    luciole::CreateService service;
    TEST_CHECK(service.prepareImage(location(fixture.root), data));
    pump.until([&] { return !service.busy(); });
    pump.create(service, "taken.png");
    TEST_CHECK(
        service.snapshot().status == S::Error && service.snapshot().error->kind == luciole::FileErrorKind::Exists
    );
    TEST_CHECK(contents(fixture.root / "taken.png") == "existing");
    pump.create(service, "pasted.png");
    TEST_CHECK(service.snapshot().status == S::Succeeded && contents(fixture.root / "pasted.png") == data);
    pump.replay(service);
    TEST_CHECK(service.snapshot().status == S::Succeeded && !fs::exists(fixture.root / "pasted.png"));
    pump.replay(service);
    TEST_CHECK(service.snapshot().status == S::Succeeded && contents(fixture.root / "pasted.png") == data);
  }

  void linkHistory(Pump& pump) {
    using K = luciole::CreateKind;
    using S = luciole::CreateStatus;
    for (const auto kind : {K::SymbolicLink, K::HardLink}) {
      Fixture fixture;
      const auto source = fixture.root / "source";
      std::ofstream(source) << "keep source";
      luciole::CreateService service;
      prepareLink(pump, service, source);
      createLink(pump, service, fixture.root, "created", kind);
      for (int cycle = 0; cycle < 3; ++cycle) {
        TEST_CHECK(service.snapshot().receipt && contents(source) == "keep source");
        pump.replay(service);
        TEST_CHECK(
            service.snapshot().status == S::Succeeded
            && !fs::is_symlink(fixture.root / "created")
            && !fs::exists(fixture.root / "created")
            && contents(source) == "keep source"
        );
        pump.replay(service);
        TEST_CHECK(service.snapshot().status == S::Succeeded);
        TEST_CHECK(
            kind == K::HardLink ? fs::equivalent(source, fixture.root / "created")
                                : fs::read_symlink(fixture.root / "created") == source
        );
      }
      pump.replay(service);
      if (kind == K::SymbolicLink) {
        fs::remove(source);
        pump.replay(service);
        TEST_CHECK(service.snapshot().status == S::Succeeded && fs::read_symlink(fixture.root / "created") == source);
      } else {
        std::ofstream(source) << "changed through alias";
        pump.replay(service);
        TEST_CHECK(service.snapshot().status == S::Error && !fs::exists(fixture.root / "created"));
        TEST_CHECK(contents(source) == "changed through alias");
      }
    }
  }

  void cancelledReplay(Pump& pump) {
    Fixture fixture;
    luciole::CreateService service;
    pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
    pump.create(service, "item");
    TEST_CHECK(service.replay(service.snapshot().receipt));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const auto& snapshot = service.snapshot();
    TEST_CHECK(
        snapshot.status == luciole::CreateStatus::Succeeded || snapshot.status == luciole::CreateStatus::Cancelled
    );
    TEST_CHECK(fs::exists(fixture.root / "item") != snapshot.removed);
    TEST_CHECK(
        std::distance(fs::directory_iterator(fixture.root), fs::directory_iterator{}) == (snapshot.removed ? 0 : 1)
    );
  }

  void restrictiveHistoryMask(Pump& pump) {
    for (const auto kind : {luciole::CreateKind::Folder, luciole::CreateKind::EmptyFile}) {
      Fixture fixture;
      struct MaskGuard {
        mode_t previous = ::umask(0777);
        ~MaskGuard() { ::umask(previous); }
      } guard;
      luciole::CreateService service;
      pump.prepare(service, fixture.root, kind);
      pump.create(service, "mode-zero");
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded && service.snapshot().receipt);
      const bool directoryWritable = kind != luciole::CreateKind::Folder
          || ::faccessat(AT_FDCWD, (fixture.root / "mode-zero").c_str(), W_OK, AT_EACCESS) == 0;
      pump.replay(service);
      if (!directoryWritable) {
        TEST_CHECK(
            service.snapshot().status == luciole::CreateStatus::Error
            && service.snapshot().error->kind == luciole::FileErrorKind::Denied
        );
        struct stat info{};
        TEST_CHECK(::lstat((fixture.root / "mode-zero").c_str(), &info) == 0 && (info.st_mode & 0777) == 0);
        fs::permissions(fixture.root / "mode-zero", fs::perms::owner_all);
        TEST_CHECK(fs::remove(fixture.root / "mode-zero"));
        continue;
      }
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded && fs::is_empty(fixture.root));
      pump.replay(service);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded && service.snapshot().receipt);
      struct stat info{};
      TEST_CHECK(::lstat((fixture.root / "mode-zero").c_str(), &info) == 0 && (info.st_mode & 0777) == 0);
      pump.replay(service);
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded && fs::is_empty(fixture.root));
    }
  }

  void undoSourceRace(Pump& pump) {
    unsigned injections = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      Fixture fixture;
      luciole::CreateService service;
      pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
      pump.create(service, "created");
      const int watch = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
      TEST_CHECK(watch >= 0 && ::inotify_add_watch(watch, fixture.root.c_str(), IN_CREATE) >= 0);
      bool renamed = false, inserted = false;
      std::jthread rival([&](const std::stop_token& stop) {
        std::array<char, 4096> events{};
        bool started = false;
        while (!stop.stop_requested()) {
          started |= ::read(watch, events.data(), events.size()) > 0;
          if (started && ::rename((fixture.root / "created").c_str(), (fixture.root / "held").c_str()) == 0) {
            renamed = true;
            const int foreign =
                ::open((fixture.root / "created").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (foreign >= 0) {
              inserted = true;
              TEST_CHECK(::write(foreign, "foreign", 7) == 7);
              TEST_CHECK(::close(foreign) == 0);
            }
            return;
          }
          std::this_thread::yield();
        }
      });
      pump.replay(service);
      rival.request_stop();
      rival.join();
      TEST_CHECK(::close(watch) == 0);
      if (renamed) {
        TEST_CHECK(fs::is_empty(fixture.root / "held"));
      }
      if (inserted) {
        ++injections;
        TEST_CHECK(contents(fixture.root / "created") == "foreign");
        TEST_CHECK(!service.snapshot().receipt);
      }
    }
    std::println("Creation undo source race: {} replacements injected", injections);
  }

  void monitoredCreation(Pump& pump) {
    Fixture fixture;
    luciole::DirectoryModel model;
    luciole::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(fixture.root));
    pump.until([&] { return model.snapshot().status == luciole::DirectoryStatus::Ready; });
    luciole::CreateService service;
    pump.prepare(service, fixture.root);
    pump.create(service, "created");
    pump.until([&] { return model.rows().size() == 1; });
    const auto id = model.rows().front();
    TEST_CHECK(
        model.entry(id)->location() == service.snapshot().destination
        && model.entry(id)->type == luciole::FileType::Directory
        && model.select(id)
    );
    pump.prepare(service, fixture.root, luciole::CreateKind::EmptyFile);
    pump.create(service, "created.txt");
    pump.until([&] { return model.rows().size() == 2; });
    const auto file = std::ranges::find_if(model.rows(), [&](auto id) {
      return model.entry(id)->location() == service.snapshot().destination;
    });
    TEST_CHECK(
        file != model.rows().end()
        && model.entry(*file)->type == luciole::FileType::Regular
        && model.entry(*file)->size == 0
        && model.select(*file)
    );
  }
} // namespace
int main() {
  try {
    Fixture home;
    fs::create_directory(home.root / "config");
    std::ofstream(home.root / "config/user-dirs.dirs") << "XDG_TEMPLATES_DIR=\"$HOME/Templates\"\n";
    TEST_CHECK(g_setenv("HOME", home.root.c_str(), TRUE));
    TEST_CHECK(g_setenv("XDG_CONFIG_HOME", (home.root / "config").c_str(), TRUE));
    Pump pump;
    templateCatalog(pump, home.root);
    createsDocuments(pump);
    staleTemplatesAndCancellation(pump);
    createsLinks(pump);
    linkCollisionsAndErrors(pump);
    staleLinksAndCancellation(pump);
    linksAcrossFilesystems(pump);
    createsFolders(pump);
    createsEmptyFiles(pump);
    collisionsAndRetry(pump);
    staleDestinations(pump, luciole::CreateKind::Folder);
    staleDestinations(pump, luciole::CreateKind::EmptyFile);
    validationCancellationAndPermissions(pump);
    monitoredCreation(pump);
    creationHistory(pump);
    creationHistoryGuards(pump);
    retainedDocumentHistory(pump);
    imageHistory(pump);
    linkHistory(pump);
    cancelledReplay(pump);
    undoSourceRace(pump);
    restrictiveHistoryMask(pump);
    std::println("create-service tests passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "create-service: {}", error.what());
    return 1;
  }
}
