#include "browser/chooser_policy.h"
#include "test_check.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

namespace {
  using luciole::ChooserDecision;
  using luciole::ChooserMode;
  using luciole::DirectoryEntry;
  using luciole::FileType;
  using luciole::Location;
  using Kind = luciole::ChooserDecision::Kind;

  const std::string kFolder = "/tmp/luciole-chooser-fixture";

  DirectoryEntry entry(luciole::EntryId id, const std::string& name, FileType type = FileType::Regular) {
    return {
        .id = id,
        .place = Location::fromPath(kFolder + "/" + name),
        .name = name,
        .type = type,
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt
    };
  }

  ChooserDecision decide(ChooserMode mode, const std::vector<DirectoryEntry>& entries) {
    return luciole::chooserDecision(mode, Location::fromPath(kFolder), entries);
  }

  void testFileMode() {
    const ChooserMode single{};
    const auto file = decide(single, {entry(1, "a.txt")});
    TEST_CHECK(file.kind == Kind::Accept && file.locations == std::vector{Location::fromPath(kFolder + "/a.txt")});
    TEST_CHECK(file.reason.empty());
    const auto folder = decide(single, {entry(2, "sub", FileType::Directory)});
    TEST_CHECK(folder.kind == Kind::Enter && folder.locations == std::vector{Location::fromPath(kFolder + "/sub")});
    const auto mixed =
        decide({.directory = false, .multiple = true}, {entry(1, "a.txt"), entry(2, "sub", FileType::Directory)});
    TEST_CHECK(mixed.kind == Kind::Unavailable && mixed.reason == "Only files can be chosen.");
    const auto two = decide(single, {entry(1, "a.txt"), entry(3, "b.txt")});
    TEST_CHECK(two.kind == Kind::Unavailable && two.reason == "Choose one file.");
    const auto many = decide({.directory = false, .multiple = true}, {entry(1, "a.txt"), entry(3, "b.txt")});
    TEST_CHECK(many.kind == Kind::Accept && many.locations.size() == 2);
    const auto none = decide(single, {});
    TEST_CHECK(none.kind == Kind::Unavailable && none.reason.empty());
    const auto special = decide(single, {entry(4, "fifo", FileType::Special)});
    TEST_CHECK(special.kind == Kind::Unavailable && special.reason == "Only files can be chosen.");
    auto pending = entry(1, "a.txt");
    pending.metadataPending = true;
    TEST_CHECK(decide(single, {pending}).reason == "Reading item metadata…");
  }

  void testDirectoryMode() {
    const ChooserMode mode{.directory = true, .multiple = false};
    const auto current = decide(mode, {});
    TEST_CHECK(current.kind == Kind::Accept && current.locations == std::vector{Location::fromPath(kFolder)});
    const auto folder = decide(mode, {entry(2, "sub", FileType::Directory)});
    TEST_CHECK(folder.kind == Kind::Accept && folder.locations.size() == 1);
    const auto file = decide(mode, {entry(1, "a.txt")});
    TEST_CHECK(file.kind == Kind::Unavailable && file.reason == "Only folders can be chosen.");
    const auto two = decide(mode, {entry(2, "sub", FileType::Directory), entry(3, "other", FileType::Directory)});
    TEST_CHECK(two.kind == Kind::Unavailable);
    TEST_CHECK(
        decide(
            {.directory = true, .multiple = true},
            {entry(2, "sub", FileType::Directory), entry(3, "other", FileType::Directory)}
        )
            .kind
        == Kind::Accept
    );
  }

  void testNonNative() {
    const auto trash = luciole::chooserDecision({}, Location::trash(), std::vector{entry(1, "a.txt")});
    TEST_CHECK(trash.kind == Kind::Unavailable && trash.reason == "Only local files can be chosen.");
    TEST_CHECK(luciole::chooserDecision({}, std::nullopt, {}).reason == "Only local files can be chosen.");
  }

  void testSymlinks() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("luciole-chooser-links-" + std::to_string(::getpid()));
    fs::create_directories(root / "dir");
    std::ofstream(root / "file") << "x";
    fs::create_directory_symlink(root / "dir", root / "dir-link");
    fs::create_symlink(root / "file", root / "file-link");
    fs::create_symlink(root / "missing", root / "dangling");
    const auto link = [&](luciole::EntryId id, const std::string& name) {
      return DirectoryEntry{
          .id = id,
          .place = Location::fromPath((root / name).string()),
          .name = name,
          .type = FileType::Symlink,
          .symlink = true,
          .size = std::nullopt,
          .modifiedSeconds = std::nullopt
      };
    };
    const auto folder = std::optional(Location::fromPath(root.string()));
    std::vector dirLink{link(1, "dir-link")};
    std::vector fileLink{link(2, "file-link")};
    const std::vector dangling{link(3, "dangling")};
    TEST_CHECK(luciole::chooserDecision({}, folder, dirLink).kind == Kind::Unavailable);
    dirLink.front().type = FileType::Directory;
    fileLink.front().type = FileType::Regular;
    TEST_CHECK(luciole::chooserDecision({}, folder, dirLink).kind == Kind::Enter);
    TEST_CHECK(luciole::chooserDecision({}, folder, fileLink).kind == Kind::Accept);
    TEST_CHECK(luciole::chooserDecision({}, folder, dangling).kind == Kind::Unavailable);
    TEST_CHECK(luciole::chooserDecision({.directory = true, .multiple = false}, folder, dirLink).kind == Kind::Accept);
    // Saving follows a link itself rather than trusting the listing's mark, which GIO listings never set and native
    // ones set late or keep from an older target.
    TEST_CHECK(luciole::isLink(dirLink.front()) && !luciole::isLink(entry(4, "a.txt")));
    TEST_CHECK(!luciole::offersSaveName(luciole::chosenType(dirLink.front())));
    TEST_CHECK(luciole::offersSaveName(luciole::chosenType(fileLink.front())));
    TEST_CHECK(luciole::offersSaveName(luciole::chosenType(dangling.front())));
    auto repointed = fileLink.front();
    repointed.linksToDirectory = true;
    TEST_CHECK(luciole::offersSaveName(luciole::chosenType(repointed)));
    fs::remove_all(root);
  }

  void testSelection() {
    luciole::DirectoryModel model;
    luciole::DirectorySnapshot snapshot;
    snapshot.generation = 1;
    snapshot.location = Location::fromPath(kFolder);
    snapshot.status = luciole::DirectoryStatus::Ready;
    snapshot.entries = std::vector{entry(1, "b.txt"), entry(2, "a.txt"), entry(3, "c.txt")};
    model.applySnapshot(std::move(snapshot));
    TEST_CHECK(model.select(3) && model.select(2, luciole::SelectionMode::Toggle));
    const auto chosen = luciole::chooserSelection(model);
    TEST_CHECK(chosen.size() == 2 && chosen[0].id == 2 && chosen[1].id == 3);
  }

  // Selecting a file while saving offers its name to save over; folders are entered instead.
  void testSaveNames() {
    TEST_CHECK(luciole::offersSaveName(FileType::Regular));
    TEST_CHECK(luciole::offersSaveName(FileType::Symlink));
    TEST_CHECK(!luciole::offersSaveName(FileType::Directory));
    TEST_CHECK(luciole::chosenType(entry(1, "a.txt")) == FileType::Regular);
    TEST_CHECK(luciole::chosenType(entry(2, "sub", FileType::Directory)) == FileType::Directory);
  }

  void testFileNames() {
    TEST_CHECK(luciole::fileNameProblem("report.odt").empty());
    TEST_CHECK(luciole::fileNameProblem("r\xe9port").empty());
    TEST_CHECK(luciole::fileNameProblem("") == "Enter a file name.");
    TEST_CHECK(luciole::fileNameProblem("..") == "Choose another name.");
    TEST_CHECK(!luciole::fileNameProblem("../etc/passwd").empty());
    TEST_CHECK(!luciole::fileNameProblem(std::string_view("a\0b", 3)).empty());
  }
} // namespace

int main() {
  try {
    testFileMode();
    testDirectoryMode();
    testNonNative();
    testSymlinks();
    testSelection();
    testSaveNames();
    testFileNames();
    std::println("chooser policy checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "chooser policy test: {}", error.what());
    return 1;
  }
}
