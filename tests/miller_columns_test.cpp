#include "browser/miller_columns.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::BrowserController;
  using luciole::DirectoryStatus;
  using luciole::Location;
  using luciole::MillerColumns;

  class Fixture {
  public:
    Fixture() {
      const luciole::GString name(g_dir_make_tmp("luciole-miller-XXXXXX", nullptr));
      TEST_CHECK(name != nullptr);
      path = name.get();
      fs::create_directories(path / "a" / "nested");
      fs::create_directory(path / "b");
      std::ofstream(path / "a" / "file.txt") << "column fixture";
      std::ofstream(path / "a" / "nested" / "kept.txt") << "selected fixture";
      fs::create_directory_symlink("..", path / "a" / "cycle");
    }
    ~Fixture() {
      std::error_code ignored;
      fs::permissions(path / "locked", fs::perms::owner_all, ignored);
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path path;
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    void ready(const BrowserController& browser) {
      until([&] { return browser.model().snapshot().status != DirectoryStatus::Loading; });
      TEST_CHECK(browser.model().snapshot().status == DirectoryStatus::Ready);
    }
    void loaded(MillerColumns& columns) {
      const auto count = columns.columns().size();
      for (auto i = count > 3 ? count - 3 : 0; i < count; ++i) {
        columns.load(i);
      }
      until([&] {
        columns.synchronize();
        return std::ranges::all_of(columns.columns(), [](const auto& column) {
          return column->browser->model().snapshot().status != DirectoryStatus::Loading;
        });
      });
      columns.synchronize();
    }

  private:
    GlibPollSource m_source;
  };
  luciole::EntryId entry(const BrowserController& browser, const std::string& name) {
    for (const auto& item : browser.model().snapshot().entries) {
      if (item.name == name) {
        return item.id;
      }
    }
    throw std::runtime_error("missing entry: " + name);
  }

  std::size_t columnAt(const MillerColumns& columns, const fs::path& path) {
    const auto location = Location::fromPath(path.native());
    for (std::size_t i = 0; i < columns.columns().size(); ++i) {
      if (columns.columns()[i]->location == location) {
        return i;
      }
    }
    throw std::runtime_error("missing ancestor: " + path.native());
  }

  void providerShortcutColumns() {
    Fixture fixture;
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    MillerColumns columns;
    columns.start(source, false);
    pump.loaded(columns);
    auto& parent = columns.active();
    // A late metadata publication would replace the injected listing and drop its selection.
    pump.until([&] { return !parent.model().snapshot().metadataLoading; });
    auto snapshot = parent.model().snapshot();
    luciole::DirectoryEntry shortcut{
        .id = 10000,
        .place = Location::network().child("server"),
        .name = "server",
        .type = luciole::FileType::Shortcut,
        .size = {},
        .modifiedSeconds = {}
    };
    const auto first = Location::fromPath((fixture.path / "a").native());
    shortcut.setShortcutTarget(first);
    snapshot.entries = {shortcut};
    parent.model().applySnapshot(snapshot);
    TEST_CHECK(parent.model().select(shortcut.id));
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 2 && columns.columns().back()->location == first);
    pump.loaded(columns);
    const auto replacement = Location::fromPath((fixture.path / "b").native());
    shortcut.setShortcutTarget(replacement);
    snapshot.entries = {shortcut};
    parent.model().applySnapshot(snapshot);
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 2 && columns.columns().back()->location == replacement);
    pump.loaded(columns);
    TEST_CHECK(columns.columns().back()->browser->model().rows().empty());
  }

  void parentChainAndHistory() {
    Fixture fixture;
    std::ofstream(fixture.path / "a" / "nested" / "kept.txt~") << "backup";
    Pump pump;
    BrowserController source({}, false);
    source.setShowBackups(false);
    source.setSearchRecursive(false);
    const auto current = Location::fromPath((fixture.path / "a" / "nested").native());
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    source.open(current);
    pump.ready(source);
    source.model().select(entry(source, "kept.txt"));
    source.setCaptureViewport([] {
      return luciole::BrowseViewport{.mode = luciole::ViewMode::List, .iconSize = 160, .vertical = 45};
    });
    MillerColumns columns;
    columns.start(source);
    TEST_CHECK(std::ranges::all_of(columns.columns(), [](const auto& column) {
      return !column->browser->model().filter().showBackups && !column->browser->searchRecursive();
    }));
    TEST_CHECK(columns.columns().front()->browser->model().snapshot().status == DirectoryStatus::Idle);
    pump.loaded(columns);
    TEST_CHECK(
        columns.focused() + 1 == columns.columns().size() && columns.active().model().snapshot().location == current
    );
    TEST_CHECK(columns.columns().front()->location == Location::fromPath("/"));
    TEST_CHECK(columns.active().model().rows().size() == 1);
    TEST_CHECK(columns.active().model().selection().selected.contains(entry(columns.active(), "kept.txt")));
    const auto viewport = columns.active().takeRestoredViewport();
    TEST_CHECK(viewport && viewport->vertical == 45 && viewport->iconSize == 160);
    const auto parentIndex = columnAt(columns, fixture.path / "a");
    const auto& parent = *columns.columns()[parentIndex]->browser;
    TEST_CHECK(parent.model().selection().selected.contains(entry(parent, "nested")));
    TEST_CHECK(!parent.canBack());
    TEST_CHECK(columns.active().back());
    pump.loaded(columns);
    TEST_CHECK(columns.active().model().snapshot().location == Location::fromPath(fixture.path.native()));
    TEST_CHECK(columns.active().forward());
    pump.loaded(columns);
    TEST_CHECK(columns.active().model().snapshot().location == current);
    columns.active().open(Location::fromPath((fixture.path / "b").native()));
    pump.loaded(columns);
    TEST_CHECK(
        columns.columns().front()->location == Location::fromPath("/")
        && columns.active().model().snapshot().location == Location::fromPath((fixture.path / "b").native())
    );
  }

  void hiddenLinksAndFailedParents() {
    Fixture fixture;
    Pump pump;
    fs::create_directories(fixture.path / ".hidden" / "leaf");
    fs::create_directory_symlink("a", fixture.path / "link");
    BrowserController source({}, false);
    source.open(Location::fromPath((fixture.path / ".hidden" / "leaf").native()));
    pump.ready(source);
    MillerColumns columns;
    columns.start(source);
    pump.loaded(columns);
    TEST_CHECK(columns.active().model().snapshot().location == source.model().snapshot().location);
    const auto hiddenParent = columnAt(columns, fixture.path);
    TEST_CHECK(columns.columns()[hiddenParent]->browser->model().selection().selected.empty());
    columns.focus(hiddenParent);
    columns.interact(hiddenParent);
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == hiddenParent + 1);
    source.open(Location::fromPath((fixture.path / "link" / "nested").native()));
    pump.ready(source);
    columns.start(source);
    pump.loaded(columns);
    TEST_CHECK(columns.active().model().snapshot().location == source.model().snapshot().location);
    const auto linkParent = columnAt(columns, fixture.path);
    const auto& parent = *columns.columns()[linkParent]->browser;
    TEST_CHECK(parent.model().selection().selected.contains(entry(parent, "link")));
    source.open(Location::fromPath((fixture.path / "missing" / "leaf").native()));
    pump.until([&] { return source.model().snapshot().status == DirectoryStatus::Error; });
    columns.start(source);
    pump.loaded(columns);
    TEST_CHECK(
        columns.active().model().snapshot().status == DirectoryStatus::Error
        && columns.active().model().snapshot().location == source.model().snapshot().location
    );
    if (::getuid() != 0) {
      fs::create_directories(fixture.path / "locked" / "leaf");
      fs::permissions(fixture.path / "locked", fs::perms::owner_exec);
      source.open(Location::fromPath((fixture.path / "locked" / "leaf").native()));
      pump.ready(source);
      columns.start(source);
      pump.loaded(columns);
      TEST_CHECK(
          columns.active().model().snapshot().status == DirectoryStatus::Ready
          && columns.columns()[columnAt(columns, fixture.path / "locked")]->browser->model().snapshot().status
              == DirectoryStatus::Error
      );
    }
  }

  void boundedEntryAndEarlyInteraction() {
    Fixture fixture;
    auto path = fixture.path;
    for (std::size_t i = 0; i < MillerColumns::maxColumns + 2; ++i) {
      path /= "deep";
      fs::create_directory(path);
    }
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(path.native()));
    pump.ready(source);
    MillerColumns columns;
    columns.start(source);
    TEST_CHECK(columns.columns().size() == MillerColumns::maxColumns);
    pump.loaded(columns);
    TEST_CHECK(columns.active().model().snapshot().location == source.model().snapshot().location);
    source.open(Location::fromPath((fixture.path / "a" / "nested").native()));
    pump.ready(source);
    columns.start(source);
    const auto index = columnAt(columns, fixture.path);
    columns.load(index);
    pump.ready(*columns.columns()[index]->browser);
    columns.focus(index);
    columns.interact(index);
    auto& parent = *columns.columns()[index]->browser;
    parent.model().select(entry(parent, "b"));
    pump.loaded(columns);
    TEST_CHECK(columns.columns().back()->location == Location::fromPath((fixture.path / "b").native()));
    columns.start(source, false);
    pump.loaded(columns);
    TEST_CHECK(
        columns.columns().size() == 1
        && columns.active().model().snapshot().location == source.model().snapshot().location
    );
  }

  void branchesAndSelection() {
    Fixture fixture;
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    source.setCaptureViewport([] {
      return luciole::BrowseViewport{.mode = luciole::ViewMode::Miller, .iconSize = 192};
    });
    MillerColumns columns;
    columns.start(source, false);
    pump.ready(columns.active());
    TEST_CHECK(columns.active().contextSerial() != source.contextSerial());
    auto& parent = columns.active();
    parent.model().select(entry(parent, "a"));
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 2 && columns.focused() == 0);
    TEST_CHECK(columns.enter());
    pump.ready(columns.active());
    TEST_CHECK(columns.active().model().snapshot().location == Location::fromPath((fixture.path / "a").native()));
    TEST_CHECK(columns.active().viewport().iconSize == 192);
    columns.columns()[0]->width = 345;
    columns.active().model().select(entry(columns.active(), "nested"));
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 3);
    auto* old = columns.columns()[2]->browser.get();
    TEST_CHECK(columns.focus(0));
    parent.model().select(entry(parent, "b"));
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 2);
    TEST_CHECK(columns.columns()[1]->location == Location::fromPath((fixture.path / "b").native()));
    TEST_CHECK(columns.columns()[0]->width == 345);
    TEST_CHECK(old->model().snapshot().status == DirectoryStatus::Cancelled);
    parent.model().select(entry(parent, "a"), luciole::SelectionMode::Toggle);
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 1);
    columns.collectRetired();
    parent.model().clearSelection();
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 1 && !columns.enter());
  }

  void historySearchAndReplacement() {
    Fixture fixture;
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    MillerColumns columns;
    columns.start(source, false);
    pump.ready(columns.active());
    columns.active().model().select(entry(columns.active(), "a"));
    TEST_CHECK(columns.enter());
    pump.ready(columns.active());
    const auto context = columns.active().contextSerial();
    columns.active().model().select(entry(columns.active(), "cycle"));
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 2);
    columns.active().model().select(entry(columns.active(), "nested"));
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 3);
    columns.active().setSearchQuery("file");
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 2);
    pump.ready(columns.active());
    columns.active().setSearchQuery("");
    TEST_CHECK(columns.active().back());
    columns.synchronize();
    pump.ready(columns.active());
    TEST_CHECK(
        columns.focused() == 0 && columns.active().model().snapshot().location == source.model().snapshot().location
    );
    TEST_CHECK(columns.active().contextSerial() != context);
    TEST_CHECK(columns.active().forward());
    columns.synchronize();
    pump.ready(columns.active());
    TEST_CHECK(columns.active().model().snapshot().location == Location::fromPath((fixture.path / "a").native()));
    columns.active().model().select(entry(columns.active(), "nested"));
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 2);
    const auto oldId = columns.columns().back()->id;
    fs::rename(fixture.path / "a" / "nested", fixture.path / "a" / "retired");
    fs::create_directory(fixture.path / "a" / "nested");
    columns.active().refresh();
    pump.ready(columns.active());
    columns.synchronize();
    TEST_CHECK(columns.columns().size() == 1 || columns.columns().back()->id != oldId);
    columns.clear();
    columns.collectRetired();
  }

  void resourceBoundAndIndependentPanes() {
    Fixture fixture;
    auto path = fixture.path;
    for (std::size_t i = 0; i < MillerColumns::maxColumns + 2; ++i) {
      path /= "deep";
      fs::create_directory(path);
    }
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    MillerColumns first, other;
    first.start(source, false);
    other.start(source, false);
    pump.ready(other.active());
    const auto otherContext = other.active().contextSerial();
    for (std::size_t i = 0; i < MillerColumns::maxColumns + 2; ++i) {
      pump.ready(first.active());
      first.active().model().select(entry(first.active(), "deep"));
      TEST_CHECK(first.enter());
      TEST_CHECK(first.columns().size() <= MillerColumns::maxColumns);
    }
    TEST_CHECK(first.columns().size() == MillerColumns::maxColumns);
    TEST_CHECK(
        other.columns().size() == 1
        && other.active().contextSerial() == otherContext
        && other.active().model().selection().selected.empty()
    );
    first.clear();
    first.collectRetired();
  }
} // namespace

int main() {
  try {
    branchesAndSelection();
    providerShortcutColumns();
    parentChainAndHistory();
    hiddenLinksAndFailedParents();
    boundedEntryAndEarlyInteraction();
    historySearchAndReplacement();
    resourceBoundAndIndependentPanes();
    std::println("Miller column checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    return 1;
  }
}
