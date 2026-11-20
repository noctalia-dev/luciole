#include "event_loop/glib_poll_source.h"
#include "fs/directory_session.h"
#include "fs/file_query.h"
#include "fs/gio_interop.h"
#include "fs/native_listing.h"
#include "fs/recursive_search.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <linux/magic.h>
#include <memory>
#include <mutex>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>

namespace {
  using namespace std::chrono_literals;
  using luciole::DirectorySession;
  using luciole::DirectoryStatus;
  using luciole::Location;
  namespace fs = std::filesystem;

  struct Fixture {
    fs::path path;
    Fixture() {
      GError* error = nullptr;
      const luciole::GString directory(g_dir_make_tmp("luciole-test-XXXXXX", &error));
      if (!directory) {
        const std::string message(error->message);
        g_error_free(error);
        throw std::runtime_error(message);
      }
      path = directory.get();
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(const std::string& name, const std::string& contents = "data") const {
      std::ofstream file(path / name);
      file << contents;
      TEST_CHECK(file.good());
    }
    Location location() const { return Location::fromPath(path.native()); }
  };

  class Pump {
  public:
    // Reports the caller on timeout, so a CI-only stall names the wait that never finished.
    void until(const std::function<bool()>& done, std::source_location caller = std::source_location::current()) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        luciole::test::check(std::chrono::steady_clock::now() < deadline, "pump.until deadline", caller);
        once();
      }
    }

    void once() {
      std::vector<pollfd> fds;
      const auto start = m_source.addPollFds(fds);
      const int requested = m_source.pollTimeoutMs();
      const int timeout = requested < 0 ? 20 : std::min(requested, 20);
      TEST_CHECK(::poll(fds.data(), fds.size(), timeout) >= 0);
      m_source.dispatch(fds, start);
    }

  private:
    GlibPollSource m_source;
  };

  const luciole::DirectoryEntry& entry(const DirectorySession& session, const std::string& name) {
    const auto& entries = session.snapshot().entries;
    const auto it = std::ranges::find(entries, name, &luciole::DirectoryEntry::name);
    TEST_CHECK(it != entries.end());
    return *it;
  }

  bool hasEntry(const DirectorySession& session, const std::string& name) {
    return std::ranges::find(session.snapshot().entries, name, &luciole::DirectoryEntry::name)
        != session.snapshot().entries.end();
  }

  // Whether the published entry at name is a link known to lead to a folder.
  bool linksToDirectory(const DirectorySession& session, std::string_view name) {
    const auto& entries = session.snapshot().entries;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries.brief(i).name == name) {
        return entries.brief(i).linksToDirectory;
      }
    }
    return false;
  }

  // Ready, with sniffed content types published.
  void
  ready(Pump& pump, const DirectorySession& session, std::source_location caller = std::source_location::current()) {
    pump.until(
        [&session] {
          return session.snapshot().status != DirectoryStatus::Loading && !session.snapshot().contentTypesPending;
        },
        caller
    );
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Ready);
  }

  void testLocations() {
    const std::string bytes = "/tmp/a space#%\n" + std::string(1, static_cast<char>(0xFF));
    const auto location = Location::fromPath(bytes);
    TEST_CHECK(location.nativePath() == bytes);
    TEST_CHECK(location.uri().contains("%23%25%0A%FF"));
    TEST_CHECK(Location::fromUri(location.uri()) == location);
    const auto remote = Location::fromUri("sftp://example.test/folder%20name");
    TEST_CHECK(!remote.nativePath());
    for (const auto& invalid :
         std::vector<std::string>{"relative/path", "file:///bad%xy", std::string("file:///bad\0tail", 17)}) {
      bool rejected = false;
      try {
        (void)Location::fromUri(invalid);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      TEST_CHECK(rejected);
    }
  }

  // Local listings derive icons and child locations themselves; both must equal what GIO reports.
  void testLocalIconsAndLocations(Pump& pump, const fs::path& profile) {
    const auto home = profile / "home";
    TEST_CHECK(home.native() == g_get_home_dir());
    for (const auto* folder : {"Desktop", "Docs", "ordinary", "Pictures"}) {
      fs::create_directories(home / folder);
    }
    const auto write = [&](const std::string& name, const std::string& contents) {
      std::ofstream file(home / name, std::ios::binary);
      file << contents;
      TEST_CHECK(file.good());
    };
    write("notes.txt", "text");
    write("more.txt", "more text");
    write("empty", "");
    write("picture", std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, '\0'));
    write("launcher.desktop", "[Desktop Entry]\nType=Application\nName=Test\nExec=true\n");
    write("a space#%\n" + std::string(1, static_cast<char>(0xFF)), "raw");
    write("ünï côdé.txt", "unicode");
    fs::create_symlink("notes.txt", home / "link");
    fs::create_symlink("missing", home / "broken");
    fs::create_directory_symlink(home / "Docs", profile / "docs-link");

    DirectorySession session;
    std::size_t special = 0;
    for (const auto& folder : {profile, home}) {
      session.open(Location::fromPath(folder.native()));
      ready(pump, session);
      for (const auto& entry : session.snapshot().entries) {
        const auto location = entry.location();
        const auto file = luciole::gioFile(location);
        const luciole::GioPtr<GFileInfo> info(g_file_query_info(
            file.get(),
            G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON
                                           "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
        ));
        TEST_CHECK(info);
        const auto expected = luciole::readFileIcon(info.get(), location);
        TEST_CHECK(entry.icon && *entry.icon == expected);
        special += expected.names.front().starts_with("user-") || expected.names.front().starts_with("folder-");
        TEST_CHECK(Location::fromUri(location.uri()) == location);
        TEST_CHECK(Location::fromPath((folder / entry.name).native()) == location);
      }
    }
    // Home, Desktop, Docs and Pictures; equal artwork is stored once.
    TEST_CHECK(special == 4);
    TEST_CHECK(entry(session, "notes.txt").icon == entry(session, "more.txt").icon);
    TEST_CHECK(entry(session, "notes.txt").icon != entry(session, "ordinary").icon);
    TEST_CHECK(entry(session, "launcher.desktop").icon->desktopFile == (home / "launcher.desktop").native());
  }

  void testContentTypes(Pump& pump) {
    Fixture fixture;
    fixture.write(".bash_history", "cd Documents\nprintf hello\n");
    fixture.write("extensionless", "ordinary text without an extension\n");
    fixture.write("binary", std::string("\0\1\2\3\0\xff", 6));
    fixture.write(
        "launcher.desktop", "[Desktop Entry]\nType=Application\nName=Mail App\nExec=false\nMimeType=text/html;\n"
    );
    fixture.write("record.yaml", "message: hello\n");
    gsize pngSize = 0;
    const luciole::GString png(
        reinterpret_cast<char*>(g_base64_decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a4V0AAAAASUVORK5CYII=", &pngSize
        ))
    );
    fixture.write("image-without-extension", std::string(png.get(), pngSize));
    fs::create_symlink(".bash_history", fixture.path / "history-link");
    TEST_CHECK(::mkfifo((fixture.path / "pipe").c_str(), 0600) == 0);
    DirectorySession session;
    session.open(fixture.location());
    ready(pump, session);
    TEST_CHECK(entry(session, ".bash_history").contentType == "text/plain");
    TEST_CHECK(entry(session, "extensionless").contentType == "text/plain");
    TEST_CHECK(entry(session, "binary").contentType == "application/octet-stream");
    TEST_CHECK(entry(session, "launcher.desktop").contentType == "application/x-desktop");
    TEST_CHECK(entry(session, "record.yaml").contentType == "application/yaml");
    const luciole::GString launcherDescription(g_content_type_get_description("application/x-desktop"));
    const auto launcherDetails =
        luciole::readFileDetails(Location::fromPath((fixture.path / "launcher.desktop").native()), {});
    TEST_CHECK(launcherDetails.typeDescription == launcherDescription.get());
    TEST_CHECK(launcherDetails.displayName == "launcher.desktop");
    TEST_CHECK(
        luciole::readFileDetails(Location::fromPath((fixture.path / "binary").native()), {}).typeDescription == "File"
    );
    TEST_CHECK(
        luciole::readFileDetails(Location::fromPath((fixture.path / "history-link").native()), {}).typeDescription
        == "Link"
    );
    TEST_CHECK(entry(session, "image-without-extension").contentType == "image/png");
    TEST_CHECK(entry(session, "history-link").symlink && entry(session, "history-link").contentType == "inode/symlink");
    TEST_CHECK(entry(session, "pipe").type == luciole::FileType::Special);
    const auto details = luciole::readFileDetails(Location::fromPath((fixture.path / ".bash_history").native()), {});
    const luciole::GString textDescription(g_content_type_get_description("text/plain"));
    TEST_CHECK(details.typeDescription == textDescription.get());
    TEST_CHECK(
        luciole::readFileDetails(Location::fromPath((fixture.path / "history-link").native()), {}).type
        == luciole::FileType::Symlink
    );
    TEST_CHECK(
        luciole::readFileDetails(Location::fromPath((fixture.path / "pipe").native()), {}).type
        == luciole::FileType::Special
    );
    luciole::RecursiveSearch search;
    search.start(fixture.location(), "bash_history", true);
    pump.until([&] { return search.snapshot().status != DirectoryStatus::Loading; });
    TEST_CHECK(
        search.snapshot().status == DirectoryStatus::Ready
        && search.snapshot().entries.size() == 1
        && search.snapshot().entries.front().contentType == "text/plain"
    );
    fixture.write("extensionless", std::string("\0\1\2\3\0\xff", 6));
    pump.until([&] {
      return session.snapshot().status == DirectoryStatus::Ready
          && entry(session, "extensionless").contentType == "application/octet-stream";
    });

    const luciole::GioPtr<GFileInfo> info(g_file_info_new());
    TEST_CHECK(!luciole::readContentType(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, "text/plain");
    TEST_CHECK(luciole::readContentType(info.get()) == "text/plain");
    g_file_info_set_content_type(info.get(), "application/octet-stream");
    TEST_CHECK(luciole::readContentType(info.get()) == "application/octet-stream");
    g_file_info_set_content_type(info.get(), "");
    TEST_CHECK(luciole::readContentType(info.get()) == "text/plain");
    g_file_info_set_content_type(info.get(), "\xff");
    TEST_CHECK(luciole::readContentType(info.get()) == "text/plain");
    g_file_info_set_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, 42);
    TEST_CHECK(luciole::readContentType(info.get()) == "text/plain");
  }

  void testEnumerationAndMonitoring(Pump& pump) {
    Fixture fixture;
    const std::string unusual = "raw#%\n" + std::string(1, static_cast<char>(0xFF));
    fixture.write(unusual);
    TEST_CHECK(::mkfifo((fixture.path / ".hidden").c_str(), 0600) == 0);
    fs::create_directory(fixture.path / "folder");
    fs::create_symlink("missing-target", fixture.path / "broken-link");
    // Cross several asynchronous batches without turning hard links into one entry.
    for (int i = 0; i < 150; ++i) {
      fixture.write(std::to_string(i));
    }
    fs::create_hard_link(fixture.path / "0", fixture.path / "hard-link");
    DirectorySession session;
    session.open(fixture.location());
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Loading);
    ready(pump, session);
    TEST_CHECK(session.snapshot().entries.size() == 155);
    TEST_CHECK(!session.snapshot().monitorError);
    TEST_CHECK(entry(session, unusual).location().nativePath() == (fixture.path / unusual).native());
    TEST_CHECK(g_utf8_validate(entry(session, unusual).displayName().c_str(), -1, nullptr));
    TEST_CHECK(entry(session, ".hidden").hidden);
    TEST_CHECK(entry(session, "broken-link").symlink);
    TEST_CHECK(entry(session, "folder").type == luciole::FileType::Directory);
    TEST_CHECK(entry(session, "0").id != entry(session, "hard-link").id);
    const auto id = entry(session, unusual).id;
    TEST_CHECK(entry(session, "folder").contentType == "inode/directory");
    session.refresh();
    ready(pump, session);
    TEST_CHECK(entry(session, unusual).id == id);
    fs::rename(fixture.path / unusual, fixture.path / "renamed");
    pump.until([&session] {
      return session.snapshot().status == DirectoryStatus::Ready && hasEntry(session, "renamed");
    });
    TEST_CHECK(entry(session, "renamed").id == id);
    fixture.write("renamed", "longer contents");
    pump.until([&session] { return entry(session, "renamed").size == 15; });
    TEST_CHECK(entry(session, "renamed").id == id);
    fs::remove(fixture.path / "renamed");
    pump.until([&session] { return !hasEntry(session, "renamed"); });
    fixture.write("renamed");
    pump.until([&session] { return hasEntry(session, "renamed"); });
    TEST_CHECK(entry(session, "renamed").id != id);
  }

  void testSearchSnapshotOwnership(Pump& pump) {
    Fixture fixture;
    for (int i = 0; i < 150; ++i) {
      fixture.write("match-" + std::to_string(i));
    }
    const std::string raw = "match-raw-%\n\xff";
    fixture.write(raw);
    luciole::RecursiveSearch search;
    std::vector<luciole::DirectorySnapshot> published;
    search.setChangedCallback([&] {
      if (!search.snapshot().entries.empty()) {
        published.push_back(search.snapshot());
      }
    });
    search.start(fixture.location(), "match", true);
    pump.until([&] { return search.snapshot().status != DirectoryStatus::Loading; });
    TEST_CHECK(search.snapshot().status == DirectoryStatus::Ready && search.snapshot().entries.size() == 151);
    TEST_CHECK(published.size() > 1 && published.front().entries.size() < 151);
    const auto retained = published.front();
    const auto* first = &retained.entries.front();
    const auto found = std::ranges::find(search.snapshot().entries, first->id, &luciole::DirectoryEntry::id);
    TEST_CHECK(found != search.snapshot().entries.end() && &*found == first);
    const auto unusual = std::ranges::find(search.snapshot().entries, raw, &luciole::DirectoryEntry::name);
    TEST_CHECK(
        unusual != search.snapshot().entries.end() && unusual->location().nativePath() == (fixture.path / raw).native()
    );
    const auto complete = search.snapshot();
    search.setChangedCallback({});
    search.start(fixture.location(), "no matches", true);
    search.cancel();
    TEST_CHECK(retained.entries.size() < 151 && &retained.entries.front() == first);
    TEST_CHECK(complete.entries.size() == 151 && complete.entries.front().id == first->id);
    TEST_CHECK(first->location().nativePath().has_value());
  }

  void testGenerationAndLifetime(Pump& pump) {
    Fixture first;
    Fixture second;
    first.write("first");
    second.write("second");
    DirectorySession session;
    session.open(first.location());
    const auto generation = session.snapshot().generation;
    session.open(second.location());
    ready(pump, session);
    TEST_CHECK(session.snapshot().generation > generation);
    TEST_CHECK(!hasEntry(session, "first") && hasEntry(session, "second"));
    const auto retained = session.snapshot();
    session.open(first.location());
    session.cancel();
    bool drained = false;
    g_idle_add(
        +[](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &drained
    );
    pump.until([&drained] { return drained; });
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Cancelled);
    TEST_CHECK(retained.status == DirectoryStatus::Ready && retained.entries.front().name == "second");
    session.refresh();
    ready(pump, session);
    first.write("after-resume");
    pump.until([&session] { return hasEntry(session, "after-resume"); });
    const auto beforeCancel = entry(session, "first").id;
    session.cancel();
    fs::remove(first.path / "first");
    first.write("first");
    session.refresh();
    ready(pump, session);
    TEST_CHECK(entry(session, "first").id != beforeCancel);

    auto dying = std::make_unique<DirectorySession>();
    dying->setChangedCallback([&dying] { dying.reset(); });
    dying->open(first.location());
    // A worker wakeup must reach the GLib fd through our poll adapter.
    bool woke = false;
    std::thread worker([&woke] {
      g_main_context_invoke(
          nullptr,
          +[](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &woke
      );
    });
    worker.join();
    pump.until([&woke] { return woke; });
  }

  void testChangesDuringLoading(Pump& pump) {
    Fixture fixture;
    fixture.write("original");
    fixture.write("unchanged");
    DirectorySession session;
    session.open(fixture.location());
    ready(pump, session);
    const auto unchangedId = entry(session, "unchanged").id;
    bool changed = false;
    session.setChangedCallback([&] {
      if (!changed && session.snapshot().status == DirectoryStatus::Loading) {
        changed = true;
        fs::rename(fixture.path / "original", fixture.path / "moved");
        fixture.write("during-load");
      }
    });
    session.refresh();
    pump.until([&session] {
      return session.snapshot().status == DirectoryStatus::Ready
          && hasEntry(session, "moved")
          && hasEntry(session, "during-load")
          && !hasEntry(session, "original");
    });
    // An explicit rescan can see the new name before the monitor reports its rename.
    // Monitor-driven rename continuity is covered by testEnumerationAndMonitoring.
    TEST_CHECK(entry(session, "unchanged").id == unchangedId);
    const auto movedId = entry(session, "moved").id;

    // An atomic replacement must invalidate the target's old identity, even
    // when the temporary source never appeared in a published listing.
    fixture.write("replacement-tmp", "replacement");
    fs::rename(fixture.path / "replacement-tmp", fixture.path / "moved");
    pump.until([&session, movedId] {
      return entry(session, "moved").id != movedId && entry(session, "moved").size == 11;
    });
    session.setChangedCallback({});
    fs::remove_all(fixture.path);
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(session.snapshot().error->kind == luciole::FileErrorKind::Missing);
    TEST_CHECK(session.snapshot().entries.empty());
  }

  void testErrorsAndEmpty(Pump& pump) {
    Fixture fixture;
    DirectorySession session;
    session.open(fixture.location());
    ready(pump, session);
    TEST_CHECK(session.snapshot().entries.empty());
    session.open(Location::fromPath((fixture.path / "missing").native()));
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(session.snapshot().error->kind == luciole::FileErrorKind::Missing);
    fixture.write("plain-file");
    session.open(Location::fromPath((fixture.path / "plain-file").native()));
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(session.snapshot().error->kind == luciole::FileErrorKind::NotDirectory);
    session.open(Location::fromUri("luciole-missing-provider://user:secret@example.invalid/folder"));
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(session.snapshot().error->kind == luciole::FileErrorKind::Unsupported);
    const auto& message = session.snapshot().error->message;
    TEST_CHECK(message.contains("luciole-missing-provider://") && message.contains("GIO/GVfs"));
    TEST_CHECK(!message.contains("user") && !message.contains("secret") && !message.contains("example.invalid"));
    session.open(fixture.location());
    ready(pump, session);
    TEST_CHECK(!session.snapshot().error && !session.snapshot().entries.empty());
  }

  // A folder that changes faster than it can be scanned still publishes, at a bounded rate.
  void testChurnDoesNotStarve(Pump& pump) {
    Fixture fixture;
    for (int index = 0; index < 20'000; ++index) {
      const std::ofstream file(fixture.path / std::format("file-{:05d}", index));
    }
    DirectorySession session;

    // Creations and deletions are not rate-limited by GIO, unlike content changes.
    const std::jthread writer([&fixture](const std::stop_token& stop) {
      for (int index = 0; !stop.stop_requested(); ++index) {
        const std::ofstream file(fixture.path / std::format("busy-{}", index));
        std::error_code ignored;
        fs::remove(fixture.path / std::format("busy-{}", index - 1), ignored);
        std::this_thread::sleep_for(5ms);
      }
    });
    const auto start = std::chrono::steady_clock::now();
    session.open(fixture.location());
    pump.until([&] { return session.snapshot().status == DirectoryStatus::Ready; });
    const auto firstGeneration = session.snapshot().generation;
    TEST_CHECK(std::chrono::steady_clock::now() - start < 3s);
    // A slow first scan can schedule a two-second debounce before the next scan starts.
    pump.until([&] {
      return session.snapshot().generation > firstGeneration && session.snapshot().status == DirectoryStatus::Ready;
    });
  }

  bool unknown(const luciole::FileAccess& access) {
    const auto values = {access.read, access.write, access.execute, access.rename, access.trash, access.remove};
    return std::ranges::all_of(values, [](const auto value) { return !value.has_value(); });
  }

  void testMetadataAbsenceAndTypes() {
    const luciole::GioPtr<GFileInfo> info(g_file_info_new());
    TEST_CHECK(!luciole::readContentType(info.get()));
    g_file_info_set_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, true);
    TEST_CHECK(!luciole::readContentType(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, "");
    TEST_CHECK(!luciole::readContentType(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, "\xff");
    TEST_CHECK(!luciole::readContentType(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, "text/plain");
    TEST_CHECK(luciole::readContentType(info.get()) == "text/plain");
    TEST_CHECK(
        unknown(luciole::readFileAccess(info.get()))
        && !luciole::readFilesystemInfo(info.get()).readOnly.has_value()
        && !luciole::readFilesystemInfo(info.get()).freeBytes.has_value()
    );
    g_file_info_set_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_READ, true);
    g_file_info_set_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, false);
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, "false");
    g_file_info_set_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, false);
    g_file_info_set_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, 1);
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_FREE, "1024");
    const auto access = luciole::readFileAccess(info.get());
    const auto filesystem = luciole::readFilesystemInfo(info.get());
    TEST_CHECK(access.read == true && access.remove == false && !access.write.has_value());
    TEST_CHECK(
        filesystem.readOnly == false
        && !filesystem.remote.has_value()
        && !filesystem.type.has_value()
        && !filesystem.freeBytes.has_value()
    );
    g_file_info_set_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_FREE, 0);
    TEST_CHECK(luciole::readFilesystemInfo(info.get()).freeBytes == 0);
    g_file_info_set_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_FREE, G_MAXUINT64);
    TEST_CHECK(luciole::readFilesystemInfo(info.get()).freeBytes == G_MAXUINT64);
  }

  void testCapabilityLifecycle(Pump& pump) {
    Fixture fixture;
    fixture.write("plain");
    const auto file = fixture.path / "plain";
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
    DirectorySession session;
    session.open(fixture.location());
    TEST_CHECK(session.snapshot().metadataLoading && unknown(session.snapshot().metadata.access));
    ready(pump, session);
    pump.until([&session] { return !session.snapshot().metadataLoading; });
    TEST_CHECK(!session.snapshot().accessError && !session.snapshot().filesystemError);
    TEST_CHECK(
        session.snapshot().metadata.access.read == true
        && session.snapshot().metadata.access.write == true
        && session.snapshot().metadata.access.execute == true
    );
    TEST_CHECK(session.snapshot().metadata.filesystem.readOnly == false);
    TEST_CHECK(session.snapshot().metadata.filesystem.freeBytes.has_value());
    const auto access = entry(session, "plain").access;
    TEST_CHECK(access.read == true && access.write == true && access.execute == false);
    const auto id = entry(session, "plain").id;
    const auto published = session.snapshot();
    const auto* retainedEntry = &published.entries.front();
    TEST_CHECK(retainedEntry == &session.snapshot().entries.front());
    const auto generation = session.snapshot().generation;
    bool invalidated = false;
    session.setChangedCallback([&] {
      if (session.snapshot().generation == generation
          && session.snapshot().metadataLoading
          && !session.snapshot().metadata.filesystem.freeBytes.has_value()
          && unknown(session.snapshot().metadata.access)
          && unknown(entry(session, "plain").access)) {
        invalidated = true;
      }
    });
    fs::permissions(file, fs::perms::owner_all);
    pump.until([&] {
      return session.snapshot().generation > generation
          && session.snapshot().status == DirectoryStatus::Ready
          && !session.snapshot().metadataLoading;
    });
    TEST_CHECK(invalidated && entry(session, "plain").id == id);
    TEST_CHECK(published.status == DirectoryStatus::Ready && retainedEntry->access.rename == true);
    TEST_CHECK(&published.entries.front() == retainedEntry && retainedEntry->access.remove == true);
    TEST_CHECK(entry(session, "plain").access.execute == (::access(file.c_str(), X_OK) == 0));
    session.setChangedCallback({});
    fs::permissions(file, fs::perms::owner_read);
    session.refresh();
    TEST_CHECK(
        session.snapshot().metadataLoading
        && !session.snapshot().metadata.filesystem.freeBytes.has_value()
        && unknown(session.snapshot().metadata.access)
        && unknown(entry(session, "plain").access)
    );
    ready(pump, session);
    pump.until([&session] { return !session.snapshot().metadataLoading; });
    TEST_CHECK(entry(session, "plain").access.write == (::access(file.c_str(), W_OK) == 0));
    session.cancel();
    TEST_CHECK(
        !session.snapshot().metadataLoading
        && !session.snapshot().metadata.filesystem.freeBytes.has_value()
        && unknown(session.snapshot().metadata.access)
        && unknown(entry(session, "plain").access)
    );

    session.open(Location::fromPath(file.native()));
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(
        !session.snapshot().metadataLoading
        && unknown(session.snapshot().metadata.access)
        && !session.snapshot().metadata.filesystem.readOnly.has_value()
    );
    // Retire a file query immediately: its non-executable access must never
    // replace the next directory generation's traversal capability.
    session.open(Location::fromPath(file.native()));
    session.open(fixture.location());
    ready(pump, session);
    pump.until([&session] { return !session.snapshot().metadataLoading; });
    TEST_CHECK(session.snapshot().metadata.access.execute == true);
  }

  // Every fact of a native listing equals what GIO's local backend reports for the same file.
  void compareWithGio(const luciole::DirectoryEntry& entry) {
    const auto location = entry.location();
    const auto file = luciole::gioFile(location);
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        file.get(), "standard::*,time::*,unix::*,owner::*,access::*,etag::value,id::*",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info);
    TEST_CHECK(entry.name == g_file_info_get_name(info.get()));
    TEST_CHECK(entry.displayName() == g_file_info_get_display_name(info.get()));
    TEST_CHECK(entry.type == luciole::fileType(g_file_info_get_file_type(info.get())));
    TEST_CHECK(entry.symlink == luciole::readIsSymlink(info.get()));
    TEST_CHECK(entry.hidden == (g_file_info_get_is_hidden(info.get()) != FALSE || entry.name.starts_with('.')));
    TEST_CHECK(entry.size == luciole::infoUint64(info.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE));
    TEST_CHECK(entry.modifiedSeconds == luciole::infoUint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED));
    TEST_CHECK(entry.etag() == std::optional<std::string>(g_file_info_get_etag(info.get())));
    const auto changed = luciole::infoUint64(info.get(), G_FILE_ATTRIBUTE_TIME_CHANGED);
    const auto nanos = luciole::infoUint32(info.get(), "time::changed-nsec");
    TEST_CHECK(changed && nanos && entry.contentRevision == luciole::ChangeRevision(*changed, *nanos));
    TEST_CHECK(entry.details == luciole::readEntryMetadata(info.get()));
    TEST_CHECK(entry.identity() == luciole::readFileIdentity(info.get()));
    TEST_CHECK(entry.contentType == luciole::readContentType(info.get()));
    TEST_CHECK(entry.icon && *entry.icon == luciole::readFileIcon(info.get(), location));
    const auto expected = luciole::readFileAccess(info.get());
    TEST_CHECK(entry.access.rename == expected.rename);
    TEST_CHECK(entry.access.remove == expected.remove);
    TEST_CHECK(entry.access.trash == expected.trash);
    if (entry.symlink) {
      TEST_CHECK(!entry.access.read && !entry.access.write && !entry.access.execute);
    } else {
      TEST_CHECK(entry.access.read == expected.read && entry.access.write == expected.write);
      TEST_CHECK(entry.access.execute == expected.execute);
    }
  }

  void testNativeMatchesGio(Pump& pump) {
    Fixture fixture;
    const auto write = [&](const std::string& name, const std::string& contents, fs::perms mode) {
      std::ofstream(fixture.path / name, std::ios::binary) << contents;
      fs::permissions(fixture.path / name, mode);
    };
    write("regular.txt", "text", fs::perms::owner_read | fs::perms::owner_write);
    write("empty", "", fs::perms::owner_read | fs::perms::owner_write);
    write("readonly", "fixed", fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    write("script.sh", "#!/bin/sh\ntrue\n", fs::perms::owner_all);
    write("picture", std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, '\0'), fs::perms::owner_read);
    write("raw-\xff", "raw", fs::perms::owner_read | fs::perms::owner_write);
    write("ünï côdé.txt", "unicode", fs::perms::owner_read | fs::perms::owner_write);
    write(".hidden", "regular.txt\npicture\n", fs::perms::owner_read | fs::perms::owner_write);
    fs::create_symlink("regular.txt", fixture.path / "link");
    fs::create_symlink("missing", fixture.path / "broken");
    fs::create_directory_symlink("locked", fixture.path / "folder-link");
    TEST_CHECK(::mkfifo((fixture.path / "fifo").c_str(), 0600) == 0);
    fs::create_directory(fixture.path / "locked");
    std::ofstream(fixture.path / "locked" / "inner") << "inner";
    fs::permissions(fixture.path / "locked", fs::perms::owner_read | fs::perms::owner_exec);

    DirectorySession session;
    for (const auto& folder : {fixture.path, fixture.path / "locked"}) {
      session.open(Location::fromPath(folder.native()));
      ready(pump, session);
      TEST_CHECK(!session.snapshot().entries.empty());
      for (const auto& entry : session.snapshot().entries) {
        compareWithGio(entry);
      }
      if (folder == fixture.path) {
        pump.until([&] { return linksToDirectory(session, "folder-link"); });
        TEST_CHECK(!entry(session, "link").linksToDirectory && !entry(session, "broken").linksToDirectory);
        TEST_CHECK(!entry(session, "locked").linksToDirectory);
      }
    }
    // Root can retain write access after chmod, including in the CI container.
    TEST_CHECK(entry(session, "inner").access.rename == (::access((fixture.path / "locked").c_str(), W_OK) == 0));
    fs::permissions(fixture.path / "locked", fs::perms::owner_all);
  }

  // Uncertain names publish at once with their name-based type; GIO's sniffed type follows, and rescans of an
  // unchanged file keep it without reading the file again.
  void testDeferredSniffing(Pump& pump) {
    Fixture fixture;
    const auto picture = fixture.path / "picture";
    std::ofstream(picture, std::ios::binary) << std::string("\x89PNG\r\n\x1a\n", 8) << std::string(64, '\0');
    DirectorySession session;
    bool publishedPending = false;
    session.setChangedCallback([&] {
      publishedPending = publishedPending
          || (session.snapshot().status == DirectoryStatus::Ready && session.snapshot().contentTypesPending);
    });
    session.open(Location::fromPath(fixture.path.native()));
    ready(pump, session);
    TEST_CHECK(publishedPending);
    TEST_CHECK(entry(session, "picture").contentType == "image/png");

    publishedPending = false;
    session.refresh();
    ready(pump, session);
    TEST_CHECK(!publishedPending && entry(session, "picture").contentType == "image/png");

    std::ofstream(picture, std::ios::binary | std::ios::trunc) << "plain words\n";
    session.refresh();
    ready(pump, session);
    TEST_CHECK(publishedPending && entry(session, "picture").contentType == "text/plain");
    session.setChangedCallback({});
  }

  void testBrowseCatalogue(Pump& pump) {
    Fixture fixture;
    for (int i = 0; i < 1400; ++i) {
      fixture.write(std::format("file{}.txt", i), "");
    }
    const std::string raw = "raw-" + std::string(1, static_cast<char>(0xFF));
    fixture.write(raw, "raw bytes");
    fixture.write("Straße.txt", "Unicode");
    fixture.write("picture", std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, '\0'));
    fixture.write(".hidden", "file10.txt\n");
    fs::create_directory(fixture.path / "folder");
    fs::create_symlink("missing", fixture.path / "broken");
    fs::create_directory_symlink("folder", fixture.path / "folder-link");
    DirectorySession session;
    session.setCatalogueEnabled(true);
    session.open(fixture.location());
    ready(pump, session);
    pump.until([&] { return linksToDirectory(session, "folder-link"); });
    const auto initial = session.snapshot().entries;
    TEST_CHECK(initial.isCatalogue() && initial.size() == 1407);
    const auto find = [&](const std::string& name) {
      for (std::size_t i = 0; i < initial.size(); ++i) {
        if (initial.brief(i).name == name) {
          return i;
        }
      }
      throw std::runtime_error("missing catalogue item");
    };
    const auto rawIndex = find(raw);
    const auto hiddenIndex = find("file10.txt");
    const auto pictureIndex = find("picture");
    const auto brokenIndex = find("broken");
    const auto folderLinkIndex = find("folder-link");
    TEST_CHECK(initial.brief(rawIndex).displayName != raw);
    TEST_CHECK(initial.brief(hiddenIndex).hidden && initial.brief(brokenIndex).symlink);
    TEST_CHECK(initial.brief(folderLinkIndex).linksToDirectory && !initial.brief(brokenIndex).linksToDirectory);
    TEST_CHECK(!initial.brief(find("folder")).linksToDirectory);
    TEST_CHECK(initial.location(rawIndex).nativePath() == (fixture.path / raw).native());
    for (std::size_t i = 0; i < initial.size(); ++i) {
      TEST_CHECK(!initial.known(i));
    }
    const auto& pending = initial[pictureIndex];
    TEST_CHECK(pending.metadataPending && !pending.size && !pending.identity());
    // Requests made while a monitor rescan runs are dropped; like the view, ask again on every pass.
    pump.until([&] {
      for (const auto index : {rawIndex, hiddenIndex, pictureIndex, brokenIndex, folderLinkIndex}) {
        session.requestMetadata(index, initial.brief(index).id, luciole::EntryDetails::All);
      }
      const auto& entries = session.snapshot().entries;
      return entries.known(pictureIndex) != nullptr && entries.known(folderLinkIndex) != nullptr;
    });
    const auto rich = session.snapshot().entries;
    // GIO details never report a link's target, so they keep the catalogue's.
    TEST_CHECK(rich.known(folderLinkIndex)->linksToDirectory);
    TEST_CHECK(rich.sameCatalogue(initial) && pending.metadataPending && !initial.known(pictureIndex));
    const auto& picture = *rich.known(pictureIndex);
    TEST_CHECK(!picture.metadataPending && !picture.metadataFailed && picture.contentType == "image/png");
    TEST_CHECK(picture.identity() && picture.contentRevision && picture.size == 72);
    TEST_CHECK(
        rich.known(hiddenIndex)->hidden && rich.known(rawIndex)->displayName() == initial.brief(rawIndex).displayName
    );
    const auto icon = picture.icon;
    TEST_CHECK(icon != nullptr);
    // Real GIO is authoritative for visible facts, including content sniffing and true identities.
    const auto file = luciole::gioFile(picture.location());
    const luciole::GioPtr<GFileInfo> info(
        g_file_query_info(file.get(), "*", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr)
    );
    TEST_CHECK(info && picture.identity() == luciole::readFileIdentity(info.get()));
    const auto access = luciole::readFileAccess(info.get());
    TEST_CHECK(
        picture.access.read == access.read
        && picture.access.write == access.write
        && picture.access.execute == access.execute
        && picture.access.rename == access.rename
        && picture.access.trash == access.trash
        && picture.access.remove == access.remove
    );
    TEST_CHECK(picture.details == luciole::readEntryMetadata(info.get()));
    // Walking beyond the working-set limit evicts facts, not catalogue names or retained snapshots.
    for (std::size_t begin = 0; begin < initial.size(); begin += 128) {
      const auto end = std::min(begin + 128, initial.size());
      pump.until([&] {
        bool known = true;
        for (auto i = begin; i < end; ++i) {
          if (!session.snapshot().entries.known(i)) {
            session.requestMetadata(i, initial.brief(i).id);
            known = false;
          }
        }
        return known;
      });
    }
    std::size_t retained = 0;
    for (std::size_t i = 0; i < initial.size(); ++i) {
      retained += session.snapshot().entries.known(i) != nullptr;
    }
    TEST_CHECK(retained == 1024 && rich.known(pictureIndex)->icon == icon);
    const auto projected = luciole::DirectoryEntries::project({rich}, {{0, pictureIndex}, {0, rawIndex}});
    TEST_CHECK(projected.front().identity() == picture.identity() && projected.back().name == raw);
    const auto columnIndex = initial.size() - 1;
    const auto beforeColumns = session.snapshot().entries;
    TEST_CHECK(beforeColumns.known(columnIndex) && beforeColumns.known(columnIndex)->queriedDetails == 0);
    TEST_CHECK(!beforeColumns.known(columnIndex)->details.owner && !beforeColumns.known(columnIndex)->details.mode);
    // A rescan can retire queued facts. Start one explicitly and keep requesting
    // the visible column until the new listing can serve it, just as the view does.
    session.refresh();
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Loading);
    pump.until([&] {
      session.requestMetadata(columnIndex, initial.brief(columnIndex).id, luciole::EntryDetails::Permissions);
      const auto* value = session.snapshot().entries.known(columnIndex);
      return value && (value->queriedDetails & luciole::EntryDetails::Permissions) != 0;
    });
    const auto* permissions = session.snapshot().entries.known(columnIndex);
    TEST_CHECK(permissions->details.mode && !permissions->details.owner && !permissions->details.group);
    TEST_CHECK(!beforeColumns.known(columnIndex)->details.mode);
    session.refresh();
    pump.until([&] {
      session.requestMetadata(columnIndex, initial.brief(columnIndex).id, luciole::EntryDetails::All);
      const auto* value = session.snapshot().entries.known(columnIndex);
      return value && value->queriedDetails == luciole::EntryDetails::All;
    });
    TEST_CHECK(session.snapshot().entries.known(columnIndex)->details.owner);
    session.refresh();
    ready(pump, session);
    TEST_CHECK(session.snapshot().entries.brief(pictureIndex).id == initial.brief(pictureIndex).id);
    TEST_CHECK(!session.snapshot().entries.known(pictureIndex) && rich.known(pictureIndex)->size == 72);
    // A retired ID cannot populate a new snapshot, and cancellation suppresses queued work.
    session.requestMetadata(pictureIndex, UINT64_MAX);
    session.requestMetadata(pictureIndex, session.snapshot().entries.brief(pictureIndex).id);
    session.cancel();
    pump.once();
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Cancelled && pending.metadataPending);
  }

  void testCatalogueContinuity() {
    Fixture fixture;
    for (int i = 0; i < 600; ++i) {
      fixture.write(std::format("file{}", i));
    }
    auto listing = luciole::listNativeFolder(fixture.path.native(), 0, nullptr, true);
    TEST_CHECK(listing.catalogue && !listing.unsupported);
    const auto& catalogue = *listing.catalogue;
    std::size_t index = 0;
    while (catalogue.brief(index).name != "file0") {
      ++index;
    }
    const std::array rows{index};
    auto check = luciole::validateCatalogueRows(catalogue, rows, nullptr);
    TEST_CHECK(!check.parentChanged && !check.unavailable && check.changed.empty());
    fs::rename(fixture.path / "file0", fixture.path / "held-old-file");
    fixture.write("file0", "replacement");
    check = luciole::validateCatalogueRows(catalogue, rows, nullptr);
    TEST_CHECK(!check.parentChanged && !check.unavailable && check.changed == std::vector{index});
    Fixture parent;
    fs::rename(fixture.path, parent.path / "held-old-folder");
    fs::create_directory(fixture.path);
    check = luciole::validateCatalogueRows(catalogue, rows, nullptr);
    TEST_CHECK(check.parentChanged && !check.unavailable);
  }

  // Children that cannot be inspected leave the listing to GIO, which reports what it can.
  void testNativeUnsupportedFallsBack(Pump& pump) {
    if (::geteuid() == 0) {
      return;
    }
    Fixture fixture;
    fs::create_directory(fixture.path / "closed");
    std::ofstream(fixture.path / "closed" / "inside") << "data";
    fs::permissions(fixture.path / "closed", fs::perms::owner_read | fs::perms::owner_write);
    const auto folder = fixture.path / "closed";
    TEST_CHECK(luciole::listNativeFolder(folder.native(), 0, nullptr).unsupported);
    DirectorySession session;
    session.open(Location::fromPath(folder.native()));
    ready(pump, session);
    TEST_CHECK(session.snapshot().entries.size() == 1);
    TEST_CHECK(session.snapshot().entries.front().name == "inside");
    fs::permissions(fixture.path / "closed", fs::perms::owner_all);
  }

  // Following a link can hang on an unresponsive mount without end. The listing publishes and cancels without
  // waiting for it, links stay with the files until their targets are known, and hung checks hold only the shared
  // link workers.
  void testSlowLinkTarget(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.path / "folder");
    fs::create_directory_symlink("folder", fixture.path / "folder-link");
    fixture.write("file");
    struct Gate {
      std::mutex mutex;
      std::condition_variable released;
      bool open = false;
      std::atomic<int> waiting = 0;
    };
    const auto gate = std::make_shared<Gate>();
    const auto hanging = [gate](const std::string& path) {
      ++gate->waiting;
      std::unique_lock lock(gate->mutex);
      gate->released.wait(lock, [&] { return gate->open; });
      return luciole::linksToDirectory(path);
    };
    DirectorySession session(0, {}, hanging);
    session.open(fixture.location());
    ready(pump, session);
    pump.until([&] { return gate->waiting == 1; });
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Ready && !linksToDirectory(session, "folder-link"));
    session.cancel();
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Cancelled);
    // The cancelled check still hangs; the next listing completes without it, and its own check takes the other
    // link worker.
    session.open(fixture.location());
    ready(pump, session);
    pump.until([&] { return gate->waiting == 2; });
    TEST_CHECK(!linksToDirectory(session, "folder-link"));
    // With every link worker hung, other folders still list, and their links wait for a free worker.
    Fixture other;
    fs::create_directory(other.path / "folder");
    fs::create_directory_symlink("folder", other.path / "folder-link");
    DirectorySession waiting;
    waiting.open(other.location());
    ready(pump, waiting);
    TEST_CHECK(!linksToDirectory(waiting, "folder-link"));
    {
      const std::scoped_lock lock(gate->mutex);
      gate->open = true;
    }
    gate->released.notify_all();
    pump.until([&] { return linksToDirectory(session, "folder-link") && linksToDirectory(waiting, "folder-link"); });
    TEST_CHECK(gate->waiting == 2);
    // A rescan keeps the known target kind while it checks the link again, so the link does not jump to the files.
    session.refresh();
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Loading);
    ready(pump, session);
    TEST_CHECK(linksToDirectory(session, "folder-link"));
    fs::remove(fixture.path / "folder-link");
    fs::create_symlink("file", fixture.path / "folder-link");
    session.refresh();
    ready(pump, session);
    pump.until([&] { return !linksToDirectory(session, "folder-link"); });
  }

  void testNetworkFilesystem() {
    const Fixture fixture;
    TEST_CHECK(!luciole::networkFilesystem(fixture.path.native()));
    TEST_CHECK(luciole::networkFilesystem((fixture.path / "missing").native()));
    constexpr std::uint32_t kFuse = 0x65735546;
    const dev_t disk = makedev(8, 1);
    const dev_t anonymous = makedev(0, 63);
    struct statfs filesystem{};
    filesystem.f_type = kFuse;
    TEST_CHECK(luciole::networkFilesystem(filesystem, anonymous));
    TEST_CHECK(!luciole::networkFilesystem(filesystem, disk));
    filesystem.f_type = NFS_SUPER_MAGIC;
    TEST_CHECK(luciole::networkFilesystem(filesystem, anonymous));
    filesystem.f_type = EXT4_SUPER_MAGIC;
    TEST_CHECK(!luciole::networkFilesystem(filesystem, disk));
  }
} // namespace

int main() {
  try {
    // A private home with configured user folders, selected before GLib first reads either.
    const Fixture profile;
    fs::create_directories(profile.path / "home");
    fs::create_directories(profile.path / "config");
    {
      std::ofstream dirs(profile.path / "config/user-dirs.dirs");
      dirs
          << "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_DOCUMENTS_DIR=\"$HOME/Docs\"\nXDG_MUSIC_DIR=\"$HOME/\"\n"
          << "XDG_PICTURES_DIR=\""
          << (profile.path / "home/Pictures").native()
          << "\"\n";
    }
    g_setenv("HOME", (profile.path / "home").c_str(), TRUE);
    g_setenv("XDG_CONFIG_HOME", (profile.path / "config").c_str(), TRUE);
    testLocations();
    Pump pump;
    testLocalIconsAndLocations(pump, profile.path);
    testContentTypes(pump);
    testEnumerationAndMonitoring(pump);
    testSearchSnapshotOwnership(pump);
    testGenerationAndLifetime(pump);
    testChangesDuringLoading(pump);
    testErrorsAndEmpty(pump);
    testChurnDoesNotStarve(pump);
    testMetadataAbsenceAndTypes();
    testCapabilityLifecycle(pump);
    testNativeMatchesGio(pump);
    testNativeUnsupportedFallsBack(pump);
    testNetworkFilesystem();
    testSlowLinkTarget(pump);
    testDeferredSniffing(pump);
    testBrowseCatalogue(pump);
    testCatalogueContinuity();
    std::println("directory session checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "directory session test: {}", error.what());
    return 1;
  }
}
