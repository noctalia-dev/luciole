#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "fs/trash_availability.h"
#include "operations/empty_trash_service.h"
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
  using Outcome = luciole::EmptyTrashOutcome;
  using Status = luciole::EmptyTrashStatus;
  void write(const fs::path& path, const std::string& value) { std::ofstream(path) << value; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  struct Fixture {
    fs::path root;
    Fixture() {
      const luciole::GString path(g_dir_make_tmp("luciole-empty-trash-XXXXXX", nullptr));
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
        const auto start = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    void prepare(luciole::EmptyTrashService& service) {
      TEST_CHECK(service.prepare());
      until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == Status::Ready);
    }
    void empty(luciole::EmptyTrashService& service) {
      TEST_CHECK(service.empty());
      until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == Status::Finished);
    }

  private:
    GlibPollSource m_source;
  };
  void trash(const fs::path& path) {
    GError* error = nullptr;
    const bool ok = g_file_trash(luciole::gioFile(luciole::Location::fromPath(path.native())).get(), nullptr, &error);
    g_clear_error(&error);
    TEST_CHECK(ok);
  }
  fs::path trashFiles() { return fs::path(g_get_user_data_dir()) / "Trash/files"; }
  fs::path trashInfo() { return fs::path(g_get_user_data_dir()) / "Trash/info"; }
  void unavailableAndLifetime(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "keep", "native content");
    luciole::EmptyTrashService service;
    TEST_CHECK(!service.empty());
    TEST_CHECK(service.prepare());
    TEST_CHECK(!service.prepare() && !service.empty());
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(
        service.snapshot().status == Status::Error
        && !service.canEmpty()
        && read(fixture.root / "keep") == "native content"
    );
    TEST_CHECK(service.prepare());
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == Status::Cancelled && !service.empty());
    auto destroyed = std::make_unique<luciole::EmptyTrashService>();
    bool notified = false;
    destroyed->setChangedCallback([&] { notified = true; });
    TEST_CHECK(destroyed->prepare());
    notified = false;
    destroyed.reset();
    TEST_CHECK(service.prepare());
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!notified);
  }
  void nativeDeletion(Pump& pump) {
    Fixture fixture;
    const auto folder = fixture.root / "selected";
    fs::create_directories(folder / "nested");
    write(folder / "nested/file", "delete");
    write(fixture.root / "keep", "preserve");
    fs::create_symlink(fixture.root / "keep", folder / "link");
    fs::create_symlink("missing", folder / "broken");
    luciole::DirectorySession session;
    session.open(luciole::Location::fromPath(fixture.root.native()));
    pump.until([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);
    const auto found =
        std::ranges::find(session.snapshot().entries, std::string("selected"), &luciole::DirectoryEntry::name);
    TEST_CHECK(found != session.snapshot().entries.end());
    // Freeze the entry: asynchronous session updates can replace its storage.
    const auto selected = *found; // NOLINT(performance-unnecessary-copy-initialization)
    luciole::EmptyTrashService service;
    TEST_CHECK(service.prepareDeletion({selected}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canEmpty() && read(folder / "nested/file") == "delete");
    service.cancel();
    TEST_CHECK(service.deleteSelection({selected}));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!service.empty() && read(folder / "nested/file") == "delete");
    TEST_CHECK(service.deleteSelection({selected}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == Status::Finished && service.snapshot().outsideTrash);
    TEST_CHECK(!fs::exists(folder) && read(fixture.root / "keep") == "preserve");
    TEST_CHECK(service.snapshot().results.size() == 1 && service.snapshot().results[0].outcome == Outcome::Deleted);
  }
  void successfulEmptyAndNewArrivals(Pump& pump) {
    Fixture fixture;
    luciole::EmptyTrashService service;
    pump.prepare(service);
    TEST_CHECK(service.snapshot().results.empty() && !service.canEmpty() && !service.empty());
    fs::create_directories(fixture.root / "first/Folder 文");
    fs::create_directories(fixture.root / "second");
    write(fixture.root / "first/same #%.txt", "one");
    write(fixture.root / "second/same #%.txt", "two");
    write(fixture.root / "first/Folder 文/child", "nested");
    write(fixture.root / "outside", "untouched");
    fs::create_symlink(fixture.root / "outside", fixture.root / "link");
    fs::create_symlink("missing", fixture.root / "broken");
    fs::create_directory_symlink(fixture.root / "second", fixture.root / "first/Folder 文/external");
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.root / raw, "raw");
    write(fixture.root / ".hidden", "hidden");
    for (const auto& path :
         {fixture.root / "first/same #%.txt", fixture.root / "second/same #%.txt", fixture.root / "first/Folder 文",
          fixture.root / "link", fixture.root / "broken", fixture.root / raw, fixture.root / ".hidden"}) {
      trash(path);
    }
    pump.prepare(service);
    TEST_CHECK(service.snapshot().results.size() == 7 && service.canEmpty());
    const auto expectedSize =
        28U + (fixture.root / "outside").native().size() + (fixture.root / "second").native().size();
    TEST_CHECK(service.snapshot().size == expectedSize && !service.snapshot().sizeIncomplete);
    TEST_CHECK(std::distance(fs::directory_iterator(trashInfo()), fs::directory_iterator{}) == 7);
    service.cancel();
    TEST_CHECK(!service.canEmpty() && fs::exists(trashFiles() / "same #%.txt"));
    pump.prepare(service);
    write(fixture.root / "arrived-later", "retain");
    trash(fixture.root / "arrived-later");
    pump.empty(service);
    TEST_CHECK(
        service.snapshot().results.size() == 7
        && service.completed() == 7
        && std::ranges::all_of(service.snapshot().results, [](const auto& item) {
             return item.outcome == Outcome::Deleted;
           })
    );
    TEST_CHECK(read(fixture.root / "outside") == "untouched" && fs::is_directory(fixture.root / "second"));
    TEST_CHECK(
        fs::exists(trashFiles() / "arrived-later")
        && fs::exists(trashInfo() / "arrived-later.trashinfo")
        && std::distance(fs::directory_iterator(trashInfo()), fs::directory_iterator{}) == 1
    );
    pump.prepare(service);
    pump.empty(service);
    pump.prepare(service);
    TEST_CHECK(service.snapshot().results.empty() && fs::is_empty(trashInfo()) && fs::is_empty(trashFiles()));
    service.cancel();
  }
  void selectedDeletion(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "keep-selected-test", "keep unrelated");
    write(fixture.root / "delete-selected-test", "delete selected");
    for (const auto* name : {"keep-selected-test", "delete-selected-test"}) {
      trash(fixture.root / name);
    }
    const auto selected = [&](const char* name = "delete-selected-test") {
      luciole::DirectorySession session;
      session.open(luciole::Location::trash());
      pump.until([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
      TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);
      const auto found = std::ranges::find_if(session.snapshot().entries, [&](const auto& entry) {
        return entry.trashInfo().original == luciole::Location::fromPath((fixture.root / name).native());
      });
      TEST_CHECK(found != session.snapshot().entries.end());
      return *found;
    };
    luciole::EmptyTrashService service;
    TEST_CHECK(!service.prepareSelection({}));
    auto entry = selected();
    TEST_CHECK(service.prepareSelection({entry}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canEmpty() && service.snapshot().results.size() == 1 && service.snapshot().size == 15);
    service.cancel();
    TEST_CHECK(!service.empty() && read(trashFiles() / "delete-selected-test") == "delete selected");
    TEST_CHECK(service.prepareSelection({entry}));
    pump.until([&] { return !service.busy(); });
    write(trashFiles() / "delete-selected-test", "changed after confirmation");
    pump.empty(service);
    TEST_CHECK(service.snapshot().results.front().outcome == Outcome::Failed);
    TEST_CHECK(read(trashFiles() / "keep-selected-test") == "keep unrelated");
    TEST_CHECK(service.prepareSelection({entry}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == Status::Error && !service.empty());
    entry = selected();
    TEST_CHECK(service.deleteSelection({entry}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == Status::Finished && !service.canEmpty());
    TEST_CHECK(service.snapshot().results.front().outcome == Outcome::Deleted);
    TEST_CHECK(
        !fs::exists(trashFiles() / "delete-selected-test")
        && read(trashFiles() / "keep-selected-test") == "keep unrelated"
    );
    entry.place = luciole::Location::trash();
    TEST_CHECK(service.prepareSelection({entry}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == Status::Error && !service.empty());
    TEST_CHECK(service.deleteSelection({selected("keep-selected-test")}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().results.front().outcome == Outcome::Deleted);
  }
  void staleItemsAndPartialOutcomes(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "replaced", "original");
    write(fixture.root / "good", "remove");
    write(fixture.root / "restored", "restore");
    for (const auto* name : {"replaced", "good", "restored"}) {
      trash(fixture.root / name);
    }
    luciole::EmptyTrashService service;
    pump.prepare(service);
    fs::rename(trashFiles() / "replaced", fixture.root / "held-original");
    write(trashFiles() / "replaced", "replacement");
    fs::rename(trashFiles() / "restored", fixture.root / "restored");
    fs::remove(trashInfo() / "restored.trashinfo");
    pump.empty(service);
    TEST_CHECK(
        std::ranges::count(service.snapshot().results, Outcome::Deleted, &luciole::EmptyTrashResult::outcome) == 1
        && std::ranges::count(service.snapshot().results, Outcome::Failed, &luciole::EmptyTrashResult::outcome) == 1
        && std::ranges::count(service.snapshot().results, Outcome::Skipped, &luciole::EmptyTrashResult::outcome) == 1
    );
    TEST_CHECK(
        read(trashFiles() / "replaced") == "replacement"
        && read(fixture.root / "held-original") == "original"
        && read(fixture.root / "restored") == "restore"
    );
    pump.prepare(service);
    pump.empty(service);
  }
  void cancellationBetweenItems(Pump& pump) {
    Fixture fixture;
    for (int index = 0; index < 128; ++index) {
      const auto path = fixture.root / ("cancel-" + std::to_string(index));
      write(path, "content");
      trash(path);
    }
    luciole::EmptyTrashService service;
    pump.prepare(service);
    TEST_CHECK(service.empty());
    pump.until([&] { return service.completed() != 0 || !service.busy(); });
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == Status::Finished);
    for (const auto& result : service.snapshot().results) {
      const auto name = result.location.basename();
      TEST_CHECK(name);
      const bool exists = fs::exists(trashFiles() / *name);
      TEST_CHECK((result.outcome == Outcome::Deleted) == !exists);
    }
    service.cancel();
    TEST_CHECK(service.snapshot().status == Status::Finished);
    pump.prepare(service);
    if (service.canEmpty()) {
      pump.empty(service);
    } else {
      service.cancel();
    }
  }
  void confirmationDoesNotWaitForSize(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "large-folder");
    for (int index = 0; index < 256; ++index) {
      write(fixture.root / "large-folder" / std::to_string(index), "data");
    }
    trash(fixture.root / "large-folder");
    luciole::EmptyTrashService service;
    bool requested = false, accepted = false, measuring = false;
    service.setChangedCallback([&] {
      if (!requested && service.snapshot().status == Status::Ready) {
        requested = true;
        measuring = service.busy() && !service.snapshot().size;
        accepted = service.empty();
      }
    });
    TEST_CHECK(service.prepare());
    pump.until([&] {
      return service.snapshot().status == Status::Finished || service.snapshot().status == Status::Error;
    });
    TEST_CHECK(
        requested
        && measuring
        && accepted
        && service.snapshot().results.front().outcome == Outcome::Deleted
        && !fs::exists(trashFiles() / "large-folder")
    );
    service.setChangedCallback({});
  }
  void trashAvailability(Pump& pump) {
    luciole::TrashAvailability status;
    const auto count = [&] {
      bool done = false;
      std::optional<std::uint32_t> result;
      status.query([&](auto value) {
        result = value;
        done = true;
      });
      TEST_CHECK(!done);
      pump.until([&] { return done; });
      return result;
    };
    TEST_CHECK(count() == 0);
    bool obsolete = false;
    status.query([&](auto) { obsolete = true; });
    TEST_CHECK(count() == 0 && !obsolete);
    status.query([&](auto) { obsolete = true; });
    status.cancel();
    TEST_CHECK(count() == 0 && !obsolete);
    {
      luciole::TrashAvailability retired;
      retired.query([&](auto) { obsolete = true; });
    }
    TEST_CHECK(count() == 0 && !obsolete);
    Fixture fixture;
    write(fixture.root / ".hidden-status", "disposable");
    trash(fixture.root / ".hidden-status");
    TEST_CHECK(count() == 1);
    luciole::EmptyTrashService service;
    pump.prepare(service);
    pump.empty(service);
    TEST_CHECK(count() == 0);
  }
  void unavailableTrashAvailability(Pump& pump) {
    luciole::TrashAvailability status;
    bool done = false;
    status.query([&](auto count) {
      TEST_CHECK(!count);
      done = true;
    });
    pump.until([&] { return done; });
  }
  void incompleteSize(Pump& pump) {
    if (::geteuid() == 0) {
      return;
    }
    Fixture fixture;
    fs::create_directory(fixture.root / "unreadable");
    write(fixture.root / "unreadable/content", "retain");
    trash(fixture.root / "unreadable");
    fs::permissions(trashFiles() / "unreadable", fs::perms::none);
    luciole::EmptyTrashService service;
    pump.prepare(service);
    TEST_CHECK(service.snapshot().sizeIncomplete);
    fs::permissions(trashFiles() / "unreadable", fs::perms::owner_all);
    pump.prepare(service);
    TEST_CHECK(service.snapshot().size == 6 && !service.snapshot().sizeIncomplete);
    pump.empty(service);
  }
} // namespace

int main(int argc, char** argv) {
  try {
    Pump pump;
    if (argc == 2 && std::string_view(argv[1]) == "--selected-provider") {
      selectedDeletion(pump);
    } else if (argc == 2 && std::string_view(argv[1]) == "--provider") {
      successfulEmptyAndNewArrivals(pump);
      selectedDeletion(pump);
      staleItemsAndPartialOutcomes(pump);
      cancellationBetweenItems(pump);
      confirmationDoesNotWaitForSize(pump);
      incompleteSize(pump);
      trashAvailability(pump);
    } else {
      TEST_CHECK(argc == 1);
      nativeDeletion(pump);
      unavailableAndLifetime(pump);
      unavailableTrashAvailability(pump);
    }
    std::println("empty-trash-service checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "empty-trash-service: {}", error.what());
    return 1;
  }
}
