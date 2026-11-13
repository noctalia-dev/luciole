#include "event_loop/glib_poll_source.h"
#include "fs/directory_session.h"
#include "fs/file_query.h"
#include "fs/gio_interop.h"
#include "fs/recursive_search.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
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
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
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

  void ready(Pump& pump, const DirectorySession& session) {
    pump.until([&session] { return session.snapshot().status != DirectoryStatus::Loading; });
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
    fixture.write(".hidden");
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
    TEST_CHECK(entry(session, unusual).location.nativePath() == (fixture.path / unusual).native());
    TEST_CHECK(g_utf8_validate(entry(session, unusual).displayName.c_str(), -1, nullptr));
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
    DirectorySession session;
    session.open(fixture.location());
    ready(pump, session);
    const auto originalId = entry(session, "original").id;
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
    TEST_CHECK(entry(session, "moved").id == originalId);

    // An atomic replacement must invalidate the target's old identity, even
    // when the temporary source never appeared in a published listing.
    fixture.write("replacement-tmp", "replacement");
    fs::rename(fixture.path / "replacement-tmp", fixture.path / "moved");
    pump.until([&session, originalId] {
      return entry(session, "moved").id != originalId && entry(session, "moved").size == 11;
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
    TEST_CHECK(
        entry(session, "plain").access.read == true
        && entry(session, "plain").access.write == true
        && entry(session, "plain").access.execute == false
    );
    const auto id = entry(session, "plain").id;
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
} // namespace

int main() {
  try {
    testLocations();
    Pump pump;
    testContentTypes(pump);
    testEnumerationAndMonitoring(pump);
    testGenerationAndLifetime(pump);
    testChangesDuringLoading(pump);
    testErrorsAndEmpty(pump);
    testMetadataAbsenceAndTypes();
    testCapabilityLifecycle(pump);
    std::println("directory session checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "directory session test: {}", error.what());
    return 1;
  }
}
