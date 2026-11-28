#include "browser/directory_model.h"
#include "event_loop/glib_poll_source.h"
#include "fs/filename.h"
#include "operations/rename_service.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/luciole-rename-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()) != nullptr);
      root = path;
    }
    ~Fixture() {
      std::error_code error;
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
    void prepare(luciole::RenameService& service, const fs::path& path) {
      TEST_CHECK(service.prepare(location(path)));
      until([&] { return !service.busy(); });
      TEST_CHECK(service.canRename());
    }
    void rename(luciole::RenameService& service, const std::string& name) {
      TEST_CHECK(service.rename(name));
      until([&] { return !service.busy(); });
    }

  private:
    GlibPollSource m_source;
  };
  void successfulRenames(Pump& pump) {
    Fixture fixture;
    luciole::RenameService service;
    write(fixture.root / "source.txt", "content");
    pump.prepare(service, fixture.root / "source.txt");
    pump.rename(service, "résumé 文.txt");
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Succeeded
        && read(fixture.root / "résumé 文.txt") == "content"
        && !fs::exists(fixture.root / "source.txt")
    );
    TEST_CHECK(service.snapshot().destination == location(fixture.root / "résumé 文.txt"));
    pump.prepare(service, fixture.root / "résumé 文.txt");
    pump.rename(service, "résumé 文.txt");
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Succeeded && !service.snapshot().receipt);
    pump.prepare(service, fixture.root / "résumé 文.txt");
    pump.rename(service, "RÉSUMÉ 文.txt");
    TEST_CHECK(read(fixture.root / "RÉSUMÉ 文.txt") == "content");
    fs::create_directory(fixture.root / "folder");
    write(fixture.root / "folder/child", "nested");
    pump.prepare(service, fixture.root / "folder");
    pump.rename(service, "new folder");
    TEST_CHECK(read(fixture.root / "new folder/child") == "nested");
    fs::create_symlink("missing", fixture.root / "broken");
    pump.prepare(service, fixture.root / "broken");
    pump.rename(service, "new link");
    TEST_CHECK(fs::read_symlink(fixture.root / "new link") == "missing");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.root / raw, "raw data");
    pump.prepare(service, fixture.root / raw);
    pump.rename(service, "readable");
    TEST_CHECK(read(fixture.root / "readable") == "raw data");
  }
  void collisionsAndReplacement(Pump& pump) {
    Fixture fixture;
    luciole::RenameService service;
    write(fixture.root / "source", "source");
    write(fixture.root / "existing", "existing");
    pump.prepare(service, fixture.root / "source");
    pump.rename(service, "existing");
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error
        && service.canRename()
        && read(fixture.root / "existing") == "existing"
        && read(fixture.root / "source") == "source"
    );
    fs::create_directory(fixture.root / "directory");
    pump.rename(service, "directory");
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error && fs::is_directory(fixture.root / "directory")
    );
    fs::create_symlink("absent", fixture.root / "symlink");
    pump.rename(service, "symlink");
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Error && fs::is_symlink(fixture.root / "symlink"));
    fs::create_hard_link(fixture.root / "source", fixture.root / "hard-link");
    pump.rename(service, "hard-link");
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error
        && fs::equivalent(fixture.root / "source", fixture.root / "hard-link")
    );
    pump.rename(service, "available");
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Succeeded);
    write(fixture.root / "source", "original");
    pump.prepare(service, fixture.root / "source");
    fs::rename(fixture.root / "source", fixture.root / "original");
    write(fixture.root / "source", "replacement");
    pump.rename(service, "renamed");
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error
        && read(fixture.root / "source") == "replacement"
        && read(fixture.root / "original") == "original"
        && !fs::exists(fixture.root / "renamed")
    );
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "original");
    pump.prepare(service, fixture.root / "parent/item");
    fs::rename(fixture.root / "parent", fixture.root / "old-parent");
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "replacement");
    pump.rename(service, "renamed");
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error
        && read(fixture.root / "old-parent/item") == "original"
        && read(fixture.root / "parent/item") == "replacement"
    );
  }
  void validationAndCancellation(Pump& pump) {
    Fixture fixture;
    luciole::RenameService service;
    write(fixture.root / "source", "data");
    pump.prepare(service, fixture.root / "source");
    for (const auto& name : std::vector<std::string>{
             "", ".", "..", "child/name", std::string("a\0b", 3), std::string(1, static_cast<char>(0xff))
         }) {
      TEST_CHECK(!service.rename(name) && fs::exists(fixture.root / "source"));
    }
    pump.rename(service, std::string(10000, 'a'));
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Error && fs::exists(fixture.root / "source"));
    service.cancel();
    TEST_CHECK(!service.canRename());
    write(fixture.root / "editable", "before");
    pump.prepare(service, fixture.root / "editable");
    write(fixture.root / "editable", "updated while editing");
    pump.rename(service, "edited");
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Succeeded
        && read(fixture.root / "edited") == "updated while editing"
    );
    pump.prepare(service, fixture.root / "source");
    TEST_CHECK(service.rename("cancel-result"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == luciole::RenameStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == luciole::RenameStatus::Cancelled)
        && fs::exists(fixture.root / "cancel-result") == committed
        && fs::exists(fixture.root / "source") != committed
    );
    if (committed) {
      fs::rename(fixture.root / "cancel-result", fixture.root / "source");
    }
    TEST_CHECK(service.prepare(location(fixture.root / "source")));
    TEST_CHECK(!service.prepare(location(fixture.root / "source")) && !service.rename("other"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Cancelled && fs::exists(fixture.root / "source"));
    TEST_CHECK(service.prepare(luciole::Location::fromUri("sftp://example.invalid/file")));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Error && !service.canRename());
    TEST_CHECK(service.prepare(location(fixture.root / "missing")));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Error);
    TEST_CHECK(
        luciole::filenameStemLength("archive.tar.gz", false) == 11
        && luciole::filenameStemLength(".bashrc", false) == 7
        && luciole::filenameStemLength("folder.name", true) == 11
        && luciole::filenameStemLength("trailing.", false) == 9
    );
    // Repeated F2 toggles between selecting the stem and the full filename (including extension).
    TEST_CHECK(luciole::nextRenameSelectionEnd("AGENTS.md", false, 0, 6) == 9);
    TEST_CHECK(luciole::nextRenameSelectionEnd("AGENTS.md", false, 0, 9) == 6);
    TEST_CHECK(luciole::nextRenameSelectionEnd("AGENTS.md", false, 2, 2) == 6);
    TEST_CHECK(luciole::nextRenameSelectionEnd("AGENTS.md", false, 1, 6) == 6);
    TEST_CHECK(luciole::nextRenameSelectionEnd("archive.tar.gz", false, 0, 11) == 14);
    TEST_CHECK(luciole::nextRenameSelectionEnd("archive.tar.gz", false, 0, 14) == 11);
    TEST_CHECK(luciole::nextRenameSelectionEnd(".config.json", false, 0, 7) == 12);
    TEST_CHECK(luciole::nextRenameSelectionEnd(".config.json", false, 0, 12) == 7);
    TEST_CHECK(luciole::nextRenameSelectionEnd("folder.name", true, 0, 11) == 11);
    TEST_CHECK(luciole::nextRenameSelectionEnd(".bashrc", false, 0, 7) == 7);
    TEST_CHECK(luciole::nextRenameSelectionEnd("Makefile", false, 0, 8) == 8);
    TEST_CHECK(luciole::nextRenameSelectionEnd("trailing.", false, 0, 9) == 9);
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root / "source");
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.rename(service, "denied");
      fs::permissions(fixture.root, fs::perms::owner_all);
      TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Error && fs::exists(fixture.root / "source"));
    }
  }
  void selectionSurvives(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "before.txt", "content");
    luciole::DirectoryModel model;
    luciole::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(fixture.root));
    pump.until([&] { return model.snapshot().status == luciole::DirectoryStatus::Ready && model.rows().size() == 1; });
    const auto id = model.rows().front();
    TEST_CHECK(model.select(id));
    luciole::RenameService service;
    pump.prepare(service, fixture.root / "before.txt");
    pump.rename(service, "after.txt");
    pump.until([&] {
      const auto* entry = model.entry(id);
      return entry != nullptr && entry->name == "after.txt";
    });
    TEST_CHECK(model.selection().selected.contains(id) && model.selection().focused == id);
  }
  void undoRedo(Pump& pump) {
    Fixture fixture;
    luciole::RenameService service;
    const auto raw = std::string("raw-;") + static_cast<char>(0xff);
    write(fixture.root / raw, "content");
    pump.prepare(service, fixture.root / raw);
    pump.rename(service, "résumé 文.txt");
    TEST_CHECK(service.snapshot().receipt != nullptr);
    auto receipt = service.snapshot().receipt;
    TEST_CHECK(service.replay(receipt));
    TEST_CHECK(!service.replay(receipt));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Succeeded);
    TEST_CHECK(read(fixture.root / raw) == "content" && !fs::exists(fixture.root / "résumé 文.txt"));
    for (int i = 0; i < 6; ++i) {
      receipt = service.snapshot().receipt;
      TEST_CHECK(receipt && service.replay(receipt));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Succeeded && service.snapshot().receipt);
    }
    auto stale = receipt;
    TEST_CHECK(service.replay(stale));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Error);
    pump.prepare(service, fixture.root / raw);
    pump.rename(service, "readable");
    receipt = service.snapshot().receipt;
    write(fixture.root / raw, "do not overwrite");
    TEST_CHECK(service.replay(receipt));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error
        && service.snapshot().error->kind == luciole::FileErrorKind::Exists
    );
    TEST_CHECK(read(fixture.root / raw) == "do not overwrite" && read(fixture.root / "readable") == "content");
    fs::remove(fixture.root / raw);
    TEST_CHECK(service.replay(receipt));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::RenameStatus::Succeeded && read(fixture.root / raw) == "content");
    pump.prepare(service, fixture.root / raw);
    pump.rename(service, "readable");
    receipt = service.snapshot().receipt;
    const auto time = fs::last_write_time(fixture.root / "readable");
    write(fixture.root / "readable", "changed");
    fs::last_write_time(fixture.root / "readable", time);
    TEST_CHECK(service.replay(receipt));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error
        && service.snapshot().error->kind == luciole::FileErrorKind::Changed
    );
    TEST_CHECK(read(fixture.root / "readable") == "changed" && !fs::exists(fixture.root / raw));
  }
  void undoTargetChanges(Pump& pump) {
    Fixture fixture;
    luciole::RenameService service;
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/a", "original");
    pump.prepare(service, fixture.root / "parent/a");
    pump.rename(service, "b");
    auto receipt = service.snapshot().receipt;
    fs::rename(fixture.root / "parent/b", fixture.root / "parent/held");
    write(fixture.root / "parent/b", "replacement");
    TEST_CHECK(service.replay(receipt));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error && read(fixture.root / "parent/b") == "replacement"
    );
    pump.prepare(service, fixture.root / "parent/b");
    pump.rename(service, "c");
    receipt = service.snapshot().receipt;
    fs::rename(fixture.root / "parent", fixture.root / "old-parent");
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/c", "other parent");
    TEST_CHECK(service.replay(receipt));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Error && read(fixture.root / "parent/c") == "other parent"
    );
    fs::create_directory(fixture.root / "folder");
    write(fixture.root / "folder/child", "nested");
    pump.prepare(service, fixture.root / "folder");
    pump.rename(service, "renamed-folder");
    TEST_CHECK(service.replay(service.snapshot().receipt));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(
        service.snapshot().status == luciole::RenameStatus::Succeeded && read(fixture.root / "folder/child") == "nested"
    );
    fs::create_symlink("missing", fixture.root / "link");
    pump.prepare(service, fixture.root / "link");
    pump.rename(service, "renamed-link");
    TEST_CHECK(service.replay(service.snapshot().receipt));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == luciole::RenameStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == luciole::RenameStatus::Cancelled)
        && fs::is_symlink(fixture.root / "link") == committed
        && fs::is_symlink(fixture.root / "renamed-link") != committed
    );
    TEST_CHECK(!service.replay({}));
  }
} // namespace
int main() {
  try {
    Pump pump;
    successfulRenames(pump);
    collisionsAndReplacement(pump);
    validationAndCancellation(pump);
    selectionSurvives(pump);
    undoRedo(pump);
    undoTargetChanges(pump);
    std::println("rename-service tests passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "rename-service: {}", error.what());
    return 1;
  }
}
