#include "browser/browser_controller.h"
#include "browser/directory_tree.h"
#include "config/settings.h"
#include "directory_fixture.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  void wait(const std::function<bool()>& done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < end);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void drain() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    wait([&] { return std::chrono::steady_clock::now() >= end; });
  }
  luciole::EntryId find(const luciole::DirectoryModel& model, const fs::path& path) {
    const auto location = luciole::Location::fromPath(path.native());
    for (const auto id : model.rows()) {
      if (model.entry(id)->location() == location) {
        return id;
      }
    }
    return 0;
  }
  auto ready(luciole::BrowserController& browser) {
    wait([&] { return browser.model().snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(browser.model().snapshot().status == luciole::DirectoryStatus::Ready);
  }
  void details(const fs::path& root) {
    fs::create_directories(root / "a" / "nested");
    fs::create_directories(root / "b");
    std::ofstream(root / "a" / "nested" / "café-<literal>-文.txt") << "payload";
    std::ofstream(root / "a" / "z.txt") << "z";
    std::ofstream(root / "a" / ".hidden") << 'h';
    fs::create_directory_symlink("..", root / "a" / "cycle");
    luciole::BrowserController browser({}, false);
    browser.open(luciole::Location::fromPath(root.native()));
    ready(browser);
    TEST_CHECK(!browser.expandableFolders() && browser.model().rows().size() == 2);
    const auto rootSnapshot = browser.model().snapshot();
    const auto* rootEntry = &rootSnapshot.entries.front();
    browser.setExpandableFolders(true);
    auto& model = browser.model();
    TEST_CHECK(&model.snapshot().entries.front() == rootEntry);
    const auto a = find(model, root / "a");
    TEST_CHECK(browser.toggleFolder(a));
    wait([&] { return !browser.treeRow(a).loading; });
    TEST_CHECK(model.snapshot().location == luciole::Location::fromPath(root.native()));
    const auto nested = find(model, root / "a" / "nested");
    TEST_CHECK(nested && model.treeParent(nested) == a && model.treeDepth(nested) == 1);
    TEST_CHECK(!find(model, root / "a" / ".hidden"));
    TEST_CHECK(!browser.treeRow(find(model, root / "a" / "cycle")).expandable);
    TEST_CHECK(browser.toggleFolder(nested));
    wait([&] { return !browser.treeRow(nested).loading; });
    const auto leaf = find(model, root / "a" / "nested" / "café-<literal>-文.txt");
    TEST_CHECK(leaf && model.treeDepth(leaf) == 2);
    const auto expandedSnapshot = model.snapshot();
    const auto* leafEntry = model.entry(leaf);
    TEST_CHECK(&model.snapshot().entries.front() == rootEntry);
    TEST_CHECK(model.select(leaf));
    std::optional<luciole::Location> opened;
    browser.setFileActivatedCallback([&](const auto& location) { opened = location; });
    browser.activate(leaf);
    TEST_CHECK(opened == luciole::Location::fromPath((root / "a" / "nested" / "café-<literal>-文.txt").native()));
    auto sort = model.sort();
    sort.descending = true;
    model.setSort(sort);
    TEST_CHECK(model.selection().selected.contains(leaf));
    const auto position = std::ranges::find(model.rows(), nested);
    TEST_CHECK(position != model.rows().end() && *(position + 1) == leaf);
    browser.refresh();
    ready(browser);
    wait([&] { return !browser.treeRow(a).loading && !browser.treeRow(nested).loading; });
    TEST_CHECK(find(model, root / "a" / "nested" / "café-<literal>-文.txt") == leaf);
    TEST_CHECK(model.selection().selected.contains(leaf));
    browser.toggleHidden();
    TEST_CHECK(find(model, root / "a" / ".hidden"));
    TEST_CHECK(browser.toggleFolder(a));
    TEST_CHECK(!model.entry(leaf) && !model.selection().selected.contains(leaf));
    const auto held = std::ranges::find(expandedSnapshot.entries, leaf, &luciole::DirectoryEntry::id);
    TEST_CHECK(held != expandedSnapshot.entries.end() && &*held == leafEntry);
    TEST_CHECK(held->location() == luciole::Location::fromPath((root / "a/nested/café-<literal>-文.txt").native()));
    TEST_CHECK(browser.toggleFolder(a));
    wait([&] { return !browser.treeRow(a).loading; });
    TEST_CHECK(!browser.treeRow(find(model, root / "a" / "nested")).expanded);
    model.select(a);
    TEST_CHECK(browser.treeKey(true));
    TEST_CHECK(model.treeParent(*model.selection().focused) == a);
    TEST_CHECK(browser.treeKey(false) && model.selection().focused == a);
    browser.setSearchQuery("z.txt");
    ready(browser);
    TEST_CHECK(!browser.treeRow(a).enabled && model.rows().size() == 1);
    browser.setSearchQuery({});
    ready(browser);
    TEST_CHECK(!browser.treeRow(a).expanded);
    browser.setExpandableFolders(false);
    TEST_CHECK(!browser.treeRow(a).enabled && model.rows().size() == 2);
  }
  luciole::DirectoryEntry rootEntry(const fs::path& path, luciole::EntryId id) {
    return {
        .id = id,
        .place = luciole::Location::fromPath(path.native()),
        .name = path.filename().native(),
        .type = luciole::FileType::Directory,
        .size = {},
        .modifiedSeconds = {}
    };
  }
  void boundsAndSidebar(const fs::path& root) {
    luciole::DirectoryModel model;
    luciole::DirectoryTree tree(model, true);
    luciole::DirectorySnapshot snapshot;
    snapshot.status = luciole::DirectoryStatus::Ready;
    snapshot.entries = {rootEntry(root / "b", 10), rootEntry(root / "a", 20), rootEntry(root / "a" / "nested", 30)};
    tree.setRoot(snapshot);
    TEST_CHECK((model.rows() == std::vector<luciole::EntryId>{10, 20, 30}));
    TEST_CHECK(tree.expand(20));
    wait([&] { return !tree.row(20).loading; });
    TEST_CHECK(model.rows().size() == 4); // Only real subfolders, no file/symlink entries.
    const auto child = model.rows()[2];
    TEST_CHECK(model.treeParent(child) == 20 && model.entry(child)->location() == model.entry(30)->location());
    TEST_CHECK(tree.expand(child));
    wait([&] { return !tree.row(child).loading; });
    TEST_CHECK(tree.row(child).expanded && !tree.row(30).expanded); // Same URI, independent tree occurrences.
    TEST_CHECK(tree.expand(10));
    wait([&] { return !tree.row(10).loading; });
    tree.releaseLocation(luciole::Location::fromPath((root / "a").native()));
    TEST_CHECK(!tree.row(20).expanded && !tree.row(child).expanded && tree.row(10).expanded);
    TEST_CHECK(tree.branches() == 1);
    TEST_CHECK(tree.collapse(10) && tree.branches() == 0);
    snapshot.entries = {};
    for (std::size_t i = 0; i < luciole::DirectoryTree::maxBranches + 1; ++i) {
      const auto path = root / ("branch-" + std::to_string(i));
      fs::create_directory(path);
      test::editEntries(snapshot, [&](auto& entries) { entries.push_back(rootEntry(path, i + 1)); });
    }
    tree.setRoot(snapshot);
    for (std::size_t i = 0; i < luciole::DirectoryTree::maxBranches; ++i) {
      TEST_CHECK(tree.expand(i + 1));
      TEST_CHECK(tree.loading() <= luciole::DirectoryTree::maxLoading);
    }
    TEST_CHECK(!tree.expand(luciole::DirectoryTree::maxBranches + 1));
    TEST_CHECK(
        tree.branches() == luciole::DirectoryTree::maxBranches
        && !tree.row(luciole::DirectoryTree::maxBranches + 1).status.empty()
    );
    tree.refresh();
    TEST_CHECK(tree.busy() && tree.loading() <= luciole::DirectoryTree::maxLoading);
    wait([&] { return !tree.busy(); });
    TEST_CHECK(tree.loading() == 0);
    tree.clear();
    TEST_CHECK(tree.branches() == 0);
    drain();
    auto cycleRoot = rootEntry(root / "a", 1);
    const auto info = luciole::GioPtr<GFileInfo>(g_file_query_info(
        luciole::gioFile(luciole::Location::fromPath((root / "a" / "nested").native())).get(),
        "id::file,id::filesystem", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    cycleRoot.setIdentity(luciole::readFileIdentity(info.get()));
    TEST_CHECK(cycleRoot.identity());
    snapshot.entries = {cycleRoot};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(1));
    wait([&] { return !tree.row(1).loading; });
    const auto repeated = find(model, root / "a" / "nested");
    TEST_CHECK(repeated && !tree.expand(repeated) && tree.row(repeated).status == "Folder cycle blocked");
    test::editEntries(snapshot, [&](auto& entries) {
      entries.front().setIdentity(luciole::FileIdentity{"changed", "changed"});
    });
    tree.setRoot(snapshot);
    TEST_CHECK(tree.branches() == 0 && model.rows().size() == 1);
    const auto deep = root / "deep";
    auto path = deep;
    for (std::size_t depth = 0; depth <= luciole::DirectoryTree::maxDepth + 1; ++depth) {
      path /= "level";
      fs::create_directories(path);
    }
    snapshot.entries = {rootEntry(deep, 1)};
    tree.setRoot(snapshot);
    auto current = luciole::EntryId{1};
    path = deep;
    for (std::size_t depth = 0; depth < luciole::DirectoryTree::maxDepth; ++depth) {
      TEST_CHECK(tree.expand(current));
      wait([&] { return !tree.row(current).loading; });
      path /= "level";
      current = find(model, path);
      TEST_CHECK(current);
    }
    TEST_CHECK(model.treeDepth(current) == luciole::DirectoryTree::maxDepth && !tree.expand(current));
    tree.clear();
    const auto many = root / "many";
    fs::create_directory(many);
    for (std::size_t i = 0; i < luciole::DirectoryTree::maxChildren + 20; ++i) {
      fs::create_directory(many / std::to_string(i));
    }
    snapshot.entries = {rootEntry(many, 1)};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(1));
    wait([&] { return !tree.row(1).loading; });
    TEST_CHECK(model.rows().size() == luciole::DirectoryTree::maxChildren + 1 && !tree.row(1).status.empty());
    TEST_CHECK(tree.collapse(1) && model.rows().size() == 1);
    auto missing = rootEntry(root / "missing", 2);
    snapshot.entries = {missing};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(2));
    wait([&] { return !tree.row(2).loading; });
    TEST_CHECK(!tree.row(2).status.empty());
    tree.clear();
    snapshot.entries = {rootEntry(root / "a", 1)};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(1));
    tree.clear();
    drain();
    TEST_CHECK(tree.branches() == 0);
    {
      luciole::DirectoryModel temporary;
      luciole::DirectoryTree retiring(temporary);
      retiring.setRoot(snapshot);
      TEST_CHECK(retiring.expand(1));
    }
    drain();
  }
  void providerShortcuts(const fs::path& root) {
    luciole::DirectoryModel model;
    luciole::DirectoryTree tree(model, true);
    luciole::DirectorySnapshot snapshot;
    snapshot.status = luciole::DirectoryStatus::Ready;
    auto shortcut = rootEntry(root / "unrelated", 10);
    shortcut.place = luciole::Location::network().child("server");
    shortcut.type = luciole::FileType::Shortcut;
    shortcut.setShortcutTarget(luciole::Location::fromPath((root / "a").native()));
    snapshot.entries = {shortcut};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.row(10).expandable && tree.expand(10));
    wait([&] { return !tree.row(10).loading; });
    TEST_CHECK(find(model, root / "a" / "nested"));
    TEST_CHECK(model.entry(10)->location() == shortcut.location());
    shortcut.setShortcutTarget(luciole::Location::fromPath((root / "b").native()));
    snapshot.entries = {shortcut};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.branches() == 0 && model.rows().size() == 1);
    TEST_CHECK(!find(model, root / "a" / "nested") && tree.expand(10));
    wait([&] { return !tree.row(10).loading; });
    TEST_CHECK(model.rows().size() == 1);
  }

  void settings(const fs::path& root) {
    using P = luciole::BrowserPreference;
    const luciole::ConfigPaths paths{root / "config.toml", root / "settings.toml"};
    luciole::ConfigService service(paths);
    TEST_CHECK(service.reload());
    TEST_CHECK(!service.settings().browser.sidebarTree && !service.settings().browser.expandableFolders);
    std::ofstream(paths.base) << "[browser]\nsidebar_tree = true\nexpandable_folders = true\nshow_hidden = true\n";
    TEST_CHECK(service.reload());
    const std::array edits{
        luciole::OverrideEdit{P::SidebarTree, false}, luciole::OverrideEdit{P::ExpandableFolders, false}
    };
    TEST_CHECK(service.saveOverrides(edits) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(paths);
    TEST_CHECK(restored.reload());
    TEST_CHECK(
        !restored.settings().browser.sidebarTree
        && !restored.settings().browser.expandableFolders
        && restored.settings().browser.showHidden
    );
    const std::array reset{
        luciole::OverrideEdit{P::SidebarTree, std::nullopt}, luciole::OverrideEdit{P::ExpandableFolders, std::nullopt}
    };
    TEST_CHECK(restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(restored.settings().browser.sidebarTree && restored.settings().browser.expandableFolders);
    for (const auto* key : {"sidebar_tree", "expandable_folders"}) {
      std::ofstream(paths.base) << "[browser]\n" << key << " = 'true'\n";
      TEST_CHECK(!luciole::validateConfigFile(paths.base).empty());
      TEST_CHECK(!restored.reload() && restored.settings().browser.sidebarTree);
    }
  }
} // namespace
int main() {
  const luciole::GString temporary(g_dir_make_tmp("luciole-directory-tree-XXXXXX", nullptr));
  TEST_CHECK(temporary);
  const fs::path root(temporary.get());
  details(root);
  boundsAndSidebar(root);
  providerShortcuts(root);
  settings(root);
  fs::remove_all(root);
  std::println("directory tree checks passed");
}
