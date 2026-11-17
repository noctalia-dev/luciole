#include "config/view_preference_writer.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  void testSaving() {
    namespace fs = std::filesystem;
    const luciole::GString temporary(g_dir_make_tmp("luciole-view-writer-XXXXXX", nullptr));
    TEST_CHECK(temporary != nullptr);
    const fs::path root(temporary.get());
    struct Cleanup {
      fs::path root;
      ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    const luciole::ConfigPaths paths{root / "config.toml", root / "settings.toml"};
    std::ofstream(paths.base) << "[browser]\nshow_hidden = true\nview_mode = 'list'\n";
    GlibPollSource source;
    luciole::ViewPreferenceWriter writer(paths);
    std::string error;
    writer.setChangedCallback([&](const std::string& message) { error = message; });
    const auto wait = [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (writer.busy()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        const int timeout = source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
        source.dispatch(fds, start);
      }
    };
    writer.save(luciole::ViewMode::Grid);
    writer.save(luciole::ViewMode::List);
    writer.save(luciole::ViewMode::Grid);
    writer.submit(
        {{luciole::BrowserPreference::ThumbnailPolicy, luciole::ThumbnailPolicy::Never},
         {luciole::BrowserPreference::ThumbnailCacheMib, 256}}
    );
    writer.submit(
        {{luciole::BrowserPreference::ThumbnailPolicy, luciole::ThumbnailPolicy::All},
         {luciole::BrowserPreference::ThumbnailCacheMib, 64}}
    );
    using P = luciole::BrowserPreference;
    writer.submit(
        {{P::Density, luciole::ViewDensity::Compact},
         {P::FilenameTruncation, luciole::FilenameTruncation::Middle},
         {P::ShowSizeCaption, false},
         {P::ShowEmblems, true},
         {P::ShowBackups, true},
         {P::SwitchToNewTab, true},
         {P::ConfirmPermanentDeletion, true},
         {P::SidebarTree, false},
         {P::ExpandableFolders, true}}
    );
    writer.submit(
        {{P::Density, luciole::ViewDensity::Comfortable},
         {P::FilenameTruncation, luciole::FilenameTruncation::End},
         {P::ShowSizeCaption, true},
         {P::ShowTypeCaption, true},
         {P::ShowModifiedCaption, true},
         {P::ShowEmblems, false},
         {P::ShowBackups, false},
         {P::SwitchToNewTab, false},
         {P::ConfirmPermanentDeletion, false},
         {P::SidebarTree, true},
         {P::ExpandableFolders, false}}
    );
    writer.saveGridIconSize(96);
    writer.saveGridIconSize(64);
    writer.saveSidebarWidth(320);
    writer.saveSidebarWidth(280);
    writer.saveShowSidebar(false);
    writer.saveShowSidebar(true);
    writer.saveShowSidebar(false);
    writer.saveShowHidden(false);
    writer.saveShowHidden(true);
    writer.saveShowHidden(false);
    writer.saveSort({luciole::SortField::Name, true, true});
    writer.saveSort({luciole::SortField::Size, false, true});
    const luciole::SortOptions finalSort{luciole::SortField::Type, true, false};
    writer.saveSort(finalSort);
    writer.saveDetailsColumns({true, false, true});
    writer.saveDetailsColumns({false, true, false});
    const luciole::DetailsColumnVisibility hiddenColumns{false, false, false};
    writer.saveDetailsColumns(hiddenColumns);
    writer.saveColumnOrder(
        {luciole::DetailsColumn::Size, luciole::DetailsColumn::Type, luciole::DetailsColumn::Modified,
         luciole::DetailsColumn::Accessed, luciole::DetailsColumn::Created, luciole::DetailsColumn::Owner,
         luciole::DetailsColumn::Group, luciole::DetailsColumn::Permissions, luciole::DetailsColumn::Location}
    );
    const luciole::DetailsColumnOrder finalOrder{luciole::DetailsColumn::Modified, luciole::DetailsColumn::Size,
                                                 luciole::DetailsColumn::Type,     luciole::DetailsColumn::Accessed,
                                                 luciole::DetailsColumn::Created,  luciole::DetailsColumn::Owner,
                                                 luciole::DetailsColumn::Group,    luciole::DetailsColumn::Permissions,
                                                 luciole::DetailsColumn::Location};
    writer.saveColumnOrder(finalOrder);
    writer.saveColumnWidths({400, 200, 100, 180, 156, 156, 128, 128, 112, 260});
    const luciole::DetailsColumnWidths finalWidths{640, 192, 96, 224, 156, 156, 128, 128, 112, 260};
    writer.saveColumnWidths(finalWidths);
    writer.saveInteraction(luciole::ActivationMode::SingleClick, luciole::TypingMode::Search);
    writer.saveInteraction(luciole::ActivationMode::DoubleClick, luciole::TypingMode::Typeahead);
    writer.saveInteraction(luciole::ActivationMode::SingleClick, luciole::TypingMode::Typeahead);
    wait();
    const auto loaded = luciole::loadSettings(paths);
    TEST_CHECK(
        error.empty()
        && loaded
        && loaded->browser.viewMode == luciole::ViewMode::Grid
        && !loaded->browser.showHidden
        && !loaded->browser.showBackups
        && !loaded->browser.switchToNewTab
        && !loaded->browser.confirmPermanentDeletion
        && loaded->browser.gridIconSize == 64
        && loaded->browser.thumbnails.policy == luciole::ThumbnailPolicy::All
        && loaded->browser.thumbnails.cacheMiB == 64
        && loaded->browser.sidebarWidth == 280
        && !loaded->browser.showSidebar
        && loaded->browser.sidebarTree
        && !loaded->browser.expandableFolders
        && loaded->browser.sort == finalSort
        && loaded->browser.columns == hiddenColumns
        && loaded->browser.columnOrder == finalOrder
        && loaded->browser.columnWidths == finalWidths
        && loaded->browser.activationMode == luciole::ActivationMode::SingleClick
        && loaded->browser.typingMode == luciole::TypingMode::Typeahead
        && loaded->browser.presentation
            == luciole::BrowsePresentation(
                luciole::ViewDensity::Comfortable, {true, true, true}, false, luciole::FilenameTruncation::End
            )
    );
    std::ifstream base(paths.base);
    const std::string content((std::istreambuf_iterator<char>(base)), {});
    TEST_CHECK(content == "[browser]\nshow_hidden = true\nview_mode = 'list'\n");
    writer.saveShowHidden(true);
    wait();
    const auto shown = luciole::loadSettings(paths);
    TEST_CHECK(
        error.empty()
        && shown
        && shown->browser.showHidden
        && shown->browser.gridIconSize == 64
        && !shown->browser.showSidebar
        && shown->browser.sort == finalSort
        && shown->browser.columns == hiddenColumns
        && shown->browser.columnOrder == finalOrder
        && shown->browser.columnWidths == finalWidths
        && shown->browser.activationMode == luciole::ActivationMode::SingleClick
        && shown->browser.typingMode == luciole::TypingMode::Typeahead
    );
    const luciole::SortOptions defaultSort;
    writer.saveSort(defaultSort);
    writer.saveDetailsColumns({});
    writer.saveColumnOrder(luciole::kDefaultDetailsColumnOrder);
    writer.saveColumnWidths(luciole::kDefaultDetailsColumnWidths);
    writer.saveInteraction(luciole::ActivationMode::DoubleClick, luciole::TypingMode::Search);
    wait();
    const auto reset = luciole::loadSettings(paths);
    TEST_CHECK(
        error.empty()
        && reset
        && reset->browser.sort == defaultSort
        && reset->browser.showHidden
        && reset->browser.gridIconSize == 64
        && reset->browser.columns == luciole::DetailsColumnVisibility{}
        && reset->browser.columnOrder == luciole::kDefaultDetailsColumnOrder
        && reset->browser.columnWidths == luciole::kDefaultDetailsColumnWidths
        && reset->browser.activationMode == luciole::ActivationMode::DoubleClick
        && reset->browser.typingMode == luciole::TypingMode::Search
    );
    std::ofstream(paths.overrides) << "[broken\n";
    writer.save(luciole::ViewMode::List);
    wait();
    TEST_CHECK(!error.empty());
    std::ifstream invalid(paths.overrides);
    TEST_CHECK(std::string((std::istreambuf_iterator<char>(invalid)), {}) == "[broken\n");
  }
  void testWatchingAndReset() {
    namespace fs = std::filesystem;
    using P = luciole::BrowserPreference;
    const luciole::GString temporary(g_dir_make_tmp("luciole-settings-watch-XXXXXX", nullptr));
    TEST_CHECK(temporary != nullptr);
    const fs::path root(temporary.get());
    struct Cleanup {
      fs::path root;
      ~Cleanup() {
        std::error_code ignored;
        fs::permissions(root / "declarative.toml", fs::perms::owner_all, ignored);
        fs::remove_all(root, ignored);
      }
    } cleanup{root};
    const luciole::ConfigPaths paths{root / "config/luciole/config.toml", root / "config/luciole/settings.toml"};
    GlibPollSource source;
    auto writer = std::make_unique<luciole::ViewPreferenceWriter>(paths);
    std::string error;
    int changes = 0;
    writer->setChangedCallback([&](const std::string& message) {
      error = message;
      ++changes;
    });
    const auto until = [&](const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        const int requested = source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        source.dispatch(fds, start);
      }
    };
    const auto write = [&](const fs::path& path, const std::string& content) {
      std::ofstream(root / "staged.toml") << content;
      fs::rename(root / "staged.toml", path);
    };
    writer->startWatching();
    until([&] { return changes > 0 && !writer->busy(); });
    TEST_CHECK(error.empty() && writer->settings() == luciole::Settings{});
    fs::create_directories(paths.base.parent_path());
    const std::string base = "[browser]\nshow_hidden = true\nview_mode = 'grid'\nsidebar_width = 240\n";
    write(root / "declarative.toml", base);
    fs::create_symlink(root / "declarative.toml", paths.base);
    fs::permissions(root / "declarative.toml", fs::perms::owner_read);
    until([&] { return writer->settings()->browser.showHidden && !writer->busy(); });
    TEST_CHECK(writer->inheritedSettings()->browser.sidebarWidth == 240);
    writer->submit({{P::ShowHidden, false}, {P::ViewMode, luciole::ViewMode::List}});
    writer->saveGridIconSize(128);
    writer->reload();
    until([&] { return !writer->busy(); });
    TEST_CHECK(
        error.empty()
        && !writer->settings()->browser.showHidden
        && writer->settings()->browser.gridIconSize == 128
        && writer->settings()->browser.viewMode == luciole::ViewMode::List
        && writer->inheritedSettings()->browser.showHidden
    );
    writer->submit({{P::ShowHidden, std::nullopt}});
    until([&] { return !writer->busy(); });
    TEST_CHECK(writer->settings()->browser.showHidden && writer->settings()->browser.gridIconSize == 128);
    const auto good = writer->settings();
    const auto previousChanges = changes;
    const std::string invalid = "[browser]\ngrid_icon_size = 45\n";
    write(paths.overrides, invalid);
    until([&] { return changes > previousChanges && !error.empty() && !writer->busy(); });
    TEST_CHECK(writer->settings() == good);
    writer->saveGridIconSize(64);
    until([&] { return !writer->busy(); });
    std::ifstream broken(paths.overrides);
    TEST_CHECK(std::string((std::istreambuf_iterator<char>(broken)), {}) == invalid);
    write(paths.overrides, "[browser]\ngrid_icon_size = 96\nshow_sidebar = false\n");
    until([&] { return writer->settings()->browser.gridIconSize == 96 && error.empty() && !writer->busy(); });
    TEST_CHECK(!writer->settings()->browser.showSidebar);
    fs::permissions(paths.overrides, fs::perms::owner_read);
    writer->saveShowSidebar(true);
    until([&] { return !writer->busy(); });
    TEST_CHECK(!error.empty() && !writer->settings()->browser.showSidebar);
    fs::permissions(paths.overrides, fs::perms::owner_read | fs::perms::owner_write);
    writer->resetAll();
    until([&] { return !writer->busy(); });
    TEST_CHECK(error.empty() && writer->settings() == writer->inheritedSettings());
    std::ifstream original(root / "declarative.toml");
    TEST_CHECK(std::string((std::istreambuf_iterator<char>(original)), {}) == base && fs::is_symlink(paths.base));
    write(root / "declarative.toml", "[browser]\nshow_hidden = false\nsidebar_width = 320\n");
    until([&] {
      return writer->settings()->browser.sidebarWidth == 320
          && !writer->settings()->browser.showHidden
          && !writer->busy();
    });
    fs::remove(paths.base);
    fs::remove(paths.overrides);
    until([&] { return writer->settings() == luciole::Settings{} && !writer->busy(); });
    write(paths.overrides, "[browser]\nview_mode = 'grid'\n");
    until([&] { return writer->settings()->browser.viewMode == luciole::ViewMode::Grid && !writer->busy(); });
    fs::rename(paths.base.parent_path(), root / "old-directory");
    until([&] { return writer->settings() == luciole::Settings{} && !writer->busy(); });
    fs::rename(root / "old-directory", paths.base.parent_path());
    until([&] { return writer->settings()->browser.viewMode == luciole::ViewMode::Grid && !writer->busy(); });
    write(paths.overrides, "[browser]\nview_mode = 'grid'\n");
    writer->reload();
    writer.reset();
    const auto drain = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    until([&] { return std::chrono::steady_clock::now() >= drain; });
    writer = std::make_unique<luciole::ViewPreferenceWriter>(paths);
    writer->setChangedCallback([&](const std::string&) { writer.reset(); });
    writer->startWatching();
    until([&] { return !writer; });
    const auto afterDestruction = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    until([&] { return std::chrono::steady_clock::now() >= afterDestruction; });
  }

} // namespace

int main() {
  try {
    testSaving();
    testWatchingAndReset();
    std::println("view preference writer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "view preference writer test: {}", error.what());
    return 1;
  }
}
