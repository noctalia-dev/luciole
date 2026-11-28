#include "browser/browser_controller.h"
#include "browser/chooser_policy.h"
#include "event_loop/glib_poll_source.h"
#include "fs/directory_entries.h"
#include "fs/recent_files.h"
#include "test_check.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <glib.h>
#include <memory>
#include <poll.h>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using Edit = luciole::RecentEdit;
  using BookmarkFile = std::unique_ptr<GBookmarkFile, decltype(&g_bookmark_file_free)>;

  BookmarkFile parse(const std::string& text) {
    BookmarkFile list(g_bookmark_file_new(), g_bookmark_file_free);
    TEST_CHECK(g_bookmark_file_load_from_data(list.get(), text.data(), text.size(), nullptr));
    return list;
  }
  std::string mimeOf(GBookmarkFile* list, const char* uri) {
    const std::unique_ptr<gchar, decltype(&g_free)> mime(g_bookmark_file_get_mime_type(list, uri, nullptr), g_free);
    return mime ? mime.get() : "";
  }

  std::int64_t modified(GBookmarkFile* list, const char* uri) {
    GDateTime* time = g_bookmark_file_get_modified_date_time(list, uri, nullptr);
    return time ? g_date_time_to_unix(time) : -1;
  }

  void edits() {
    const char* a = "file:///tmp/a.txt";
    const char* b = "file:///tmp/b.png";
    // A missing list starts empty; a used file gets its type, Luciole as an application and the use time.
    auto text = luciole::editRecentList(std::nullopt, {{Edit::Kind::Use, a, "text/plain"}}, 1700000000);
    TEST_CHECK(text && *text);
    auto list = parse(**text);
    TEST_CHECK(g_bookmark_file_get_size(list.get()) == 1 && mimeOf(list.get(), a) == "text/plain");
    TEST_CHECK(g_bookmark_file_has_application(list.get(), a, "Luciole", nullptr));
    TEST_CHECK(modified(list.get(), a) == 1700000000);
    // Other applications' entries stay; using a file again updates it in place.
    text = luciole::editRecentList(**text, {{Edit::Kind::Use, b, ""}, {Edit::Kind::Use, a, "text/plain"}}, 1700000100);
    list = parse(**text);
    TEST_CHECK(g_bookmark_file_get_size(list.get()) == 2 && mimeOf(list.get(), b) == "application/octet-stream");
    TEST_CHECK(modified(list.get(), a) == 1700000100);
    // Forgetting removes only that file; forgetting an unknown one changes nothing.
    const auto both = **text;
    text = luciole::editRecentList(both, {{Edit::Kind::Forget, a, ""}}, 0);
    list = parse(**text);
    TEST_CHECK(g_bookmark_file_get_size(list.get()) == 1 && g_bookmark_file_has_item(list.get(), b));
    TEST_CHECK(luciole::editRecentList(both, {{Edit::Kind::Forget, "file:///tmp/none", ""}}, 0) == std::nullopt);
    // Clearing empties the list, and an empty or missing list stays as it is.
    text = luciole::editRecentList(both, {{Edit::Kind::Clear, "", ""}}, 0);
    TEST_CHECK(g_bookmark_file_get_size(parse(**text).get()) == 0);
    TEST_CHECK(luciole::editRecentList(std::nullopt, {{Edit::Kind::Clear, "", ""}}, 0) == std::nullopt);
    // A list that cannot be read is never overwritten by other edits, but clearing it is what was asked.
    const std::string broken = "not a bookmark file";
    TEST_CHECK(!luciole::editRecentList(broken, {{Edit::Kind::Use, a, "text/plain"}}, 0));
    TEST_CHECK(!luciole::editRecentList(broken, {{Edit::Kind::Forget, a, ""}}, 0));
    text = luciole::editRecentList(broken, {{Edit::Kind::Clear, "", ""}}, 0);
    TEST_CHECK(text && *text && g_bookmark_file_get_size(parse(**text).get()) == 0);
  }

  void until(const std::function<bool()>& done) {
    for (int turns = 0; !done() && turns < 100000; ++turns) {
      g_main_context_iteration(nullptr, TRUE);
    }
    TEST_CHECK(done());
  }
  std::string contents(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }

  // Edits submitted together are written in order, one batch at a time.
  void store() {
    std::string pattern = (fs::temp_directory_path() / "luciole-recent-XXXXXX").native();
    const fs::path root = ::mkdtemp(pattern.data());
    const auto file = root / "recently-used.xbel";
    {
      luciole::RecentFiles recent(file);
      const auto a = luciole::Location::fromPath((root / "a.txt").native());
      const auto b = luciole::Location::fromPath((root / "b.png").native());
      recent.recordUse(a, "text/plain");
      recent.recordUse(b, {});
      // Only local files are recorded.
      recent.recordUse(luciole::Location::fromUri("sftp://host/c.txt"), {});
      std::string forgotten = "pending";
      recent.forget({a}, [&](const std::string& error) { forgotten = error; });
      TEST_CHECK(recent.busy());
      until([&] { return !recent.busy(); });
      TEST_CHECK(forgotten.empty());
      auto list = parse(contents(file));
      TEST_CHECK(g_bookmark_file_get_size(list.get()) == 1 && g_bookmark_file_has_item(list.get(), b.uri().c_str()));
      // The type of a file chosen without one is guessed from its name.
      TEST_CHECK(mimeOf(list.get(), b.uri().c_str()) == "image/png");
      TEST_CHECK((fs::status(file).permissions() & fs::perms::group_read) == fs::perms::none);
      // A request with nothing to forget, sent while a write runs, still finishes.
      recent.recordUse(a, "text/plain");
      std::string none = "pending";
      recent.forget({}, [&](const std::string& error) { none = error; });
      until([&] { return !recent.busy(); });
      TEST_CHECK(none.empty());
      std::string cleared = "pending";
      recent.clear([&](const std::string& error) { cleared = error; });
      until([&] { return !recent.busy(); });
      TEST_CHECK(cleared.empty() && g_bookmark_file_get_size(parse(contents(file)).get()) == 0);
      // Lists other applications keep long are still edited.
      BookmarkFile large(g_bookmark_file_new(), g_bookmark_file_free);
      for (int item = 0; item < 3000; ++item) {
        const auto uri = std::format("file:///tmp/luciole-recent-fixture/{:04}.txt", item);
        g_bookmark_file_add_application(large.get(), uri.c_str(), "Other", "other %u");
        g_bookmark_file_set_mime_type(large.get(), uri.c_str(), "text/plain");
      }
      TEST_CHECK(g_bookmark_file_to_file(large.get(), file.c_str(), nullptr));
      TEST_CHECK(fs::file_size(file) > 1024 * 1024);
      std::string trimmed = "pending";
      recent.forget(
          {luciole::Location::fromUri("file:///tmp/luciole-recent-fixture/0000.txt")},
          [&](const std::string& error) { trimmed = error; }
      );
      until([&] { return !recent.busy(); });
      TEST_CHECK(trimmed.empty() && g_bookmark_file_get_size(parse(contents(file)).get()) == 2999);
    }
    fs::remove_all(root);
  }

  luciole::DirectoryEntry recentItem(const std::string& name, std::optional<luciole::Location> target, bool folder) {
    auto recent = std::make_shared<const luciole::Location>(luciole::Location::recent());
    luciole::DirectoryEntry entry{
        .id = 1,
        .place = luciole::EntryPlace::inFolder(recent),
        .name = name,
        .type = folder ? luciole::FileType::Directory : luciole::FileType::Regular,
        .size = {},
        .modifiedSeconds = {},
    };
    entry.setShortcutTarget(std::move(target));
    return entry;
  }

  // A Recent item opens, navigates to and is chosen as its file, while its own address stays the list's.
  void items() {
    const auto file = luciole::Location::fromPath("/tmp/elsewhere/report.pdf");
    const auto item = recentItem("1a2b", file, false);
    TEST_CHECK(item.location().isRecent() && item.recentTarget() && *item.recentTarget() == file);
    const auto standIn = item.atRecentTarget();
    TEST_CHECK(standIn.location() == file && !standIn.recentTarget() && standIn.name == "report.pdf");
    const auto folder = luciole::Location::fromPath("/tmp/elsewhere");
    TEST_CHECK(recentItem("3c4d", folder, true).navigationLocation() == folder);
    // Elsewhere a target is a provider shortcut's, not a Recent file.
    auto shortcut = recentItem("x", file, false);
    shortcut.place = luciole::Location::fromPath("/tmp/x");
    TEST_CHECK(shortcut.recentTarget() == nullptr);

    const auto recent = std::optional(luciole::Location::recent());
    const std::vector<luciole::DirectoryEntry> chosen{item};
    auto decision = luciole::chooserDecision({}, recent, chosen);
    TEST_CHECK(decision.kind == luciole::ChooserDecision::Kind::Accept && decision.locations == std::vector{file});
    const std::vector<luciole::DirectoryEntry> folders{recentItem("3c4d", folder, true)};
    decision = luciole::chooserDecision({}, recent, folders);
    TEST_CHECK(decision.kind == luciole::ChooserDecision::Kind::Enter && decision.locations == std::vector{folder});
    const std::vector<luciole::DirectoryEntry> lost{recentItem("5e6f", std::nullopt, false)};
    TEST_CHECK(luciole::chooserDecision({}, recent, lost).kind == luciole::ChooserDecision::Kind::Unavailable);
    // Recent itself is no folder to choose.
    TEST_CHECK(
        luciole::chooserDecision({.directory = true, .multiple = false}, recent, {}).kind
        == luciole::ChooserDecision::Kind::Unavailable
    );
  }

  // Recent lists the files used last first; re-sorting it and changing the default stay separate.
  void sorting() {
    using luciole::SortField;
    GlibPollSource source;
    luciole::BrowserController browser({}, false);
    browser.open(luciole::Location::recent());
    TEST_CHECK(browser.model().sort().field == SortField::Accessed && browser.model().sort().descending);
    browser.setDefaultSort({.field = SortField::Size});
    TEST_CHECK(browser.model().sort().field == SortField::Accessed);
    browser.setRecentSort({.field = SortField::Name});
    TEST_CHECK(browser.model().sort().field == SortField::Name);
    browser.open(luciole::Location::fromPath(fs::temp_directory_path().native()));
    TEST_CHECK(browser.model().sort().field == SortField::Size);
    TEST_CHECK(browser.back());
    TEST_CHECK(browser.model().sort().field == SortField::Name);
  }

  void ready(GlibPollSource& source, const luciole::BrowserController& browser) {
    for (int turns = 0; browser.model().snapshot().status == luciole::DirectoryStatus::Loading && turns < 2000;
         ++turns) {
      std::vector<pollfd> fds;
      const auto start = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 5) >= 0);
      source.dispatch(fds, start);
    }
    TEST_CHECK(browser.model().snapshot().status == luciole::DirectoryStatus::Ready);
  }

  // GVfs reloads the list shortly after the file changes, so look again until it shows `count` items.
  void listed(GlibPollSource& source, luciole::BrowserController& browser, std::size_t count) {
    for (int tries = 0; tries < 50 && browser.model().snapshot().entries.size() != count; ++tries) {
      std::vector<pollfd> fds;
      const auto start = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 100) >= 0);
      source.dispatch(fds, start);
      browser.refresh();
      ready(source, browser);
    }
    TEST_CHECK(browser.model().snapshot().entries.size() == count);
  }

  // Run by hand with GVfs's recent backend and a throwaway XDG_DATA_HOME: lists what Luciole recorded, then forgets
  // one file and clears the rest.
  void provider() {
    const fs::path root = g_get_user_data_dir();
    // This clears the list, so it never runs on the real one.
    if (root == fs::path(g_get_home_dir()) / ".local/share" || !g_getenv("XDG_DATA_HOME")) {
      std::println(stderr, "set XDG_DATA_HOME to a throwaway folder for the bus and this test");
      std::exit(2);
    }
    fs::create_directories(root / "files");
    const auto a = root / "files" / "first report.txt";
    const auto b = root / "files" / "photo.png";
    std::ofstream(a) << "a";
    std::ofstream(b) << "b";
    GlibPollSource source;
    luciole::RecentFiles recent;
    recent.recordUse(luciole::Location::fromPath(a.native()), {});
    std::string written = "pending";
    recent.forget({}, [&](const std::string& error) { written = error; });
    until([&] { return !recent.busy(); });
    if (!written.empty()) {
      std::println("write failed: {}", written);
    }
    TEST_CHECK(written.empty());
    luciole::BrowserController browser({}, false);
    browser.open(luciole::Location::recent());
    ready(source, browser);
    recent.recordUse(luciole::Location::fromPath(b.native()), {});
    until([&] { return !recent.busy(); });
    listed(source, browser, 2);
    const auto& entries = browser.model().snapshot().entries;
    for (const auto& entry : entries) {
      std::println(
          "{} -> {} in {} used {}", entry.displayName(), entry.recentTarget() ? entry.recentTarget()->uri() : "-",
          entry.parentDisplayName(), entry.details.accessedSeconds.value_or(-1)
      );
    }
    const auto first = std::ranges::find(entries, "first report.txt", &luciole::DirectoryEntry::displayName);
    TEST_CHECK(
        first != entries.end()
        && first->recentTarget()
        && *first->recentTarget() == luciole::Location::fromPath(a.native())
        && first->parentDisplayName() == (root / "files").native()
        && first->details.accessedSeconds.value_or(0) > 0
    );
    recent.forget({luciole::Location::fromPath(a.native())}, [](const std::string&) {});
    until([&] { return !recent.busy(); });
    listed(source, browser, 1);
    recent.clear([](const std::string&) {});
    until([&] { return !recent.busy(); });
    listed(source, browser, 0);
    // The files stay.
    TEST_CHECK(fs::exists(a) && fs::exists(b));
    std::println("recent provider checks passed");
  }
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--provider") {
    provider();
    return 0;
  }
  edits();
  store();
  items();
  sorting();
  return 0;
}
