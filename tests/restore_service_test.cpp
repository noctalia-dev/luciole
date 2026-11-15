#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "operations/restore_service.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  using O = luciole::RestoreOutcome;
  using C = luciole::TransferConflictChoice;
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  struct Fixture {
    fs::path root;
    Fixture() {
      const luciole::GString path(g_dir_make_tmp("luciole-restore-XXXXXX", nullptr));
      TEST_CHECK(path != nullptr);
      root = path.get();
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 5) >= 0);
        m_source.dispatch(fds, first);
      }
    }
    luciole::DirectoryEntries listing(const luciole::Location& location) {
      luciole::DirectorySession session;
      session.open(location);
      until([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
      TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);
      return session.snapshot().entries;
    }
    luciole::DirectoryEntry trashed(const fs::path& original) {
      auto entries = listing(luciole::Location::trash());
      const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
        return entry.trashInfo().original == luciole::Location::fromPath(original.native());
      });
      TEST_CHECK(found != entries.end());
      return *found;
    }
    void done(luciole::RestoreService& service) {
      until([&] { return !service.busy(); });
    }
    void decide(luciole::RestoreService& service, C choice) {
      until([&] { return service.conflict().has_value() || !service.busy(); });
      TEST_CHECK(service.conflict());
      TEST_CHECK(!service.resolve(service.conflict()->id + 1, choice));
      TEST_CHECK(service.resolve(service.conflict()->id, choice));
    }

  private:
    GlibPollSource m_source;
  };
  void trash(const fs::path& path) {
    GError* error = nullptr;
    const bool result =
        g_file_trash(luciole::gioFile(luciole::Location::fromPath(path.native())).get(), nullptr, &error);
    const std::string message = error ? error->message : "trash failed";
    g_clear_error(&error);
    if (!result) {
      throw std::runtime_error(message);
    }
  }
  fs::path backing(const luciole::DirectoryEntry& entry) {
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(entry.location()).get(), G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info);
    const auto* uri = g_file_info_get_attribute_string(info.get(), G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
    TEST_CHECK(uri);
    const auto path = luciole::Location::fromUri(uri).nativePath();
    TEST_CHECK(path);
    return *path;
  }
  void guardsAndUnavailable(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "keep.txt", "keep");
    auto entry = pump.listing(luciole::Location::fromPath(fixture.root.native())).front();
    luciole::RestoreService service;
    TEST_CHECK(!service.start({}) && !service.start({entry}));
    entry.place = luciole::Location::fromUri("trash:///unavailable");
    entry.setTrash(
        luciole::TrashInfo{
            .original = luciole::Location::fromPath((fixture.root / "keep.txt").native()),
            .deletionDate = entry.trashInfo().deletionDate,
        }
    );
    TEST_CHECK(!service.start({entry, entry}));
    TEST_CHECK(service.start({entry}) && !service.start({entry}));
    pump.done(service);
    TEST_CHECK(service.results().front().outcome == O::Failed && read(fixture.root / "keep.txt") == "keep");
    auto dying = std::make_unique<luciole::RestoreService>();
    bool notified = false;
    dying->setChangedCallback([&] { notified = true; });
    TEST_CHECK(dying->start({entry}));
    dying.reset();
    TEST_CHECK(service.start({entry}));
    pump.done(service);
    TEST_CHECK(!notified);
  }
  void ordinaryRestore(Pump& pump) {
    Fixture fixture;
    fs::create_directories(fixture.root / "folder 文/nested");
    write(fixture.root / "folder 文/nested/child", "nested");
    write(fixture.root / "plain #%.txt", "original");
    write(fixture.root / "outside", "untouched");
    fs::create_symlink(fixture.root / "outside", fixture.root / "link");
    fs::create_symlink("missing", fixture.root / "broken");
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.root / raw, "raw name");
    std::vector<luciole::DirectoryEntry> selection;
    for (const auto& name :
         {std::string("folder 文"), std::string("plain #%.txt"), std::string("link"), std::string("broken"), raw}) {
      const auto original = fixture.root / name;
      trash(original);
      selection.push_back(pump.trashed(original));
    }
    luciole::RestoreService service;
    TEST_CHECK(service.start(selection));
    pump.done(service);
    TEST_CHECK(service.results().size() == selection.size() && service.completed() == selection.size());
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == O::Restored;
    }));
    TEST_CHECK(read(fixture.root / "folder 文/nested/child") == "nested");
    TEST_CHECK(read(fixture.root / "plain #%.txt") == "original" && read(fixture.root / raw) == "raw name");
    TEST_CHECK(fs::is_symlink(fixture.root / "link") && fs::is_symlink(fixture.root / "broken"));
    TEST_CHECK(read(fixture.root / "outside") == "untouched");
  }
  void conflicts(Pump& pump) {
    Fixture fixture;
    const auto path = fixture.root / "same.txt";
    write(path, "restore");
    trash(path);
    const auto selected = pump.trashed(path);
    write(path, "existing");
    luciole::RestoreService service;
    TEST_CHECK(service.start({selected}));
    pump.decide(service, C::Skip);
    pump.done(service);
    TEST_CHECK(service.results().front().outcome == O::Skipped && read(path) == "existing");
    write(fixture.root / "same (1).txt", "number collision");
    TEST_CHECK(service.start({selected}));
    pump.decide(service, C::KeepBoth);
    pump.done(service);
    TEST_CHECK(service.results().front().outcome == O::Restored);
    TEST_CHECK(read(path) == "existing" && read(fixture.root / "same (2).txt") == "restore");
    trash(fixture.root / "same (2).txt");
    const auto replace = pump.trashed(fixture.root / "same (2).txt");
    write(fixture.root / "same (2).txt", "replace me");
    TEST_CHECK(service.start({replace}));
    pump.decide(service, C::Replace);
    pump.done(service);
    TEST_CHECK(service.results().front().outcome == O::Restored && read(fixture.root / "same (2).txt") == "restore");
    fs::create_directory(fixture.root / "folder");
    write(fixture.root / "folder/inside", "original folder");
    trash(fixture.root / "folder");
    const auto folder = pump.trashed(fixture.root / "folder");
    fs::create_directory(fixture.root / "folder");
    write(fixture.root / "folder/keep", "keep existing folder");
    TEST_CHECK(service.start({folder}));
    pump.decide(service, C::KeepBoth);
    pump.done(service);
    TEST_CHECK(service.results().front().outcome == O::Restored);
    TEST_CHECK(read(fixture.root / "folder (1)/inside") == "original folder");
    TEST_CHECK(read(fixture.root / "folder/keep") == "keep existing folder");
  }
  void failuresAndCancellation(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "gone");
    write(fixture.root / "gone/file", "missing parent");
    trash(fixture.root / "gone/file");
    const auto missing = pump.trashed(fixture.root / "gone/file");
    fs::remove(fixture.root / "gone");
    write(fixture.root / "stale", "before");
    trash(fixture.root / "stale");
    const auto stale = pump.trashed(fixture.root / "stale");
    write(backing(stale), "changed source content");
    write(fixture.root / "good", "good");
    trash(fixture.root / "good");
    const auto good = pump.trashed(fixture.root / "good");
    luciole::RestoreService service;
    TEST_CHECK(service.start({missing, stale, good}));
    pump.done(service);
    TEST_CHECK(service.results()[0].outcome == O::Failed && service.results()[1].outcome == O::Failed);
    TEST_CHECK(service.results()[2].outcome == O::Restored && read(fixture.root / "good") == "good");
    TEST_CHECK(!fs::exists(fixture.root / "gone") && !fs::exists(fixture.root / "stale"));
    TEST_CHECK(read(backing(missing)) == "missing parent" && read(backing(stale)) == "changed source content");
    const auto fresh = pump.trashed(fixture.root / "stale");
    write(fixture.root / "stale", "keep destination");
    TEST_CHECK(service.start({fresh}));
    pump.until([&] { return service.conflict().has_value(); });
    const auto oldId = service.conflict()->id;
    fs::remove(fixture.root / "stale");
    write(fixture.root / "stale", "destination changed");
    TEST_CHECK(service.resolve(oldId, C::Replace));
    pump.until([&] { return service.conflict() && service.conflict()->id != oldId; });
    pump.decide(service, C::Skip);
    pump.done(service);
    TEST_CHECK(
        service.results().front().outcome == O::Skipped && read(fixture.root / "stale") == "destination changed"
    );
    TEST_CHECK(service.start({fresh, missing}));
    pump.until([&] { return service.conflict().has_value(); });
    service.cancel();
    pump.done(service);
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == O::Cancelled;
    }));
    TEST_CHECK(
        read(fixture.root / "stale") == "destination changed" && read(backing(fresh)) == "changed source content"
    );
    write(fixture.root / "completed-first", "completed before cancellation");
    trash(fixture.root / "completed-first");
    const auto first = pump.trashed(fixture.root / "completed-first");
    TEST_CHECK(service.start({first, fresh, missing}));
    pump.until([&] { return service.conflict().has_value(); });
    service.cancel();
    pump.done(service);
    TEST_CHECK(
        service.results()[0].outcome == O::Restored
        && service.results()[1].outcome == O::Cancelled
        && service.results()[2].outcome == O::Cancelled
    );
    TEST_CHECK(read(fixture.root / "completed-first") == "completed before cancellation");
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "restore here");
    trash(fixture.root / "parent/item");
    const auto redirected = pump.trashed(fixture.root / "parent/item");
    write(fixture.root / "parent/item", "held destination");
    TEST_CHECK(service.start({redirected}));
    pump.until([&] { return service.conflict().has_value(); });
    fs::rename(fixture.root / "parent", fixture.root / "held-parent");
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "replacement parent");
    TEST_CHECK(service.resolve(service.conflict()->id, C::Replace));
    pump.done(service);
    TEST_CHECK(service.results().front().outcome == O::Failed);
    TEST_CHECK(
        read(fixture.root / "parent/item") == "replacement parent"
        && read(fixture.root / "held-parent/item") == "held destination"
        && read(backing(redirected)) == "restore here"
    );
  }
} // namespace

int main(int argc, char** argv) {
  try {
    Pump pump;
    if (argc == 2 && std::string_view(argv[1]) == "--provider") {
      ordinaryRestore(pump);
      conflicts(pump);
      failuresAndCancellation(pump);
    } else {
      TEST_CHECK(argc == 1);
      guardsAndUnavailable(pump);
    }
    std::println("restore-service checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "restore-service: {}", error.what());
    return 1;
  }
}
