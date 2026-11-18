#include "browser/browser_controller.h"
#include "browser/folder_views.h"
#include "config/folder_view_store.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <print>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  using luciole::BrowserController;
  using luciole::DirectoryStatus;
  using luciole::FolderView;
  using luciole::FolderViewEntry;
  using luciole::FolderViewService;
  using luciole::FolderViewStore;
  using luciole::Location;
  using luciole::SortField;
  using luciole::SortOptions;
  using luciole::ViewMode;

  struct Fixture {
    fs::path path;
    Fixture() {
      const luciole::GString name(g_dir_make_tmp("luciole-folder-views-test-XXXXXX", nullptr));
      TEST_CHECK(name != nullptr);
      path = name.get();
      fs::create_directory(path / "downloads");
      fs::create_directory(path / "documents");
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path state() const { return path / "state" / "folder_views.toml"; }
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

  private:
    GlibPollSource m_source;
  };

  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void testStoreRoundtrip() {
    Fixture fixture;
    FolderViewStore store(fixture.state());
    store.load();
    TEST_CHECK(store.entries().empty() && store.issues().empty());
    const SortOptions created{.field = SortField::Created, .descending = true};
    const std::vector<FolderViewEntry> entries{
        {.uri = "file:///home/user/Downloads", .view = {.sort = created, .mode = std::nullopt}},
        {.uri = "file:///home/user/Pictures", .view = {.sort = std::nullopt, .mode = ViewMode::Grid}},
        {.uri = "file:///home/user/Empty", .view = {}},
    };
    TEST_CHECK(store.save(entries) != luciole::AtomicWriteState::NotCommitted);
    TEST_CHECK(store.entries().size() == 2);
    const auto text = read(fixture.state());
    TEST_CHECK(text.contains("sort_by = \"created\"") && text.contains("view_mode = \"grid\""));
    TEST_CHECK(!text.contains("Empty"));
    FolderViewStore reloaded(fixture.state());
    reloaded.load();
    TEST_CHECK(reloaded.issues().empty());
    TEST_CHECK(reloaded.entries() == std::vector<FolderViewEntry>(entries.begin(), entries.begin() + 2));
  }

  void testStoreSkipsInvalidEntries() {
    Fixture fixture;
    fs::create_directories(fixture.state().parent_path());
    std::ofstream(fixture.state()) << "schema_version = 1\n"
                                      "[[folders]]\nuri = \"file:///a\"\nview_mode = \"miller\"\n"
                                      "[[folders]]\nuri = \"file:///b\"\nsort_by = \"bogus\"\n"
                                      "[[folders]]\nuri = \"file:///c\"\nsort_descending = true\n"
                                      "[[folders]]\nuri = \"file:///d\"\nsort_by = \"size\"\n"
                                      "[[folders]]\nuri = \"file:///d\"\nview_mode = \"list\"\n";
    FolderViewStore store(fixture.state());
    store.load();
    TEST_CHECK(store.issues().size() == 3);
    // The later entry for a folder replaces the earlier one.
    TEST_CHECK(store.entries().size() == 1);
    TEST_CHECK(store.entries().front().uri == "file:///d");
    TEST_CHECK(store.entries().front().view == (FolderView{.sort = std::nullopt, .mode = ViewMode::List}));

    std::ofstream(fixture.state(), std::ios::trunc) << "not toml [";
    store.load();
    TEST_CHECK(!store.issues().empty() && store.entries().empty());
  }

  void testControllerFollowsFolderViews() {
    Fixture fixture;
    Pump pump;
    const auto downloads = Location::fromPath(fixture.path / "downloads");
    const auto documents = Location::fromPath(fixture.path / "documents");
    const SortOptions created{.field = SortField::Created, .descending = true};
    const SortOptions defaults{.field = SortField::Name};
    {
      FolderViewService views(fixture.state(), true);
      int changes = 0;
      views.setChangedCallback([&] { ++changes; });
      pump.until([&] { return changes == 1; }); // First load.
      BrowserController browser(defaults, false);
      browser.setViewDefaults(ViewMode::List, luciole::kDefaultGridIconSize);
      browser.setFolderViews(&views);
      browser.open(downloads);
      pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::Ready; });
      TEST_CHECK(browser.folderView() == FolderView{});

      views.setSort(downloads, created);
      views.setMode(downloads, ViewMode::Grid);
      browser.reapplyFolderView();
      TEST_CHECK(browser.model().sort() == created && browser.viewport().mode == ViewMode::Grid);

      browser.open(documents);
      TEST_CHECK(browser.model().sort() == defaults && browser.viewport().mode == ViewMode::List);
      TEST_CHECK(browser.back());
      TEST_CHECK(browser.model().sort() == created && browser.viewport().mode == ViewMode::Grid);

      // A new default sort leaves the folder's own alone.
      browser.setDefaultSort({.field = SortField::Size});
      TEST_CHECK(browser.model().sort() == created);

      views.setEnabled(false);
      browser.reapplyFolderView();
      TEST_CHECK(!browser.folderView());
      TEST_CHECK(browser.model().sort().field == SortField::Size && browser.viewport().mode == ViewMode::List);

      views.setEnabled(true);
      views.reset(documents);
      pump.until([&] { return !views.busy(); });
    }
    // Saved settings survive into a new service.
    FolderViewService reloaded(fixture.state(), true);
    bool loaded = false;
    reloaded.setChangedCallback([&] { loaded = true; });
    pump.until([&] { return loaded; });
    TEST_CHECK(reloaded.error().empty());
    TEST_CHECK(reloaded.lookup(downloads) == (FolderView{.sort = created, .mode = ViewMode::Grid}));
    TEST_CHECK(reloaded.lookup(documents) == FolderView{});
    reloaded.reset(downloads);
    TEST_CHECK(reloaded.lookup(downloads) == FolderView{});
    pump.until([&] { return !reloaded.busy(); });
    TEST_CHECK(!read(fixture.state()).contains("downloads"));
  }
} // namespace

int main() {
  try {
    testStoreRoundtrip();
    testStoreSkipsInvalidEntries();
    testControllerFollowsFolderViews();
    std::println("folder view checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "folder views test: {}", error.what());
    return 1;
  }
}
