#include "config/state_store.h"
#include "fs/gio_ptr.h"
#include "test_check.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>
#include <sys/stat.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::ActivePane;
  using luciole::AtomicWriteState;
  using luciole::Bookmark;
  using luciole::Location;
  using luciole::SavedWindow;
  using luciole::StateStore;

  struct Fixture {
    fs::path directory;
    Fixture() {
      const luciole::GString name(g_dir_make_tmp("luciole-state-test-XXXXXX", nullptr));
      TEST_CHECK(name != nullptr);
      directory = name.get();
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path path() const { return directory / "state.toml"; }
  };

  void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    TEST_CHECK(output.good());
  }

  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  SavedWindow window(const std::string& path) {
    return {
        .tabs = {{.primary = Location::fromPath(path), .secondary = std::nullopt, .activePane = ActivePane::Primary}},
        .activeTab = 0
    };
  }

  void testRoundtrip() {
    Fixture fixture;
    const auto path = fixture.directory / "not-created" / "state.toml";
    StateStore store(path);
    TEST_CHECK(store.saveBookmarks({}) == AtomicWriteState::NotCommitted);
    TEST_CHECK(store.load() && !fs::exists(path.parent_path()));
    const std::string native =
        (fixture.directory / "space #percent%\n").native() + std::string(1, static_cast<char>(0xFF));
    const auto local = Location::fromPath(native);
    const auto remote = Location::fromUri("sftp://alice@example.test/shared%20folder");
    std::string deepNative = fixture.directory.native();
    for (int i = 0; i < 100; ++i) {
      deepNative += '/' + std::string(30, static_cast<char>(0xFF));
    }
    const auto deepLocation = Location::fromPath(deepNative);
    TEST_CHECK(deepLocation.uri().size() > 8192);
    const std::vector<Bookmark> bookmarks{
        {.label = "Work \"files\"", .location = local, .iconName = "folder"},
        {.label = "共享", .location = remote, .iconName = {}},
        {.label = "Deep native path", .location = deepLocation, .iconName = {}}
    };
    TEST_CHECK(store.saveBookmarks(bookmarks) == AtomicWriteState::Durable);
    std::vector<SavedWindow> windows{window("/tmp/second-tab"), window("/tmp/other-window")};
    windows[0].tabs.insert(
        windows[0].tabs.begin(), {.primary = local, .secondary = remote, .activePane = ActivePane::Secondary}
    );
    windows[0].activeTab = 1;
    TEST_CHECK(store.saveWindows(windows) == AtomicWriteState::Durable);
    StateStore restored(path);
    TEST_CHECK(restored.load() && restored.state().bookmarks == bookmarks && restored.state().windows == windows);
    TEST_CHECK(restored.state().bookmarks.front().location.nativePath() == native);
    TEST_CHECK(read(path).contains("schema_version = 1"));
    const auto bytes = read(path);
    struct stat before{};
    TEST_CHECK(::stat(path.c_str(), &before) == 0 && (before.st_mode & 0777) == 0600);
    TEST_CHECK(restored.saveWindows(windows) == AtomicWriteState::Unchanged && read(path) == bytes);
    struct stat after{};
    TEST_CHECK(::stat(path.c_str(), &after) == 0 && before.st_ino == after.st_ino);
  }

  void testSectionMergingAndConflicts() {
    Fixture fixture;
    StateStore first(fixture.path());
    StateStore second(fixture.path());
    TEST_CHECK(first.load() && second.load());
    const std::vector<Bookmark> a{{.label = "A", .location = Location::fromPath("/tmp/a"), .iconName = {}}};
    const std::vector<Bookmark> b{{.label = "B", .location = Location::fromPath("/tmp/b"), .iconName = {}}};
    const std::vector<Bookmark> c{{.label = "C", .location = Location::fromPath("/tmp/c"), .iconName = {}}};
    TEST_CHECK(first.saveBookmarks(a) == AtomicWriteState::Durable);
    TEST_CHECK(
        second.saveWindows({window("/tmp/session-one")}) == AtomicWriteState::Durable && second.state().bookmarks == a
    );
    TEST_CHECK(first.saveBookmarks(b) == AtomicWriteState::Durable && first.state().windows == second.state().windows);
    const auto before = read(fixture.path());
    const auto lastGood = second.state();
    TEST_CHECK(
        second.saveBookmarks(c) == AtomicWriteState::NotCommitted
        && read(fixture.path()) == before
        && second.state() == lastGood
        && !second.issues().empty()
    );
    TEST_CHECK(second.load() && second.saveBookmarks(c) == AtomicWriteState::Durable);
    TEST_CHECK(
        first.saveWindows({window("/tmp/session-two")}) == AtomicWriteState::Durable && first.state().bookmarks == c
    );
    const auto currentBytes = read(fixture.path());
    TEST_CHECK(
        second.saveWindows({window("/tmp/stale-session")}) == AtomicWriteState::NotCommitted
        && read(fixture.path()) == currentBytes
    );
    TEST_CHECK(second.saveBookmarks(c) == AtomicWriteState::Unchanged && second.state() == first.state());
    // The picker folder is last-writer-wins and leaves the other sections alone.
    TEST_CHECK(
        second.saveChooserFolder(Location::fromPath("/tmp/picked")) == AtomicWriteState::Durable
        && second.state().windows == first.state().windows
    );
    TEST_CHECK(first.saveChooserFolder(Location::fromPath("/tmp/picked-again")) == AtomicWriteState::Durable);
    StateStore reloaded(fixture.path());
    TEST_CHECK(
        reloaded.load()
        && reloaded.state().chooserFolder == Location::fromPath("/tmp/picked-again")
        && reloaded.state().bookmarks == c
    );
  }

  void testEmptyPlacesSnapshot() {
    Fixture fixture;
    StateStore first(fixture.path());
    TEST_CHECK(first.load() && !first.state().placesCustomized);
    TEST_CHECK(first.saveWindows({window("/tmp/initial")}) == AtomicWriteState::Durable);
    StateStore stale(fixture.path());
    TEST_CHECK(stale.load() && !stale.state().placesCustomized);
    TEST_CHECK(first.saveBookmarks({}) == AtomicWriteState::Durable);
    TEST_CHECK(
        stale.saveBookmarks({{.label = "Stale", .location = Location::fromPath("/tmp/stale"), .iconName = {}}})
        == AtomicWriteState::NotCommitted
    );
    TEST_CHECK(stale.saveWindows({window("/tmp/updated")}) == AtomicWriteState::Durable);
    TEST_CHECK(first.load() && first.state().placesCustomized && first.state().bookmarks.empty());
    const auto bytes = read(fixture.path());
    TEST_CHECK(first.saveBookmarks({}) == AtomicWriteState::Unchanged && read(fixture.path()) == bytes);
  }

  void testInvalidStateAndRecovery() {
    Fixture fixture;
    StateStore store(fixture.path());
    TEST_CHECK(store.load() && store.saveWindows({window("/tmp/valid")}) == AtomicWriteState::Durable);
    const auto lastGood = store.state();
    for (const auto& invalid : {
             "",
             "schema_version = 2",
             "schema_version = true",
             "broken = [",
             "schema_version = 1\nunknown = true",
             "schema_version = 1\nbookmarks = 0",
             "schema_version = 1\nplaces_customized = 1",
             "schema_version = 1\nplaces_customized = true",
             "schema_version = 1\nbookmarks = [{label = 'x', uri = '/relative'}]",
             "schema_version = 1\nbookmarks = [{label = '', uri = 'file:///tmp'}]",
             "schema_version = 1\nbookmarks = [{label = 'x', uri = 'file:///tmp', extra = true}]",
             "schema_version = 1\nwindows = [{active_tab = 0, tabs = []}]",
             "schema_version = 1\nwindows = [{active_tab = -1, tabs = []}]",
             "schema_version = 1\nwindows = [{active_tab = 2, tabs = [{primary_uri = 'file:///tmp', active_pane = "
             "'primary'}]}]",
             "schema_version = 1\nwindows = [{active_tab = 0, tabs = [{primary_uri = 'file:///tmp', active_pane = "
             "'secondary'}]}]",
             "schema_version = 1\nwindows = [{active_tab = 0, tabs = [{primary_uri = 'file:///tmp', active_pane = "
             "'invalid'}]}]",
         }) {
      write(fixture.path(), invalid);
      TEST_CHECK(!store.load() && store.state() == lastGood && !store.issues().empty());
      TEST_CHECK(
          store.saveWindows({window("/tmp/replacement")}) == AtomicWriteState::NotCommitted
          && read(fixture.path()) == invalid
      );
    }
    write(fixture.path(), "schema_version = true\n");
    TEST_CHECK(!store.load() && store.issues().front().line == 1 && store.issues().front().key == "schema_version");
    fs::remove(fixture.path());
    TEST_CHECK(store.load() && store.state() == luciole::SessionState{});
  }

  void testPrivacyAndBounds() {
    Fixture fixture;
    StateStore store(fixture.path());
    TEST_CHECK(store.load() && store.saveWindows({window("/tmp/valid")}) == AtomicWriteState::Durable);
    const auto original = read(fixture.path());
    for (const std::string uri :
         {"sftp://alice:super-secret@example.test/path", "sftp://alice;token=super-secret@example.test/path",
          "https://example.test/path?token=super-secret", "https://example.test/path#super-secret"}) {
      const std::vector<Bookmark> bookmarks{{.label = "Remote", .location = Location::fromUri(uri), .iconName = {}}};
      TEST_CHECK(store.saveBookmarks(bookmarks) == AtomicWriteState::NotCommitted && read(fixture.path()) == original);
      for (const auto& issue : store.issues()) {
        TEST_CHECK(!issue.format().contains("super-secret"));
      }
      write(fixture.path(), "schema_version = 1\nbookmarks = [{label = 'Remote', uri = '" + uri + "'}]\n");
      TEST_CHECK(!store.load());
      write(fixture.path(), original);
    }
    const auto local = Location::fromPath("/tmp/repeated");
    TEST_CHECK(
        store.saveBookmarks(
            {{.label = "one", .location = local, .iconName = {}}, {.label = "two", .location = local, .iconName = {}}}
        )
        == AtomicWriteState::NotCommitted
    );
    for (const auto& label :
         {std::string(257, 'x'), std::string("line\nbreak"), std::string(1, static_cast<char>(0xFF))}) {
      TEST_CHECK(
          store.saveBookmarks({{.label = label, .location = local, .iconName = {}}}) == AtomicWriteState::NotCommitted
      );
    }
    TEST_CHECK(
        store.saveWindows(std::vector<SavedWindow>(17, window("/tmp/window"))) == AtomicWriteState::NotCommitted
    );
    auto manyTabs = window("/tmp/tabs");
    manyTabs.tabs.resize(65, manyTabs.tabs.front());
    TEST_CHECK(store.saveWindows({manyTabs}) == AtomicWriteState::NotCommitted);
    TEST_CHECK(read(fixture.path()) == original);
  }

  void testPrivateReaderAndPaths() {
    Fixture fixture;
    StateStore store(fixture.path());
    TEST_CHECK(store.load() && store.saveWindows({window("/tmp/valid")}) == AtomicWriteState::Durable);
    const auto original = read(fixture.path());
    TEST_CHECK(::chmod(fixture.path().c_str(), 0444) == 0 && store.load());
    TEST_CHECK(
        store.saveBookmarks({{.label = "x", .location = Location::fromPath("/tmp/x"), .iconName = {}}})
        == AtomicWriteState::NotCommitted
    );
    struct stat info{};
    TEST_CHECK(::stat(fixture.path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0444);
    const auto target = fixture.directory / "target";
    fs::rename(fixture.path(), target);
    fs::create_symlink(target, fixture.path());
    TEST_CHECK(!store.load() && read(target) == original);
    fs::remove(fixture.path());
    fs::create_hard_link(target, fixture.path());
    TEST_CHECK(!store.load());
    fs::remove(fixture.path());
    fs::create_directory(fixture.path());
    TEST_CHECK(!store.load());
    fs::remove(fixture.path());
    write(fixture.path(), std::string(256 * 1024 + 1, '#'));
    TEST_CHECK(!store.load());

    const char* previous = std::getenv("XDG_STATE_HOME");
    const std::optional<std::string> saved = previous != nullptr ? std::optional<std::string>(previous) : std::nullopt;
    ::setenv("XDG_STATE_HOME", "/tmp/luciole-state-path-test", 1);
    TEST_CHECK(StateStore::pathFromEnvironment() == "/tmp/luciole-state-path-test/luciole/state.toml");
    ::setenv("XDG_STATE_HOME", "relative-invalid", 1);
    bool rejected = false;
    try {
      (void)StateStore::pathFromEnvironment();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
    ::unsetenv("XDG_STATE_HOME");
    TEST_CHECK(StateStore::pathFromEnvironment() == fs::path(std::getenv("HOME")) / ".local/state/luciole/state.toml");
    if (saved) {
      ::setenv("XDG_STATE_HOME", saved->c_str(), 1);
    }
  }

  void testPlaceSeparators() {
    Fixture fixture;
    const std::vector<Bookmark> bookmarks{
        {.label = "a", .location = Location::fromPath("/tmp/a"), .iconName = {}},
        {.label = "b", .location = Location::fromPath("/tmp/b"), .iconName = {}},
        {.label = "c", .location = Location::fromPath("/tmp/c"), .iconName = {}}
    };
    StateStore store(fixture.path());
    TEST_CHECK(store.load());
    TEST_CHECK(store.saveBookmarks(bookmarks, {1, 2}) == AtomicWriteState::Durable);
    TEST_CHECK(read(fixture.path()).contains("place_separators"));
    StateStore reread(fixture.path());
    TEST_CHECK(reread.load() && (reread.state().placeSeparators == std::vector<std::size_t>{1, 2}));
    // Saving the windows leaves the separators alone.
    TEST_CHECK(reread.saveWindows({window("/tmp")}) == AtomicWriteState::Durable);
    TEST_CHECK(store.load() && (store.state().placeSeparators == std::vector<std::size_t>{1, 2}));
    // Without separators the key is left out of the file.
    TEST_CHECK(store.saveBookmarks(bookmarks) == AtomicWriteState::Durable);
    TEST_CHECK(!read(fixture.path()).contains("place_separators") && store.state().placeSeparators.empty());

    // A separator can't sit before the first row.
    write(fixture.path(), "schema_version = 1\nplace_separators = [3, 0]\n");
    TEST_CHECK(!store.load() && store.issues().front().key == "place_separators[1]");
    // Rows out of order or repeated are dropped, keeping an increasing list.
    write(fixture.path(), "schema_version = 1\nplace_separators = [4, 2, 4, 6]\n");
    TEST_CHECK(store.load() && (store.state().placeSeparators == std::vector<std::size_t>{4, 6}));
  }

  void testPlaceSeparatorHeadings() {
    Fixture fixture;
    const std::vector<Bookmark> bookmarks{
        {.label = "a", .location = Location::fromPath("/tmp/a"), .iconName = {}},
        {.label = "b", .location = Location::fromPath("/tmp/b"), .iconName = {}},
        {.label = "c", .location = Location::fromPath("/tmp/c"), .iconName = {}}
    };
    StateStore store(fixture.path());
    TEST_CHECK(store.load());
    TEST_CHECK(store.saveBookmarks(bookmarks, {1, 2}, {"", "Work"}) == AtomicWriteState::Durable);
    TEST_CHECK(read(fixture.path()).contains("place_separator_headings"));
    StateStore reread(fixture.path());
    TEST_CHECK(reread.load() && (reread.state().placeSeparatorHeadings == std::vector<std::string>{"", "Work"}));
    // Without headings the key is left out, so builds without them can still read the file.
    TEST_CHECK(store.saveBookmarks(bookmarks, {1, 2}) == AtomicWriteState::Durable);
    TEST_CHECK(!read(fixture.path()).contains("place_separator_headings"));
    TEST_CHECK(store.state().placeSeparatorHeadings == std::vector<std::string>(2));

    // A heading for a row without a separator is dropped.
    write(
        fixture.path(), "schema_version = 1\nplace_separators = [2]\nplace_separator_headings = {2 = 'A', 3 = 'B'}\n"
    );
    TEST_CHECK(store.load() && (store.state().placeSeparatorHeadings == std::vector<std::string>{"A"}));
    write(fixture.path(), "schema_version = 1\nplace_separators = [2]\nplace_separator_headings = {x = 'A'}\n");
    TEST_CHECK(!store.load() && store.issues().front().key == "place_separator_headings.x");
    write(fixture.path(), "schema_version = 1\nplace_separators = [2]\nplace_separator_headings = {2 = 5}\n");
    TEST_CHECK(!store.load() && store.issues().front().key == "place_separator_headings.2");
    write(fixture.path(), "schema_version = 1\nplace_separators = [2]\nplace_separator_headings = {2 = ''}\n");
    TEST_CHECK(!store.load());
    // A heading of only spaces, Unicode ones included, would draw nothing, so it is refused like an empty one.
    write(fixture.path(), "schema_version = 1\nplace_separators = [2]\nplace_separator_headings = {2 = ' \u00a0'}\n");
    TEST_CHECK(!store.load() && store.issues().front().key == "place_separator_headings.2");

    // Headings are checked like place labels, but worded for headings, and blank text is refused.
    TEST_CHECK(luciole::separatorHeadingProblem("") == "Enter a heading.");
    TEST_CHECK(luciole::separatorHeadingProblem("   ") == "Enter a heading.");
    TEST_CHECK(luciole::separatorHeadingProblem("\u00a0\u3000\u2003") == "Enter a heading.");
    TEST_CHECK(
        luciole::separatorHeadingProblem(std::string(257, 'x')) == "Headings can contain at most 256 UTF-8 bytes."
    );
    TEST_CHECK(luciole::separatorHeadingProblem("Bad\tline") == "Headings cannot contain control characters.");
    TEST_CHECK(!luciole::separatorHeadingProblem(" Work ") && !luciole::separatorHeadingProblem("\u00a0Work"));
    // Place labels keep their own wording.
    TEST_CHECK(luciole::bookmarkLabelProblem("") == "Enter a label.");
    TEST_CHECK(luciole::bookmarkLabelProblem("Bad\tline") == "Labels cannot contain control characters.");
  }
} // namespace

int main() {
  try {
    testRoundtrip();
    testSectionMergingAndConflicts();
    testEmptyPlacesSnapshot();
    testInvalidStateAndRecovery();
    testPrivacyAndBounds();
    testPrivateReaderAndPaths();
    testPlaceSeparators();
    testPlaceSeparatorHeadings();
    std::println("state store checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "state store test: {}", error.what());
    return 1;
  }
}
