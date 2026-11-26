#include "browser/browser_controller.h"
#include "browser/entry_details.h"
#include "browser/location_presentation.h"
#include "event_loop/glib_poll_source.h"
#include "fs/folder_totals.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  using luciole::BrowserController;
  using luciole::DirectoryStatus;
  using luciole::EntryId;
  using luciole::Location;

  struct Fixture {
    fs::path path;
    Fixture() {
      const luciole::GString name(g_dir_make_tmp("luciole-browser-test-XXXXXX", nullptr));
      TEST_CHECK(name != nullptr);
      path = name.get();
      fs::create_directory(path / "child");
      fs::create_directory(path / "other");
      std::ofstream(path / "file") << "fixture";
      std::ofstream(path / "child" / "inside") << "child fixture";
      std::ofstream(path / ".hidden") << "hidden";
      fs::create_symlink("child", path / "folder-link");
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
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
    void drainFor(std::chrono::milliseconds duration) {
      const auto deadline = std::chrono::steady_clock::now() + duration;
      until([&] { return std::chrono::steady_clock::now() >= deadline; });
    }

  private:
    GlibPollSource m_source;
  };

  EntryId id(const BrowserController& browser, const std::string& name) {
    for (const auto& item : browser.model().snapshot().entries) {
      if (item.name == name) {
        return item.id;
      }
    }
    throw std::runtime_error("missing browser fixture entry: " + name);
  }

  void testBackupPolicyAndSearch() {
    Fixture fixture;
    std::ofstream(fixture.path / "file~") << "backup";
    std::ofstream(fixture.path / "file.bak") << "ordinary extension";
    std::ofstream(fixture.path / ".hidden~") << "hidden backup";
    std::ofstream(fixture.path / "child" / "inside~") << "nested backup";
    fs::create_directory(fixture.path / "cache~");
    std::ofstream(fixture.path / "cache~" / "inside") << "normal child of tilde directory";
    Pump pump;
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto visible = [&](const BrowserController& value, const std::string& name) {
      return std::ranges::any_of(value.model().rows(), [&](auto id) { return value.model().entry(id)->name == name; });
    };
    TEST_CHECK(visible(browser, "file~") && !visible(browser, ".hidden~"));
    browser.model().select(id(browser, "file~"));
    browser.setShowBackups(false);
    TEST_CHECK(!visible(browser, "file~") && browser.model().selection().selected.empty());
    TEST_CHECK(visible(browser, "file.bak") && visible(browser, "cache~"));
    browser.toggleHidden();
    TEST_CHECK(visible(browser, ".hidden") && !visible(browser, ".hidden~"));
    browser.setSearchQuery("inside");
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 2 && !visible(browser, "inside~"));
    TEST_CHECK(std::ranges::any_of(browser.model().rows(), [&](auto id) {
      return browser.model().entry(id)->location() == Location::fromPath((fixture.path / "cache~" / "inside").native());
    }));
    BrowserController duplicate({}, false);
    duplicate.duplicateFrom(browser);
    pump.ready(duplicate);
    TEST_CHECK(!duplicate.model().filter().showBackups && duplicate.model().rows().size() == 2);
    browser.setShowBackups(true);
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 3 && visible(browser, "inside~"));
    TEST_CHECK(!duplicate.model().filter().showBackups && !visible(duplicate, "inside~"));
    browser.setSearchQuery({});
    pump.ready(browser);
    TEST_CHECK(visible(browser, ".hidden~"));
    browser.toggleHidden();
    TEST_CHECK(visible(browser, "file~") && !visible(browser, ".hidden~"));
  }

  void testActionContextsAndFileLinks() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto original = browser.contextSerial();
    browser.refresh();
    pump.ready(browser);
    TEST_CHECK(browser.contextSerial() == original);
    browser.setSearchQuery("file");
    pump.ready(browser);
    const auto search = browser.contextSerial();
    TEST_CHECK(search != original);
    browser.setSearchQuery({});
    pump.ready(browser);
    TEST_CHECK(browser.contextSerial() != original && browser.contextSerial() != search);
    const auto beforeChild = browser.contextSerial();
    browser.activate(id(browser, "child"));
    pump.ready(browser);
    browser.back();
    pump.ready(browser);
    TEST_CHECK(browser.contextSerial() != beforeChild);
    fs::create_symlink("file", fixture.path / "file-link");
    browser.refresh();
    pump.ready(browser);
    std::optional<Location> launched;
    browser.setFileActivatedCallback([&](const Location& file) { launched = file; });
    browser.activate(id(browser, "file-link"));
    pump.until([&] { return launched.has_value(); });
    TEST_CHECK(
        launched == Location::fromPath((fixture.path / "file-link").native())
        && browser.model().snapshot().location == Location::fromPath(fixture.path.native())
    );
    launched.reset();
    browser.activate(id(browser, "file-link"));
    browser.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(browser);
    TEST_CHECK(!launched);
    auto retiring = std::make_unique<BrowserController>(luciole::SortOptions{}, false);
    retiring->open(Location::fromPath(fixture.path.native()));
    pump.ready(*retiring);
    retiring->activate(id(*retiring, "file-link"));
    retiring.reset();
  }

  void testBrokenAndChainedLinks() {
    Fixture fixture;
    fs::create_symlink("file", fixture.path / "file-link");
    fs::create_symlink("file-link", fixture.path / "chain");
    fs::create_symlink("missing", fixture.path / "broken");
    fs::create_symlink("loop-b", fixture.path / "loop-a");
    fs::create_symlink("loop-a", fixture.path / "loop-b");
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    browser.open(root);
    pump.ready(browser);
    std::optional<Location> launched;
    browser.setFileActivatedCallback([&](const Location& file) { launched = file; });
    browser.activate(id(browser, "chain"));
    pump.until([&] { return launched.has_value(); });
    TEST_CHECK(launched == root.child("chain"));
    for (const auto& name : {"broken", "loop-a"}) {
      launched.reset();
      browser.activate(id(browser, name));
      pump.until([&] { return launched.has_value() || !browser.message().empty(); });
      TEST_CHECK(!launched && !browser.message().empty());
      TEST_CHECK(browser.model().snapshot().location == root);
      browser.refresh();
      pump.ready(browser);
    }
    browser.activate(id(browser, "folder-link"));
    pump.until([&] { return browser.model().snapshot().location != root; });
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == root.child("folder-link"));
    TEST_CHECK(browser.model().snapshot().entries.size() == 1);
  }

  void testProviderShortcutNavigation() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    const auto target = root.child("child");
    browser.open(root);
    pump.ready(browser);
    std::optional<Location> launched;
    browser.setFileActivatedCallback([&](const Location& location) { launched = location; });
    auto snapshot = browser.model().snapshot();
    luciole::DirectoryEntry shortcut{
        .id = 10000,
        .place = Location::network().child("server"),
        .name = "server",
        .type = luciole::FileType::Shortcut,
        .size = {},
        .modifiedSeconds = {}
    };
    shortcut.setDisplayName("Server <literal> 文");
    shortcut.setShortcutTarget(target);
    snapshot.entries = {shortcut};
    browser.model().applySnapshot(snapshot);
    const auto retained = browser.model().snapshot();
    browser.activate(shortcut.id);
    pump.ready(browser);
    TEST_CHECK(!launched && browser.model().snapshot().location == target);
    TEST_CHECK(retained.entries.front().location() == shortcut.location());
    TEST_CHECK(retained.entries.front().navigationLocation() == target);
    TEST_CHECK(browser.back());
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == root);

    snapshot = browser.model().snapshot();
    shortcut.setShortcutTarget(std::nullopt);
    snapshot.entries = {shortcut};
    browser.model().applySnapshot(snapshot);
    browser.activate(shortcut.id);
    TEST_CHECK(!launched && !browser.message().empty() && browser.model().snapshot().location == root);
    shortcut.setShortcutTarget(shortcut.location());
    snapshot.entries = {shortcut};
    browser.model().applySnapshot(snapshot);
    browser.activate(shortcut.id);
    TEST_CHECK(!launched && browser.model().snapshot().location == root);
    // Copy-on-write target facts must not change a snapshot retained by another view.
    shortcut.setShortcutTarget(root.child("other"));
    TEST_CHECK(retained.entries.front().navigationLocation() == target);
  }

  void testDuplicatedTabs() {
    Fixture fixture;
    Pump pump;
    BrowserController original({}, false), duplicate({}, true);
    const auto root = Location::fromPath(fixture.path.native());
    original.open(root);
    pump.ready(original);
    original.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(original);
    original.model().select(id(original, "inside"));
    original.setCaptureViewport([] {
      return luciole::BrowseViewport{.mode = luciole::ViewMode::List, .iconSize = 96, .vertical = 125};
    });
    duplicate.open(Location::fromPath((fixture.path / "other").native()));
    duplicate.duplicateFrom(original);
    pump.ready(duplicate);
    TEST_CHECK(
        duplicate.canBack()
        && duplicate.model().selectedIds().size() == 1
        && duplicate.model().entry(duplicate.model().selectedIds().front())->name == "inside"
    );
    const auto viewport = duplicate.takeRestoredViewport();
    TEST_CHECK(viewport && viewport->mode == luciole::ViewMode::List && viewport->vertical == 125);
    duplicate.back();
    pump.ready(duplicate);
    TEST_CHECK(original.model().snapshot().location == Location::fromPath((fixture.path / "child").native()));
    original.setSearchQuery("inside");
    pump.ready(original);
    duplicate.duplicateFrom(original);
    pump.ready(duplicate);
    original.open(root);
    pump.ready(original);
    TEST_CHECK(duplicate.searchQuery() == "inside" && duplicate.model().rows().size() == 1);
    auto closing = std::make_unique<BrowserController>(luciole::SortOptions{}, false);
    closing->duplicateFrom(duplicate);
    closing.reset();
    pump.drainFor(30ms);
  }
  void testNavigation() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    TEST_CHECK(browser.openText(root.uri()));
    pump.ready(browser);
    TEST_CHECK(!browser.canBack() && !browser.canForward());
    browser.model().select(id(browser, "file"));
    const auto selected = browser.model().selectedIds();
    browser.open(*browser.model().snapshot().location);
    pump.ready(browser);
    TEST_CHECK(browser.model().selectedIds() == selected && !browser.canBack());
    browser.activate(id(browser, "child"));
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == Location::fromPath((fixture.path / "child").native()));
    TEST_CHECK(browser.openText("../other"));
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == Location::fromPath((fixture.path / "other").native()));
    TEST_CHECK(browser.back());
    pump.ready(browser);
    TEST_CHECK(
        browser.model().snapshot().location == Location::fromPath((fixture.path / "child").native())
        && browser.canForward()
    );
    TEST_CHECK(browser.forward());
    pump.ready(browser);
    TEST_CHECK(browser.up());
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == root);
    TEST_CHECK(browser.back());
    pump.ready(browser);
    browser.open(root);
    pump.ready(browser);
    TEST_CHECK(!browser.canForward());
    browser.activate(id(browser, "folder-link"));
    pump.until([&] {
      return browser.model().snapshot().location == Location::fromPath((fixture.path / "folder-link").native());
    });
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == Location::fromPath((fixture.path / "folder-link").native()));
    browser.activate(id(browser, "inside"));
    TEST_CHECK(!browser.message().empty() && browser.model().snapshot().status == DirectoryStatus::Ready);
  }

  void testFailureAndLiveUpdates() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto location = browser.model().snapshot().location;
    TEST_CHECK(!browser.openText("sftp://invalid%zz") && browser.model().snapshot().location == location);
    browser.toggleHidden();
    const auto hidden = id(browser, ".hidden");
    browser.model().select(hidden);
    browser.toggleHidden();
    TEST_CHECK(browser.model().selection().selected.empty());
    browser.activate(hidden);
    TEST_CHECK(browser.model().snapshot().location == location);
    browser.refresh();
    pump.ready(browser);
    const auto file = id(browser, "file");
    browser.model().select(file);
    std::ofstream(fixture.path / "added") << "new file";
    pump.until([&] { return browser.model().snapshot().entries.size() == 6; });
    TEST_CHECK(browser.model().selection().selected.contains(file));
    TEST_CHECK(browser.openText((fixture.path / "missing").native()));
    pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(browser.canBack() && browser.back());
    pump.ready(browser);
    browser.stop();
    TEST_CHECK(browser.model().snapshot().status == DirectoryStatus::Cancelled);
    browser.refresh();
    pump.ready(browser);
  }
  void testRecursiveSearch() {
    Fixture fixture;
    Fixture outside;
    fs::create_directories(fixture.path / "child/deep");
    fs::create_directory(fixture.path / ".private");
    fs::create_directory(fixture.path / "target-folder");
    for (const auto& name : {"target.txt", "child/target.txt", "child/deep/target.txt", ".private/target.txt"}) {
      std::ofstream(fixture.path / name) << "target";
    }
    std::ofstream(outside.path / "outside-target.txt") << "outside";
    std::ofstream(fixture.path / "Straße.txt") << "Unicode";
    std::ofstream(fixture.path / "*.txt") << "literal";
    fs::create_directory_symlink(outside.path, fixture.path / "external-link");
    fs::create_directory_symlink(".", fixture.path / "target-link");
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    browser.open(root);
    pump.ready(browser);
    const auto oldId = id(browser, "target.txt");
    browser.model().select(oldId);
    browser.setSearchQuery("TARGET");
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 5);
    TEST_CHECK(browser.model().selection().selected.empty());
    std::unordered_set<std::string> targets;
    for (const auto& entry : browser.model().snapshot().entries) {
      targets.insert(entry.location().uri());
      TEST_CHECK(!entry.parentDisplayName().empty() && entry.id != oldId);
      if (entry.type == luciole::FileType::Regular && !entry.symlink) {
        TEST_CHECK(entry.contentType == "text/plain");
      }
    }
    TEST_CHECK(
        targets.contains(Location::fromPath((fixture.path / "child/deep/target.txt").native()).uri())
        && !targets.contains(Location::fromPath((outside.path / "outside-target.txt").native()).uri())
    );
    browser.setSearchQuery("STRASSE");
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().name == "Straße.txt");
    browser.setSearchQuery("*");
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().name == "*.txt");
    browser.setSearchQuery("obsolete");
    browser.setSearchQuery("target");
    browser.setSearchQuery("STRASSE");
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 1 && browser.searchQuery() == "STRASSE");
    browser.setSearchQuery("target");
    browser.stop();
    TEST_CHECK(browser.model().snapshot().status == DirectoryStatus::Cancelled);
    browser.refresh();
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 5 && browser.searchQuery() == "target");
    browser.toggleHidden();
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 6);
    TEST_CHECK(!browser.openText("sftp://invalid%zz") && browser.searchQuery() == "target");
    browser.setSearchQuery({});
    TEST_CHECK(
        browser.model().snapshot().location == root
        && browser.model().selection().selected.empty()
        && id(browser, "target.txt") == oldId
    );
    browser.setSearchQuery("target");
    pump.ready(browser);
    browser.open(root);
    pump.ready(browser);
    TEST_CHECK(browser.searchQuery().empty() && !browser.canBack());
    browser.setSearchQuery("target");
    pump.ready(browser);
    const auto directory = id(browser, "target-folder");
    browser.activate(directory);
    pump.ready(browser);
    TEST_CHECK(browser.searchQuery().empty() && browser.model().snapshot().entries.empty());
    browser.setSearchQuery("missing");
    TEST_CHECK(browser.back());
    pump.ready(browser);
    TEST_CHECK(browser.searchQuery() == "target" && browser.model().snapshot().location == root);

    for (int i = 0; i < 180; ++i) {
      std::ofstream(fixture.path / ("batch-match-" + std::to_string(i))) << "batch";
    }
    bool progressive = false;
    bool selectable = false;
    browser.setChangedCallback([&] {
      if (!progressive
          && browser.model().snapshot().status == DirectoryStatus::Loading
          && !browser.model().rows().empty()) {
        progressive = true;
        selectable = browser.model().select(browser.model().rows().front());
      }
    });
    browser.setSearchQuery("batch-match");
    pump.ready(browser);
    browser.setChangedCallback({});
    TEST_CHECK(
        progressive
        && selectable
        && browser.model().rows().size() == 180
        && browser.model().selection().selected.size() == 1
    );
    browser.setSearchQuery("target");
    browser.open(Location::fromPath(outside.path.native()));
    pump.ready(browser);
    TEST_CHECK(
        browser.searchQuery().empty()
        && browser.model().snapshot().location == Location::fromPath(outside.path.native())
    );
    browser.setSearchQuery("target");
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 1);
    TEST_CHECK(browser.openText((fixture.path / "missing").native()));
    pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::Error; });
    browser.setSearchQuery("target");
    pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(
        browser.model().snapshot().error && browser.model().snapshot().error->kind == luciole::FileErrorKind::Missing
    );
  }

  void testSearchLifetimeAndPartialFailure() {
    Fixture fixture;
    Pump pump;
    auto immediate = std::make_unique<luciole::RecursiveSearch>();
    immediate->setChangedCallback([&] { immediate.reset(); });
    immediate->start(Location::fromPath(fixture.path.native()), "file", false);
    TEST_CHECK(!immediate);
    if (::geteuid() != 0) {
      const auto denied = fixture.path / "denied";
      fs::create_directory(denied);
      std::ofstream(denied / "inside-target") << "inaccessible";
      fs::permissions(denied, fs::perms::none);
      BrowserController browser({}, false);
      browser.open(Location::fromPath(fixture.path.native()));
      pump.ready(browser);
      browser.setSearchQuery("file");
      pump.ready(browser);
      fs::permissions(denied, fs::perms::owner_all);
      TEST_CHECK(browser.model().rows().size() == 1 && browser.skippedSearchFolders() == 1);
    }

    auto search = std::make_unique<luciole::RecursiveSearch>();
    bool destroyed = false;
    search->setChangedCallback([&] {
      if (!search->snapshot().entries.empty()) {
        search.reset();
        destroyed = true;
      }
    });
    search->start(Location::fromPath(fixture.path.native()), "file", false);
    pump.until([&] { return destroyed; });
    // Drain cancelled callbacks while a new independent search runs.
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    browser.setSearchQuery("inside");
    pump.ready(browser);
    TEST_CHECK(!browser.model().rows().empty());
  }

  void testDemandMetadataAndBulkFallback() {
    Fixture fixture;
    for (int i = 0; i < 1200; ++i) {
      std::ofstream(fixture.path / ("item" + std::to_string(i) + ".txt"));
    }
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    const auto child = Location::fromPath((fixture.path / "child").native());
    browser.open(root);
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().entries.isCatalogue());
    const auto find = [&](const std::string& name) {
      const auto& entries = browser.model().snapshot().entries;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries.brief(i).name == name) {
          return entries.brief(i).id;
        }
      }
      throw std::runtime_error("missing catalogue browser fixture entry");
    };
    const auto first = find("item1100.txt");
    TEST_CHECK(browser.model().select(first));
    TEST_CHECK(browser.model().selectionSummary().pendingSizes == 1);
    bool resolved = false;
    TEST_CHECK(browser.whenMetadataReady(
        {first},
        [&] {
          const auto* value = browser.model().entry(first);
          TEST_CHECK(value && !value->metadataPending && value->identity() && value->details.mode);
          TEST_CHECK(value->queriedDetails == luciole::EntryDetails::All);
          resolved = true;
        },
        luciole::EntryDetails::All
    ));
    pump.until([&] { return resolved; });
    TEST_CHECK(browser.model().selectionSummary().pendingSizes == 0);
    const auto second = find("item1150.txt");
    browser.model().select(second);
    browser.open(child);
    pump.until([&] {
      return browser.model().snapshot().location == child
          && browser.model().snapshot().status == DirectoryStatus::Ready;
    });
    TEST_CHECK(browser.back());
    pump.until([&] {
      return browser.model().snapshot().location == root && browser.model().selectedIds().size() == 1;
    });
    TEST_CHECK(browser.model().entry(browser.model().selectedIds().front())->name == "item1150.txt");
    TEST_CHECK(browser.model().entry(browser.model().selectedIds().front())->identity());
    const auto third = find("item1190.txt");
    browser.model().select(third);
    BrowserController duplicate({}, false);
    duplicate.duplicateFrom(browser);
    pump.until([&] {
      return duplicate.model().snapshot().location == root && duplicate.model().selectedIds().size() == 1;
    });
    TEST_CHECK(duplicate.model().entry(duplicate.model().selectedIds().front())->name == "item1190.txt");
    // Cloning owns its captured references; changing view can immediately destroy the source controller.
    auto transient = std::make_unique<BrowserController>(luciole::SortOptions{}, false);
    transient->open(root);
    pump.ready(*transient);
    EntryId captured = 0;
    for (std::size_t i = 0; i < transient->model().snapshot().entries.size(); ++i) {
      const auto brief = transient->model().snapshot().entries.brief(i);
      if (brief.name == "item1180.txt") {
        captured = brief.id;
        break;
      }
    }
    TEST_CHECK(captured && transient->model().select(captured));
    duplicate.duplicateFrom(*transient);
    transient.reset();
    pump.until([&] {
      const auto ids = duplicate.model().selectedIds();
      return ids.size() == 1 && duplicate.model().entry(ids.front())->name == "item1180.txt";
    });
    // Independent consumers waiting on the same row must both complete.
    int replies = 0;
    const auto fourth = find("item1170.txt");
    TEST_CHECK(browser.whenMetadataReady({fourth}, [&] { ++replies; }));
    TEST_CHECK(browser.whenMetadataReady({fourth}, [&] { ++replies; }, luciole::EntryDetails::All));
    pump.until([&] { return replies == 2; });
    browser.model().setSort({.field = luciole::SortField::Size});
    pump.until([&] {
      return browser.model().snapshot().status == DirectoryStatus::Ready
          && !browser.model().snapshot().entries.isCatalogue();
    });
    for (const auto& value : browser.model().snapshot().entries) {
      TEST_CHECK(!value.metadataPending && value.size);
    }
    browser.model().setSort({});
    pump.until([&] {
      return browser.model().snapshot().status == DirectoryStatus::Ready
          && browser.model().snapshot().entries.isCatalogue();
    });
    browser.model().selectAll();
    pump.until([&] {
      return browser.model().snapshot().status == DirectoryStatus::Ready
          && !browser.model().snapshot().entries.isCatalogue();
    });
    TEST_CHECK(browser.model().selectedIds().size() == browser.model().rows().size());
    TEST_CHECK(
        browser.model().selectionSummary().pendingSizes == 0 && browser.model().selectionSummary().unknownSizes == 0
    );
    browser.model().clearSelection();
    pump.until([&] {
      return browser.model().snapshot().status == DirectoryStatus::Ready
          && browser.model().snapshot().entries.isCatalogue();
    });
    bool cancelledReply = false;
    TEST_CHECK(browser.whenMetadataReady({find("item1111.txt")}, [&] { cancelledReply = true; }));
    browser.stop();
    pump.drainFor(100ms);
    TEST_CHECK(!cancelledReply && browser.model().selection().selected.empty());
  }

  void testHistoryState() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    luciole::BrowseViewport viewport{
        .mode = luciole::ViewMode::List, .iconSize = 96, .vertical = 123.5F, .horizontal = 42.0F, .withinRow = 9.5F
    };
    browser.setViewDefaults(luciole::ViewMode::Grid, 48);
    browser.setCaptureViewport([&] { return viewport; });
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    (void)browser.takeRestoredViewport();
    const auto selected = id(browser, "file");
    const auto focus = id(browser, "child");
    const auto anchor = id(browser, "other");
    browser.model().restoreInteraction({{selected}, focus, anchor}, selected);
    viewport.first = selected;
    browser.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(browser);
    TEST_CHECK(browser.viewport().mode == luciole::ViewMode::Grid && browser.viewport().iconSize == 48);
    TEST_CHECK(browser.back());
    pump.ready(browser);
    TEST_CHECK(browser.model().selectedIds() == std::vector{id(browser, "file")} && id(browser, "file") != selected);
    TEST_CHECK(
        browser.model().selection().focused == id(browser, "child")
        && browser.model().selection().anchor == id(browser, "other")
    );
    auto restored = browser.takeRestoredViewport();
    TEST_CHECK(
        restored
        && restored->mode == luciole::ViewMode::List
        && restored->iconSize == 96
        && restored->first == id(browser, "file")
        && restored->withinRow == 9.5F
        && restored->horizontal == 42.0F
        && restored->vertical == 123.5F
    );
    TEST_CHECK(!browser.takeRestoredViewport());
    browser.model().setSort({.field = luciole::SortField::Size, .descending = true, .directoriesFirst = false});
    browser.toggleHidden();
    browser.forward();
    pump.ready(browser);
    // Preserve the old object under another name so inode reuse cannot weaken the replacement check.
    fs::rename(fixture.path / "file", fixture.path / "old-file");
    std::ofstream(fixture.path / "file") << "replacement";
    browser.back();
    pump.ready(browser);
    TEST_CHECK(browser.model().selectedIds().empty());
    TEST_CHECK(browser.model().sort().field == luciole::SortField::Size && browser.model().filter().showHidden);
    browser.setSearchQuery("inside");
    pump.ready(browser);
    browser.model().select(id(browser, "inside"));
    viewport.first = id(browser, "inside");
    browser.open(Location::fromPath((fixture.path / "other").native()));
    pump.ready(browser);
    browser.back();
    pump.ready(browser);
    TEST_CHECK(
        browser.searchQuery() == "inside" && browser.model().selectedIds() == std::vector{id(browser, "inside")}
    );
    browser.forward();
    browser.back();
    browser.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(browser);
    TEST_CHECK(browser.searchQuery().empty() && browser.model().selectedIds().empty() && !browser.canForward());
  }

  void testCurrentFolderFilter() {
    Fixture fixture;
    const auto root = Location::fromPath(fixture.path.native());
    fs::create_directory(fixture.path / "Templates");
    std::ofstream(fixture.path / "Template1.csv") << "first";
    std::ofstream(fixture.path / "Template2.csv") << "second";
    std::ofstream(fixture.path / "Templates" / "nested-temp.csv") << "nested";
    std::ofstream(fixture.path / "child" / "nested-temp.csv") << "nested";
    std::ofstream(fixture.path / "Straße.txt") << "unicode";
    std::ofstream(fixture.path / "*.txt") << "literal";
    Pump pump;
    BrowserController browser({}, false);
    browser.setSearchRecursive(false);
    browser.open(root);
    pump.ready(browser);
    const auto first = id(browser, "Template1.csv");
    const auto listingSize = browser.model().rows().size();
    const auto generation = browser.model().snapshot().generation;
    const auto context = browser.contextSerial();
    browser.editSearchQuery("temp");
    TEST_CHECK(
        browser.model().rows().size() == 3
        && !browser.searchPending()
        && browser.model().snapshot().status == DirectoryStatus::Ready
        && browser.model().snapshot().generation == generation
        && browser.contextSerial() != context
        && id(browser, "Template1.csv") == first
    );
    for (const auto row : browser.model().rows()) {
      TEST_CHECK(browser.model().entry(row)->location().parent() == root);
    }
    browser.model().select(first);
    browser.editSearchQuery("STRASSE");
    TEST_CHECK(
        browser.model().rows() == std::vector{id(browser, "Straße.txt")} && browser.model().selection().selected.empty()
    );
    browser.editSearchQuery("*");
    TEST_CHECK(browser.model().rows() == std::vector{id(browser, "*.txt")});
    browser.editSearchQuery({});
    TEST_CHECK(browser.model().rows().size() == listingSize);
    browser.editSearchQuery("temp");
    BrowserController duplicate({}, false);
    duplicate.duplicateFrom(browser);
    pump.ready(duplicate);
    TEST_CHECK(
        !duplicate.searchRecursive() && duplicate.searchQuery() == "temp" && duplicate.model().rows().size() == 3
    );
    browser.activate(id(browser, "Templates"));
    pump.ready(browser);
    TEST_CHECK(browser.searchQuery().empty() && browser.model().rows().size() == 1);
    TEST_CHECK(browser.back());
    pump.ready(browser);
    TEST_CHECK(browser.searchQuery() == "temp" && browser.model().rows().size() == 3);
    browser.setSearchRecursive(true);
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 5 && browser.model().filter().text.empty());
    browser.editSearchQuery("pending");
    TEST_CHECK(browser.searchPending());
    browser.setSearchRecursive(false);
    TEST_CHECK(!browser.searchPending() && browser.model().rows().empty());
    browser.editSearchQuery("temp");
    TEST_CHECK(browser.model().rows().size() == 3);
    pump.drainFor(220ms);
    TEST_CHECK(browser.model().rows().size() == 3 && !browser.searchPending());
    std::ofstream(fixture.path / "Template3.csv") << "live";
    pump.until([&] { return browser.model().rows().size() == 4; });
    fs::rename(fixture.path / "Template3.csv", fixture.path / "renamed.csv");
    pump.until([&] { return browser.model().rows().size() == 3; });
    std::ofstream(fixture.path / ".temp-hidden") << "hidden";
    std::ofstream(fixture.path / "temp-backup~") << "backup";
    pump.until([&] { return browser.model().rows().size() == 4; });
    browser.setShowBackups(false);
    TEST_CHECK(browser.model().rows().size() == 3);
    browser.toggleHidden();
    TEST_CHECK(browser.model().rows().size() == 4);
    browser.refresh();
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 4 && browser.searchQuery() == "temp");
    browser.editSearchQuery({});
    browser.setExpandableFolders(true);
    TEST_CHECK(browser.toggleFolder(id(browser, "child")));
    pump.until([&] {
      return std::ranges::any_of(browser.model().rows(), [&](auto row) {
        return browser.model().entry(row)->name == "nested-temp.csv";
      });
    });
    browser.editSearchQuery("temp");
    TEST_CHECK(browser.model().rows().size() == 4);
    for (const auto row : browser.model().rows()) {
      TEST_CHECK(browser.model().entry(row)->location().parent() == root);
    }
    browser.editSearchQuery({});
    TEST_CHECK(browser.toggleFolder(id(browser, "child")));
    pump.until([&] {
      return std::ranges::any_of(browser.model().rows(), [&](auto row) {
        return browser.model().entry(row)->name == "nested-temp.csv";
      });
    });
  }

  void testDebouncedSearch() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    const auto child = Location::fromPath((fixture.path / "child").native());
    browser.open(root);
    pump.ready(browser);
    browser.model().select(id(browser, "file"));
    browser.editSearchQuery("file");
    TEST_CHECK(
        browser.searchPending()
        && browser.searchQuery() == "file"
        && browser.model().snapshot().status == DirectoryStatus::Loading
        && browser.model().rows().empty()
        && !browser.model().selectionAvailable()
        && browser.model().selectedIds().empty()
    );
    browser.editSearchQuery("ins");
    pump.drainFor(80ms);
    browser.editSearchQuery("inside");
    const auto edited = std::chrono::steady_clock::now();
    pump.ready(browser);
    TEST_CHECK(std::chrono::steady_clock::now() - edited >= 160ms);
    TEST_CHECK(
        !browser.searchPending()
        && browser.model().rows().size() == 1
        && browser.model().snapshot().entries.front().name == "inside"
    );
    browser.editSearchQuery("file");
    browser.submitSearchQuery();
    TEST_CHECK(!browser.searchPending());
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 1 && id(browser, "file"));
    browser.editSearchQuery("inside");
    browser.editSearchQuery({});
    TEST_CHECK(
        !browser.searchPending()
        && browser.searchQuery().empty()
        && browser.model().snapshot().status == DirectoryStatus::Ready
        && browser.model().selectionAvailable()
        && browser.model().rows().size() == 4
    );
    pump.drainFor(230ms);
    TEST_CHECK(browser.searchQuery().empty() && browser.model().rows().size() == 4);
    browser.editSearchQuery("inside");
    browser.stop();
    pump.drainFor(230ms);
    TEST_CHECK(
        !browser.searchPending()
        && browser.model().snapshot().status == DirectoryStatus::Cancelled
        && browser.model().rows().empty()
    );
    browser.refresh();
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 1 && id(browser, "inside"));
    browser.editSearchQuery("hidden");
    browser.toggleHidden();
    pump.ready(browser);
    TEST_CHECK(!browser.searchPending() && id(browser, ".hidden"));
    browser.editSearchQuery("inside");
    browser.open(child);
    pump.ready(browser);
    pump.drainFor(230ms);
    TEST_CHECK(
        !browser.searchPending()
        && browser.searchQuery().empty()
        && browser.model().snapshot().location == child
        && browser.model().rows().size() == 1
    );
    browser.back();
    pump.ready(browser);
    TEST_CHECK(browser.searchQuery() == "inside" && id(browser, "inside"));
    browser.editSearchQuery("file");
    browser.forward();
    pump.ready(browser);
    pump.drainFor(230ms);
    TEST_CHECK(browser.searchQuery().empty() && browser.model().snapshot().location == child);
    browser.editSearchQuery("inside");
    browser.open(child);
    pump.ready(browser);
    pump.drainFor(230ms);
    TEST_CHECK(!browser.searchPending() && browser.searchQuery().empty());
  }

  void testSearchCompositionAndTimeoutLifetime() {
    Fixture fixture;
    Pump pump;
    const auto root = Location::fromPath(fixture.path.native());
    auto browser = std::make_unique<BrowserController>(luciole::SortOptions{}, false);
    browser->open(root);
    pump.ready(*browser);
    browser->editSearchQuery("in");
    browser->setSearchComposing(true);
    browser->submitSearchQuery();
    pump.drainFor(230ms);
    TEST_CHECK(browser->searchPending() && browser->model().rows().empty());
    browser->editSearchQuery("inside");
    pump.drainFor(230ms);
    TEST_CHECK(browser->searchPending());
    browser->setSearchComposing(false);
    const auto committed = std::chrono::steady_clock::now();
    pump.ready(*browser);
    TEST_CHECK(std::chrono::steady_clock::now() - committed >= 160ms && id(*browser, "inside"));
    browser->editSearchQuery("file");
    browser->setSearchComposing(true);
    browser->editSearchQuery({});
    browser->setSearchComposing(false);
    pump.drainFor(230ms);
    TEST_CHECK(!browser->searchPending() && browser->model().rows().size() == 4);
    browser->editSearchQuery("inside");
    browser.reset();
    pump.drainFor(230ms);
    browser = std::make_unique<BrowserController>(luciole::SortOptions{}, false);
    browser->open(root);
    pump.ready(*browser);
    browser->editSearchQuery("inside");
    browser->setChangedCallback([&] {
      if (!browser->searchPending()) {
        browser.reset();
      }
    });
    pump.until([&] { return !browser; });
    pump.drainFor(230ms);
  }

  void testLocationPresentation() {
    const fs::path homePath(g_get_home_dir());
    const auto home = Location::fromPath(homePath.native());
    const auto homeCrumbs = luciole::locationBreadcrumbs(home);
    TEST_CHECK(homeCrumbs.size() == 1 && homeCrumbs.front().label == "Home" && homeCrumbs.front().location == home);
    const auto inside = Location::fromPath((homePath / "space here" / "<literal> café").native());
    const auto insideCrumbs = luciole::locationBreadcrumbs(inside);
    TEST_CHECK(
        insideCrumbs.size() == 3
        && insideCrumbs.front().location == home
        && insideCrumbs.front().label == "Home"
        && insideCrumbs[1].label == "space here"
        && insideCrumbs.back().location == inside
        && insideCrumbs.back().label == "<literal> café"
    );
    TEST_CHECK(luciole::locationEditText(inside) == (homePath / "space here" / "<literal> café").native());
    const auto homeRaw = Location::fromPath((homePath / ("raw-" + std::string(1, static_cast<char>(0xff)))).native());
    const auto rawCrumbs = luciole::locationBreadcrumbs(homeRaw);
    TEST_CHECK(rawCrumbs.size() == 2 && rawCrumbs.front().location == home && rawCrumbs.back().location == homeRaw);
    TEST_CHECK(luciole::locationEditText(homeRaw) == homeRaw.uri());
    if (homePath != "/") {
      const auto sibling =
          Location::fromPath((homePath.parent_path() / (homePath.filename().native() + "-other")).native());
      const auto siblingCrumbs = luciole::locationBreadcrumbs(sibling);
      TEST_CHECK(siblingCrumbs.front().label == "File System" && siblingCrumbs.back().location == sibling);
    }
    const auto path = Location::fromPath("/tmp/space here/<literal>/café");
    const auto crumbs = luciole::locationBreadcrumbs(path);
    TEST_CHECK(
        crumbs.size() == 5
        && crumbs.front().location == Location::fromPath("/")
        && crumbs.back().location == path
        && crumbs[3].label == "<literal>"
    );
    TEST_CHECK(luciole::locationEditText(path) == "/tmp/space here/<literal>/café");
    const auto raw = Location::fromPath("/tmp/raw-" + std::string(1, static_cast<char>(0xff)));
    TEST_CHECK(luciole::locationEditText(raw) == raw.uri() && luciole::locationBreadcrumbs(raw).back().location == raw);
    const auto remote = Location::fromUri("sftp://user:secret@example.org/space%20here/leaf");
    const auto remoteCrumbs = luciole::locationBreadcrumbs(remote);
    TEST_CHECK(
        remoteCrumbs.back().location == remote
        && remoteCrumbs.front().label == "example.org"
        && luciole::locationEditText(remote) == remote.uri()
    );
    TEST_CHECK(luciole::locationTitle(Location::fromPath("/")) == "File System");
    TEST_CHECK(luciole::locationTitle(path) == "café" && luciole::locationTitle(Location::network()) == "Network");
    TEST_CHECK(luciole::locationTitle(Location::fromUri("sftp://user:secret@example.org/")) == "example.org");
    TEST_CHECK(luciole::locationTitle(remote) == "leaf");
    if (homePath != "/") {
      TEST_CHECK(luciole::locationTitle(home) == homePath.filename().native());
    }
  }

  void testFolderSizes() {
    Fixture fixture;
    fs::create_directory(fixture.path / "empty");
    fs::create_directory(fixture.path / "sized");
    std::ofstream(fixture.path / "sized" / "one") << "abc";
    std::ofstream(fixture.path / "sized" / "two") << "abcde";
    Pump pump;
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto emptyId = id(browser, "empty");
    const auto sizedId = id(browser, "sized");
    const auto fileId = id(browser, "file");
    const auto linkId = id(browser, "folder-link");
    const auto childId = id(browser, "child");
    int changes = 0;
    browser.setChangedCallback([&] { ++changes; });
    // Two walks at once; the third waits for a slot.
    browser.requestFolderSize(emptyId);
    browser.requestFolderSize(sizedId);
    browser.requestFolderSize(childId);
    pump.until([&] {
      return browser.folderSize(emptyId) != nullptr
          && browser.folderSize(sizedId) != nullptr
          && browser.folderSize(childId) != nullptr;
    });
    pump.drainFor(150ms);
    TEST_CHECK(
        browser.folderSize(emptyId)->bytes == 0
        && browser.folderSize(sizedId)->bytes == 8
        && browser.folderSize(sizedId)->files == 2
        && browser.folderSize(childId)->bytes == 13
    );
    TEST_CHECK(changes > 0);
    TEST_CHECK(luciole::entryFolderSizeText(*browser.folderSize(emptyId)) == "0 bytes");
    // Ineligible: a file and a folder symlink.
    browser.requestFolderSize(fileId);
    browser.requestFolderSize(linkId);
    pump.drainFor(150ms);
    TEST_CHECK(!browser.folderSize(fileId) && !browser.folderSize(linkId));
    // Cached ids never re-walk.
    browser.requestFolderSize(childId);
    pump.drainFor(150ms);
    TEST_CHECK(browser.folderSize(sizedId) != nullptr);
    // Changed facts drop the cache; the next bind walks again.
    std::ofstream(fixture.path / "sized" / "three") << "xy";
    browser.refresh();
    pump.ready(browser);
    TEST_CHECK(!browser.folderSize(sizedId));
    browser.requestFolderSize(sizedId);
    pump.until([&] { return browser.folderSize(sizedId) != nullptr; });
    TEST_CHECK(browser.folderSize(sizedId)->bytes == 10);
    // Navigation retires ids and drops their sizes.
    browser.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(browser);
    TEST_CHECK(!browser.folderSize(sizedId));
  }

  void testFolderSizeNetworkRefusal() {
    Fixture fixture;
    fs::create_directory(fixture.path / "remote-ish");
    Pump pump;
    int probeCalls = 0;
    BrowserController browser({}, false, true, [&](const std::string&) {
      ++probeCalls;
      return true;
    });
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto refusalId = id(browser, "remote-ish");
    browser.requestFolderSize(refusalId);
    pump.until([&] { return browser.folderSize(refusalId) != nullptr; });
    TEST_CHECK(browser.folderSize(refusalId)->phase == luciole::FolderTotalsPhase::Unsupported);
    TEST_CHECK(luciole::entryFolderSizeText(*browser.folderSize(refusalId)).empty());
    TEST_CHECK(probeCalls == 1);
    const int settle = probeCalls;
    browser.requestFolderSize(refusalId);
    pump.drainFor(150ms);
    TEST_CHECK(probeCalls == settle && browser.folderSize(refusalId) != nullptr);
  }

} // namespace

int main() {
  try {
    testActionContextsAndFileLinks();
    testBrokenAndChainedLinks();
    testProviderShortcutNavigation();
    testDuplicatedTabs();
    testNavigation();
    testFailureAndLiveUpdates();
    testRecursiveSearch();
    testBackupPolicyAndSearch();
    testSearchLifetimeAndPartialFailure();
    testFolderSizes();
    testFolderSizeNetworkRefusal();
    testLocationPresentation();
    testHistoryState();
    testDemandMetadataAndBulkFallback();
    testCurrentFolderFilter();
    testDebouncedSearch();
    testSearchCompositionAndTimeoutLifetime();
    std::println("browser controller checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "browser controller test: {}", error.what());
    return 1;
  }
}
