#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "fs/trash_info.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 5) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    void ready(const luciole::BrowserController& browser) {
      until([&] { return browser.model().snapshot().status != luciole::DirectoryStatus::Loading; });
      const auto& snapshot = browser.model().snapshot();
      TEST_CHECK(snapshot.status == luciole::DirectoryStatus::Ready);
    }

  private:
    GlibPollSource m_source;
  };
  struct Fixture {
    fs::path root;
    Fixture() {
      const luciole::GString path(g_dir_make_tmp("luciole-trash-browse-XXXXXX", nullptr));
      TEST_CHECK(path != nullptr);
      root = path.get();
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  void metadataAndPresentation() {
    luciole::GioPtr<GFileInfo> info(g_file_info_new());
    TEST_CHECK(!luciole::readTrashInfo(info.get()).original && !luciole::readTrashInfo(info.get()).deletionDate);
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, "/wrong-type");
    g_file_info_set_attribute_byte_string(info.get(), G_FILE_ATTRIBUTE_TRASH_DELETION_DATE, "wrong-type");
    TEST_CHECK(!luciole::readTrashInfo(info.get()).original && !luciole::readTrashInfo(info.get()).deletionDate);
    g_file_info_set_attribute_byte_string(info.get(), G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, "relative/name");
    TEST_CHECK(!luciole::readTrashInfo(info.get()).original);
    const auto raw = std::string("/tmp/文 #/raw-") + static_cast<char>(0xff);
    g_file_info_set_attribute_byte_string(info.get(), G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, raw.c_str());
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_TRASH_DELETION_DATE, "2026-10-29T12:34:56");
    const auto value = luciole::readTrashInfo(info.get());
    TEST_CHECK(value.original && value.original->nativePath() == raw && value.deletionDate == "2026-10-29T12:34:56");
    TEST_CHECK(luciole::trashOriginalParentText(value) == "/tmp/文 #");
    const auto root = luciole::Location::trash();
    TEST_CHECK(root.isTrash() && !root.nativePath());
    const auto nested = luciole::Location::fromUri("trash:///a%20b/c%23.txt");
    const auto crumbs = luciole::locationBreadcrumbs(nested);
    TEST_CHECK(
        crumbs.size() == 3
        && crumbs.front().label == "Trash"
        && crumbs.front().location == root
        && crumbs[1].label == "a b"
        && crumbs.back().label == "c#.txt"
        && crumbs.back().location == nested
    );
    TEST_CHECK(luciole::locationEditText(nested) == nested.uri());
    TEST_CHECK(luciole::locationTitle(root) == "Trash" && luciole::locationTitle(nested) == "c#.txt");
  }
  void unavailableProvider() {
    Pump pump;
    luciole::BrowserController browser({}, false);
    browser.open(luciole::Location::trash());
    pump.until([&] { return browser.model().snapshot().status == luciole::DirectoryStatus::Error; });
    TEST_CHECK(
        browser.model().snapshot().error->kind == luciole::FileErrorKind::Unsupported && browser.model().rows().empty()
    );
    browser.refresh();
    pump.until([&] { return browser.model().snapshot().status == luciole::DirectoryStatus::Error; });
    Fixture fixture;
    browser.open(luciole::Location::fromPath(fixture.root.native()));
    pump.ready(browser);
    TEST_CHECK(browser.back());
    pump.until([&] { return browser.model().snapshot().status == luciole::DirectoryStatus::Error; });
  }
  void concurrentTrashLocations() {
    std::barrier gate(4);
    std::atomic<bool> valid{true};
    std::array<std::jthread, 4> workers;
    for (auto& worker : workers) {
      worker = std::jthread([&] {
        gate.arrive_and_wait();
        for (int iteration = 0; iteration < 10000; ++iteration) {
          const auto root = luciole::Location::trash();
          const auto nested = luciole::Location::fromUri("trash:///parallel%20folder/item%23.txt");
          const auto file = luciole::gioFile(root);
          const luciole::GString uri(g_file_get_uri(file.get()));
          if (!root.isTrash() || nested.nativePath() || !nested.isTrash() || root.uri() != uri.get()) {
            valid.store(false);
          }
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    TEST_CHECK(valid.load());
  }
  void providerBrowsing() {
    Fixture fixture;
    const auto a = fixture.root / "first";
    const auto b = fixture.root / "second";
    fs::create_directories(a / "Folder 文");
    fs::create_directory(b);
    std::ofstream(a / "same #%.txt") << "first";
    std::ofstream(b / "same #%.txt") << "second";
    std::ofstream(a / "Folder 文/inside.txt") << "child";
    std::ofstream(a / ".hidden") << "hidden";
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    std::ofstream(a / raw) << "raw";
    for (const auto& path : {a / "same #%.txt", b / "same #%.txt", a / "Folder 文", a / ".hidden", a / raw}) {
      GError* error = nullptr;
      const bool ok = g_file_trash(luciole::gioFile(luciole::Location::fromPath(path.native())).get(), nullptr, &error);
      g_clear_error(&error);
      TEST_CHECK(ok);
    }
    Pump pump;
    luciole::BrowserController browser({}, false);
    bool launched = false;
    browser.setFileActivatedCallback([&](const auto&) { launched = true; });
    browser.open(luciole::Location::trash());
    pump.ready(browser);
    const auto& entries = browser.model().snapshot().entries;
    TEST_CHECK(entries.size() == 5);
    TEST_CHECK(std::ranges::count(entries, "same #%.txt", &luciole::DirectoryEntry::displayName) == 2);
    const auto first = std::ranges::find_if(entries, [&](const auto& entry) {
      return entry.trashInfo().original == luciole::Location::fromPath((a / "same #%.txt").native());
    });
    TEST_CHECK(
        first != entries.end()
        && first->trashInfo().deletionDate
        && first->location().isTrash()
        && first->parentDisplayName() == a.native()
    );
    const auto id = first->id;
    const auto operation = first->location();
    browser.model().select(id);
    browser.activate(id);
    TEST_CHECK(!launched && browser.model().snapshot().location == luciole::Location::trash());
    browser.refresh();
    pump.ready(browser);
    TEST_CHECK(
        browser.model().selectedIds().empty()
        && std::ranges::any_of(browser.model().snapshot().entries, [&](const auto& entry) {
             return entry.location() == operation;
           })
    );
    browser.toggleHidden();
    TEST_CHECK(browser.model().rows().size() == 5);
    const auto folder =
        std::ranges::find(browser.model().snapshot().entries, "Folder 文", &luciole::DirectoryEntry::displayName);
    TEST_CHECK(folder != browser.model().snapshot().entries.end());
    browser.activate(folder->id);
    pump.ready(browser);
    TEST_CHECK(
        browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().displayName() == "inside.txt"
    );
    TEST_CHECK(browser.up());
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == luciole::Location::trash() && !browser.canUp());
    browser.setSearchQuery("inside");
    pump.ready(browser);
    TEST_CHECK(browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().location().isTrash());
    browser.activate(browser.model().rows().front());
    TEST_CHECK(!launched);
    browser.setSearchQuery({});
    browser.open(luciole::Location::fromPath(fixture.root.native()));
    pump.ready(browser);
    TEST_CHECK(browser.back());
    pump.ready(browser);
    TEST_CHECK(browser.model().snapshot().location == luciole::Location::trash());
  }
} // namespace

int main(int argc, char** argv) {
  try {
    metadataAndPresentation();
    if (argc == 2 && std::string_view(argv[1]) == "--uri-stress") {
      concurrentTrashLocations();
    } else if (argc == 2 && std::string_view(argv[1]) == "--provider") {
      providerBrowsing();
    } else {
      TEST_CHECK(argc == 1);
      unavailableProvider();
      concurrentTrashLocations();
    }
    std::println("trash-browse checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "trash-browse: {}", error.what());
    return 1;
  }
}
