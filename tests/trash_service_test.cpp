#include "browser/action_registry.h"
#include "browser/directory_model.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "fs/recursive_search.h"
#include "operations/empty_trash_service.h"
#include "operations/trash_service.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }
  void write(const fs::path& path, const std::string& value) { std::ofstream(path) << value; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  struct Fixture {
    fs::path root, home, data;
    Fixture() {
      std::string path = "/tmp/luciole-trash-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()) != nullptr);
      root = path;
      home = root / "home";
      data = home / ".local/share";
      fs::create_directories(data);
      TEST_CHECK(::setenv("HOME", home.c_str(), 1) == 0);
      TEST_CHECK(::setenv("XDG_DATA_HOME", data.c_str(), 1) == 0);
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
    fs::path folder(const std::string& name) const {
      const auto path = home / name;
      fs::create_directories(path);
      return path;
    }
    std::vector<fs::path> archived(const fs::path& original) const {
      std::vector<fs::path> paths;
      const auto info = data / "Trash/info";
      if (!fs::exists(info)) {
        return paths;
      }
      for (const auto& entry : fs::directory_iterator(info)) {
        std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> key(g_key_file_new(), g_key_file_unref);
        TEST_CHECK(g_key_file_load_from_file(key.get(), entry.path().c_str(), G_KEY_FILE_NONE, nullptr));
        const luciole::GString encoded(g_key_file_get_string(key.get(), "Trash Info", "Path", nullptr));
        TEST_CHECK(encoded != nullptr);
        const luciole::GString decoded(g_uri_unescape_string(encoded.get(), nullptr));
        if (decoded && original.native() == decoded.get()) {
          const luciole::GString date(g_key_file_get_string(key.get(), "Trash Info", "DeletionDate", nullptr));
          TEST_CHECK(date && std::string(date.get()).size() == 19);
          paths.push_back(data / "Trash/files" / entry.path().stem());
        }
      }
      return paths;
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
    void prepare(luciole::TrashService& service, const fs::path& parent, const std::vector<fs::path>& paths) {
      std::vector<luciole::Location> sources;
      sources.reserve(paths.size());
      for (const auto& path : paths) {
        sources.push_back(location(path));
      }
      TEST_CHECK(service.prepare(location(parent), std::move(sources)));
      until([&] { return !service.busy(); });
      TEST_CHECK(service.canTrash());
    }
    void trash(luciole::TrashService& service) {
      TEST_CHECK(service.trash());
      until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == luciole::TrashStatus::Finished);
    }

  private:
    GlibPollSource m_source;
  };
  void preservesRecoverableContent(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("content");
    const auto file = parent / "résumé 文 #%.txt";
    write(file, "recoverable");
    fs::create_directory(parent / "folder");
    write(parent / "folder/child", "nested");
    const auto outside = fixture.folder("outside");
    write(outside / "keep", "untouched");
    fs::create_directory_symlink(outside, parent / "folder/external");
    fs::create_symlink(file, parent / "link");
    fs::create_symlink("absent", parent / "broken");
    TEST_CHECK(::mkfifo((parent / "fifo").c_str(), 0600) == 0);
    luciole::TrashService service;
    pump.prepare(service, parent, {file, parent / "folder", parent / "link", parent / "broken", parent / "fifo"});
    TEST_CHECK(fs::exists(file));
    pump.trash(service);
    TEST_CHECK(service.snapshot().results.size() == 5 && service.completed() == 5);
    TEST_CHECK(std::ranges::all_of(service.snapshot().results, [](const auto& item) {
      return item.outcome == luciole::TrashOutcome::Trashed;
    }));
    TEST_CHECK(fs::is_empty(parent));
    const auto files = fixture.archived(file);
    const auto folders = fixture.archived(parent / "folder");
    TEST_CHECK(files.size() == 1 && read(files.front()) == "recoverable");
    TEST_CHECK(folders.size() == 1 && read(folders.front() / "child") == "nested");
    TEST_CHECK(read(outside / "keep") == "untouched");
    const auto links = fixture.archived(parent / "link");
    const auto broken = fixture.archived(parent / "broken");
    TEST_CHECK(links.size() == 1 && fs::read_symlink(links.front()) == file);
    TEST_CHECK(broken.size() == 1 && fs::read_symlink(broken.front()) == "absent");
    const auto fifos = fixture.archived(parent / "fifo");
    TEST_CHECK(fifos.size() == 1 && fs::is_fifo(fifos.front()));
    write(file, "second version");
    pump.prepare(service, parent, {file});
    pump.trash(service);
    const auto versions = fixture.archived(file);
    TEST_CHECK(versions.size() == 2);
    TEST_CHECK(
        std::ranges::any_of(versions, [](const auto& path) { return read(path) == "recoverable"; })
        && std::ranges::any_of(versions, [](const auto& path) { return read(path) == "second version"; })
    );
  }
  void multipleParents(const Fixture& fixture, Pump& pump) {
    const auto first = fixture.folder("drop-first");
    const auto second = fixture.folder("drop-second");
    write(first / "same", "first");
    write(second / "same", "second");
    write(second / "keep", "untouched");
    fs::create_symlink(second / "keep", first / "link");
    luciole::TrashService service;
    TEST_CHECK(service.prepare({location(first / "same"), location(second / "same"), location(first / "link")}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canTrash() && fs::exists(first / "same") && fs::exists(second / "same"));
    pump.trash(service);
    TEST_CHECK(service.completed() == 3);
    const auto firstCopy = fixture.archived(first / "same"), secondCopy = fixture.archived(second / "same");
    const auto links = fixture.archived(first / "link");
    TEST_CHECK(firstCopy.size() == 1 && read(firstCopy.front()) == "first");
    TEST_CHECK(secondCopy.size() == 1 && read(secondCopy.front()) == "second");
    TEST_CHECK(links.size() == 1 && fs::read_symlink(links.front()) == second / "keep");
    TEST_CHECK(read(second / "keep") == "untouched");

    write(first / "item", "original");
    write(second / "item", "good");
    TEST_CHECK(service.prepare({location(first / "item"), location(second / "item")}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canTrash());
    fs::rename(first, fixture.home / "drop-original");
    fs::create_directory(first);
    write(first / "item", "replacement");
    pump.trash(service);
    TEST_CHECK(service.snapshot().results[0].outcome == luciole::TrashOutcome::Failed);
    TEST_CHECK(service.snapshot().results[1].outcome == luciole::TrashOutcome::Trashed);
    TEST_CHECK(read(first / "item") == "replacement" && read(fixture.home / "drop-original/item") == "original");
    TEST_CHECK(fixture.archived(second / "item").size() == 1);
  }
  void recursiveSearchSelection(const Fixture& fixture, Pump& pump) {
    const auto root = fixture.folder("search");
    fs::create_directories(root / "first");
    fs::create_directories(root / "second");
    write(root / "first/match.txt", "first result");
    write(root / "second/match.txt", "second result");
    write(root / "keep.txt", "unselected");
    fs::create_symlink(root / "keep.txt", root / "match-link");
    luciole::RecursiveSearch search;
    luciole::DirectoryModel model;
    search.setChangedCallback([&] { model.applySnapshot(search.snapshot()); });
    search.start(location(root), "match", false);
    pump.until([&] { return search.snapshot().status == luciole::DirectoryStatus::Ready; });
    TEST_CHECK(model.rows().size() == 3);
    model.selectAll();
    luciole::ActionContext context{.model = model, .context = 79, .query = "match", .pinned = {}};
    const auto request = luciole::ActionRegistry::capture(context);
    TEST_CHECK(luciole::ActionRegistry::state(luciole::BrowserAction::Trash, context, request).enabled);
    std::vector<luciole::Location> sources;
    sources.reserve(request.targets.size());
    for (const auto& target : request.targets) {
      sources.push_back(target.location);
    }
    luciole::TrashService service;
    TEST_CHECK(service.prepare(std::move(sources)));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canTrash());
    pump.trash(service);
    TEST_CHECK(std::ranges::all_of(service.snapshot().results, [](const auto& result) {
      return result.outcome == luciole::TrashOutcome::Trashed;
    }));
    const auto first = fixture.archived(root / "first/match.txt");
    const auto second = fixture.archived(root / "second/match.txt");
    const auto link = fixture.archived(root / "match-link");
    TEST_CHECK(first.size() == 1 && read(first.front()) == "first result");
    TEST_CHECK(second.size() == 1 && read(second.front()) == "second result");
    TEST_CHECK(link.size() == 1 && fs::read_symlink(link.front()) == root / "keep.txt");
    TEST_CHECK(
        read(root / "keep.txt") == "unselected" && fs::is_empty(root / "first") && fs::is_empty(root / "second")
    );
    search.start(location(root), "match", false);
    pump.until([&] { return search.snapshot().status == luciole::DirectoryStatus::Ready; });
    TEST_CHECK(model.rows().empty());
    write(root / "first/match.txt", "permanent first");
    write(root / "second/match.txt", "permanent second");
    const auto selected = [&] {
      search.start(location(root), "match", false);
      pump.until([&] { return search.snapshot().status == luciole::DirectoryStatus::Ready; });
      return std::vector<luciole::DirectoryEntry>(search.snapshot().entries.begin(), search.snapshot().entries.end());
    };
    auto targets = selected();
    TEST_CHECK(targets.size() == 2);
    luciole::EmptyTrashService deletion;
    TEST_CHECK(deletion.prepareDeletion(targets));
    pump.until([&] { return !deletion.busy(); });
    TEST_CHECK(deletion.canEmpty() && read(root / "first/match.txt") == "permanent first");
    deletion.cancel();
    write(root / "first/match.txt", "changed since search");
    TEST_CHECK(deletion.prepareDeletion(targets));
    pump.until([&] { return !deletion.busy(); });
    TEST_CHECK(deletion.snapshot().status == luciole::EmptyTrashStatus::Error);
    TEST_CHECK(deletion.snapshot().error && deletion.snapshot().error->kind == luciole::FileErrorKind::Changed);
    TEST_CHECK(read(root / "second/match.txt") == "permanent second");
    targets = selected();
    TEST_CHECK(deletion.deleteSelection(targets));
    pump.until([&] { return !deletion.busy(); });
    TEST_CHECK(deletion.snapshot().status == luciole::EmptyTrashStatus::Finished);
    TEST_CHECK(std::ranges::all_of(deletion.snapshot().results, [](const auto& result) {
      return result.outcome == luciole::EmptyTrashOutcome::Deleted;
    }));
    TEST_CHECK(!fs::exists(root / "first/match.txt") && !fs::exists(root / "second/match.txt"));
    TEST_CHECK(read(root / "keep.txt") == "unselected");
    search.setChangedCallback({});
  }

  void invalidDropPayloads(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("drop-invalid");
    fs::create_directory(parent / "a");
    write(parent / "a/child", "child");
    write(parent / "a-", "between");
    write(parent / "keep", "keep");
    luciole::TrashService service;
    TEST_CHECK(!service.prepare(std::vector<luciole::Location>{}));
    for (const auto& sources : std::vector<std::vector<luciole::Location>>{
             {location(parent / "keep"), luciole::Location::fromUri("sftp://example.invalid/file")},
             {location(parent / "keep"), luciole::Location::trash()},
             {location(parent / "keep"), location("/")},
             {location(parent / "keep"), location(parent / "keep")},
             {location(parent / "keep"), location(parent / "missing")},
             {location(parent / "a"), location(parent / "a-"), location(parent / "a/child")}
         }) {
      TEST_CHECK(service.prepare(sources));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == luciole::TrashStatus::Error && !service.canTrash());
      TEST_CHECK(read(parent / "keep") == "keep" && read(parent / "a/child") == "child");
      TEST_CHECK(read(parent / "a-") == "between");
    }
    TEST_CHECK(service.prepare({location(parent / "keep")}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.canTrash());
    service.cancel();
    TEST_CHECK(!service.canTrash() && !service.trash() && read(parent / "keep") == "keep");
  }
  void rawNames(const Fixture& fixture, Pump& pump) {
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    const auto parent = fixture.folder(raw);
    const auto file = parent / raw;
    write(file, "raw");
    luciole::TrashService service;
    pump.prepare(service, parent, {file});
    pump.trash(service);
    const auto archived = fixture.archived(file);
    TEST_CHECK(archived.size() == 1 && read(archived.front()) == "raw");
  }
  void staleTargetsAndPartialResults(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("partial");
    for (const auto* name : {"replaced", "good", "missing"}) {
      write(parent / name, name);
    }
    luciole::TrashService service;
    pump.prepare(service, parent, {parent / "replaced", parent / "good", parent / "missing"});
    fs::rename(parent / "replaced", parent / "original");
    write(parent / "replaced", "replacement");
    fs::remove(parent / "missing");
    pump.trash(service);
    const auto& results = service.snapshot().results;
    TEST_CHECK(
        results[0].outcome == luciole::TrashOutcome::Failed
        && results[1].outcome == luciole::TrashOutcome::Trashed
        && results[2].outcome == luciole::TrashOutcome::Failed
    );
    TEST_CHECK(read(parent / "replaced") == "replacement" && read(parent / "original") == "replaced");
    const auto stale = fixture.folder("stale");
    write(stale / "item", "original");
    pump.prepare(service, stale, {stale / "item"});
    fs::rename(stale, fixture.home / "old-stale");
    fs::create_directory(stale);
    write(stale / "item", "replacement");
    pump.trash(service);
    TEST_CHECK(
        service.snapshot().results.front().outcome == luciole::TrashOutcome::Failed
        && read(stale / "item") == "replacement"
        && read(fixture.home / "old-stale/item") == "original"
    );
    const auto alias = fixture.home / "alias";
    fs::create_directory_symlink(stale, alias);
    pump.prepare(service, alias, {alias / "item"});
    fs::remove(alias);
    fs::create_directory_symlink(fixture.home / "old-stale", alias);
    pump.trash(service);
    TEST_CHECK(
        service.snapshot().results.front().outcome == luciole::TrashOutcome::Failed
        && fs::exists(stale / "item")
        && fs::exists(alias / "item")
    );
  }
  void rejectsInvalidRequests(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("invalid");
    write(parent / "keep", "keep");
    luciole::TrashService service;
    for (const auto& sources : std::vector<std::vector<luciole::Location>>{
             {location(parent / "keep"), location(parent / "keep")},
             {location(parent / "keep"), location(parent / "missing")},
             {location(parent)},
             {location(fixture.home / "elsewhere")},
             {luciole::Location::fromUri("sftp://example.invalid/file")}
         }) {
      TEST_CHECK(service.prepare(location(parent), sources));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == luciole::TrashStatus::Error && !service.canTrash());
      TEST_CHECK(read(parent / "keep") == "keep");
    }
    TEST_CHECK(service.prepare(luciole::Location::fromUri("sftp://example.invalid/"), {location(parent / "keep")}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == luciole::TrashStatus::Error);
  }
  void cancellationAndLifetime(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("cancelled");
    write(parent / "keep", "keep");
    luciole::TrashService service;
    pump.prepare(service, parent, {parent / "keep"});
    service.cancel();
    TEST_CHECK(!service.trash() && read(parent / "keep") == "keep");
    TEST_CHECK(service.prepare(location(parent), {location(parent / "keep")}));
    TEST_CHECK(!service.prepare(location(parent), {location(parent / "keep")}) && !service.trash());
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(fs::exists(parent / "keep"));
    std::vector<fs::path> paths;
    for (int index = 0; index < 128; ++index) {
      paths.push_back(parent / ("item-" + std::to_string(index)));
      write(paths.back(), "data");
    }
    pump.prepare(service, parent, paths);
    TEST_CHECK(service.trash());
    pump.until([&] { return service.completed() != 0 || !service.busy(); });
    service.cancel();
    pump.until([&] { return !service.busy(); });
    for (const auto& result : service.snapshot().results) {
      const auto path = fs::path(*result.source.nativePath());
      TEST_CHECK((result.outcome == luciole::TrashOutcome::Trashed) == !fs::exists(path));
      if (result.outcome == luciole::TrashOutcome::Trashed) {
        TEST_CHECK(fixture.archived(path).size() == 1);
      }
    }
    service.cancel();
    TEST_CHECK(service.snapshot().status == luciole::TrashStatus::Finished);
    auto destroyed = std::make_unique<luciole::TrashService>();
    bool notified = false;
    destroyed->setChangedCallback([&] { notified = true; });
    TEST_CHECK(destroyed->prepare(location(parent), {location(parent / "keep")}));
    notified = false;
    destroyed.reset();
    // A second request lets the same default context dispatch the retired worker.
    pump.prepare(service, parent, {parent / "keep"});
    TEST_CHECK(!notified && fs::exists(parent / "keep"));
    service.cancel();
  }
  void permissionFailure(const Fixture& fixture, Pump& pump) {
    if (::geteuid() == 0) {
      return;
    }
    const auto parent = fixture.folder("permission");
    write(parent / "keep", "keep");
    luciole::TrashService service;
    pump.prepare(service, parent, {parent / "keep"});
    fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec);
    pump.trash(service);
    fs::permissions(parent, fs::perms::owner_all);
    TEST_CHECK(
        service.snapshot().results.front().outcome == luciole::TrashOutcome::Failed
        && read(parent / "keep") == "keep"
        && fixture.archived(parent / "keep").empty()
    );
  }
  void monitoredSelection(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("monitored");
    write(parent / "selected", "content");
    write(parent / "remaining", "keep");
    luciole::DirectorySession session;
    luciole::DirectoryModel model;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(parent));
    pump.until([&] { return model.snapshot().status == luciole::DirectoryStatus::Ready && model.rows().size() == 2; });
    const auto selected = std::ranges::find(model.snapshot().entries, "selected", &luciole::DirectoryEntry::name);
    TEST_CHECK(selected != model.snapshot().entries.end());
    const auto remaining = std::ranges::find(model.snapshot().entries, "remaining", &luciole::DirectoryEntry::name);
    TEST_CHECK(remaining != model.snapshot().entries.end());
    const auto id = selected->id;
    const auto remainingId = remaining->id;
    TEST_CHECK(model.select(id));
    luciole::TrashService service;
    pump.prepare(service, parent, {parent / "selected"});
    pump.trash(service);
    pump.until([&] { return model.rows().size() == 1 && model.entry(id) == nullptr; });
    TEST_CHECK(model.selectedIds() == std::vector{remainingId});
    TEST_CHECK(model.selection().focused == remainingId && model.selection().anchor == remainingId);
    TEST_CHECK(read(parent / "remaining") == "keep");
    const auto archived = fixture.archived(parent / "selected");
    TEST_CHECK(archived.size() == 1 && read(archived.front()) == "content");
    session.setChangedCallback({});
  }
} // namespace

int main() {
  try {
    Fixture fixture;
    Pump pump;
    preservesRecoverableContent(fixture, pump);
    multipleParents(fixture, pump);
    recursiveSearchSelection(fixture, pump);
    invalidDropPayloads(fixture, pump);
    rawNames(fixture, pump);
    staleTargetsAndPartialResults(fixture, pump);
    rejectsInvalidRequests(fixture, pump);
    cancellationAndLifetime(fixture, pump);
    permissionFailure(fixture, pump);
    monitoredSelection(fixture, pump);
    std::println("trash-service checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "trash-service: {}", error.what());
    return 1;
  }
}
