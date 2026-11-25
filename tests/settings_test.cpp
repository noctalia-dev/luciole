#include "browser/directory_model.h"
#include "config/settings.h"
#include "fs/gio_ptr.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <toml++/toml.hpp>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  struct Fixture {
    std::filesystem::path directory;
    Fixture() {
      const luciole::GString name(g_dir_make_tmp("luciole-settings-test-XXXXXX", nullptr));
      TEST_CHECK(name != nullptr);
      directory = name.get();
    }
    ~Fixture() {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(const std::string& name, const std::string& text) const {
      std::ofstream file(directory / name, std::ios::binary);
      file << text;
      TEST_CHECK(file.good());
    }
    luciole::ConfigPaths paths() const {
      return {.base = directory / "config.toml", .overrides = directory / "settings.toml"};
    }
  };

  std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  void testBackgroundOpacity() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && service.settings().browser.backgroundOpacity == 1.0);
    fixture.write("config.toml", "[browser]\nbackground_opacity = 0\n");
    TEST_CHECK(service.reload() && service.settings().browser.backgroundOpacity == 0.0);
    fixture.write("config.toml", "[browser]\nbackground_opacity = 0.4\n");
    TEST_CHECK(service.reload() && service.settings().browser.backgroundOpacity == 0.4);
    const std::array edit{luciole::OverrideEdit{luciole::BrowserPreference::BackgroundOpacity, 0.6}};
    TEST_CHECK(service.saveOverrides(edit) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(service.reload() && service.settings().browser.backgroundOpacity == 0.6);
    TEST_CHECK(read(fixture.paths().overrides).contains("background_opacity = 0.6\n"));
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::BackgroundOpacity, std::nullopt}};
    TEST_CHECK(service.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(service.reload() && service.settings().browser.backgroundOpacity == 0.4);
    for (const auto* value : {"-0.1", "1.5", "nan", "true", "'0.5'"}) {
      fixture.write("config.toml", std::format("[browser]\nbackground_opacity = {}\n", value));
      TEST_CHECK(!service.reload() && service.settings().browser.backgroundOpacity == 0.4);
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
  }

  void testCornerRadiusScale() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && service.settings().browser.cornerRadiusScale == 1.0);
    fixture.write("config.toml", "[browser]\ncorner_radius_scale = 0\n");
    TEST_CHECK(service.reload() && service.settings().browser.cornerRadiusScale == 0.0);
    fixture.write("config.toml", "[browser]\ncorner_radius_scale = 2\n");
    TEST_CHECK(service.reload() && service.settings().browser.cornerRadiusScale == 2.0);
    const std::array edit{luciole::OverrideEdit{luciole::BrowserPreference::CornerRadiusScale, 1.5}};
    TEST_CHECK(service.saveOverrides(edit) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(service.reload() && service.settings().browser.cornerRadiusScale == 1.5);
    TEST_CHECK(read(fixture.paths().overrides).contains("corner_radius_scale = 1.5\n"));
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::CornerRadiusScale, std::nullopt}};
    TEST_CHECK(service.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(service.reload() && service.settings().browser.cornerRadiusScale == 2.0);
    for (const auto* value : {"-0.1", "2.5", "nan", "true", "'1'"}) {
      fixture.write("config.toml", std::format("[browser]\ncorner_radius_scale = {}\n", value));
      TEST_CHECK(!service.reload() && service.settings().browser.cornerRadiusScale == 2.0);
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
  }

  void testTerminalCommand() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && service.settings().browser.terminalCommand.empty());
    fixture.write("config.toml", "[browser]\nterminal_command = 'foot --app-id=Luciole'\n");
    TEST_CHECK(service.reload() && service.settings().browser.terminalCommand == "foot --app-id=Luciole");
    const std::array edit{luciole::OverrideEdit{luciole::BrowserPreference::TerminalCommand, std::string("kitty")}};
    TEST_CHECK(service.saveOverrides(edit) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(service.reload() && service.settings().browser.terminalCommand == "kitty");
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::TerminalCommand, std::nullopt}};
    TEST_CHECK(service.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(service.reload() && service.settings().browser.terminalCommand == "foot --app-id=Luciole");
    fixture.write("config.toml", "[browser]\nterminal_command = 42\n");
    TEST_CHECK(!service.reload() && service.settings().browser.terminalCommand == "foot --app-id=Luciole");
    fixture.write("config.toml", "[browser]\nterminal_command = \"foot\\nkitty\"\n");
    TEST_CHECK(!service.reload());
  }
  void testNewTabPreference() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && service.settings().browser.switchToNewTab);
    fixture.write("config.toml", "[browser]\nswitch_to_new_tab = false\n");
    TEST_CHECK(service.reload() && !service.settings().browser.switchToNewTab);
    const std::array on{luciole::OverrideEdit{luciole::BrowserPreference::SwitchToNewTab, true}};
    TEST_CHECK(service.saveOverrides(on) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && restored.settings().browser.switchToNewTab);
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::SwitchToNewTab, std::nullopt}};
    TEST_CHECK(restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(restored.reload() && !restored.settings().browser.switchToNewTab);
    fixture.write("config.toml", "[browser]\nswitch_to_new_tab = 'false'\n");
    TEST_CHECK(!restored.reload() && !restored.settings().browser.switchToNewTab);
    fixture.write("config.toml", "[browser]\nswitch_to_new_tab = true\n");
    TEST_CHECK(restored.reload() && restored.settings().browser.switchToNewTab);
  }
  void testSingleClickLocationEditPreference() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && !service.settings().browser.singleClickLocationEdit);
    fixture.write("config.toml", "[browser]\nsingle_click_location_edit = true\n");
    TEST_CHECK(service.reload() && service.settings().browser.singleClickLocationEdit);
    const std::array off{luciole::OverrideEdit{luciole::BrowserPreference::SingleClickLocationEdit, false}};
    TEST_CHECK(service.saveOverrides(off) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && !restored.settings().browser.singleClickLocationEdit);
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::SingleClickLocationEdit, std::nullopt}};
    TEST_CHECK(restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(restored.reload() && restored.settings().browser.singleClickLocationEdit);
    fixture.write("config.toml", "[browser]\nsingle_click_location_edit = 'true'\n");
    TEST_CHECK(!restored.reload() && restored.settings().browser.singleClickLocationEdit);
    fixture.write("config.toml", "[browser]\nsingle_click_location_edit = false\n");
    TEST_CHECK(restored.reload() && !restored.settings().browser.singleClickLocationEdit);
  }
  void testDeletionConfirmationPreference() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && service.settings().browser.confirmPermanentDeletion);
    fixture.write("config.toml", "[browser]\nconfirm_permanent_deletion = false\n");
    TEST_CHECK(service.reload() && !service.settings().browser.confirmPermanentDeletion);
    const std::array on{luciole::OverrideEdit{luciole::BrowserPreference::ConfirmPermanentDeletion, true}};
    TEST_CHECK(service.saveOverrides(on) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && restored.settings().browser.confirmPermanentDeletion);
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::ConfirmPermanentDeletion, std::nullopt}};
    TEST_CHECK(restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(restored.reload() && !restored.settings().browser.confirmPermanentDeletion);
    fixture.write("config.toml", "[browser]\nconfirm_permanent_deletion = 'false'\n");
    TEST_CHECK(!restored.reload() && !restored.settings().browser.confirmPermanentDeletion);
    fixture.write("config.toml", "[browser]\nconfirm_permanent_deletion = true\n");
    TEST_CHECK(restored.reload() && restored.settings().browser.confirmPermanentDeletion);
  }
  void testPermanentDeletionMenuPreference() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && !service.settings().browser.showPermanentDeletion);
    fixture.write("config.toml", "[browser]\nshow_permanent_deletion = true\n");
    TEST_CHECK(service.reload() && service.settings().browser.showPermanentDeletion);
    const std::array off{luciole::OverrideEdit{luciole::BrowserPreference::ShowPermanentDeletion, false}};
    TEST_CHECK(service.saveOverrides(off) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && !restored.settings().browser.showPermanentDeletion);
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::ShowPermanentDeletion, std::nullopt}};
    TEST_CHECK(restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(restored.reload() && restored.settings().browser.showPermanentDeletion);
    fixture.write("config.toml", "[browser]\nshow_permanent_deletion = 'true'\n");
    TEST_CHECK(!restored.reload() && restored.settings().browser.showPermanentDeletion);
    fixture.write("config.toml", "[browser]\nshow_permanent_deletion = false\n");
    TEST_CHECK(restored.reload() && !restored.settings().browser.showPermanentDeletion);
  }
  void testRestoreLastWindowPreference() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && !service.settings().browser.restoreLastWindow);
    fixture.write("config.toml", "[browser]\nrestore_last_window = true\n");
    TEST_CHECK(service.reload() && service.settings().browser.restoreLastWindow);
    const std::array off{luciole::OverrideEdit{luciole::BrowserPreference::RestoreLastWindow, false}};
    TEST_CHECK(service.saveOverrides(off) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && !restored.settings().browser.restoreLastWindow);
  }
  void testSingleWindowPreference() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && !service.settings().browser.singleWindow);
    fixture.write("config.toml", "[browser]\nsingle_window = true\n");
    TEST_CHECK(service.reload() && service.settings().browser.singleWindow);
    const std::array off{luciole::OverrideEdit{luciole::BrowserPreference::SingleWindow, false}};
    TEST_CHECK(service.saveOverrides(off) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && !restored.settings().browser.singleWindow);
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::SingleWindow, std::nullopt}};
    TEST_CHECK(restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(restored.reload() && restored.settings().browser.singleWindow);
    fixture.write("config.toml", "[browser]\nsingle_window = 'true'\n");
    TEST_CHECK(!restored.reload() && restored.settings().browser.singleWindow);
    fixture.write("config.toml", "[browser]\nsingle_window = false\n");
    TEST_CHECK(restored.reload() && !restored.settings().browser.singleWindow);
  }
  void testBackupPreference() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    TEST_CHECK(service.settings().browser.showBackups && !service.settings().browser.showHidden);
    fixture.write("config.toml", "[browser]\nshow_backups = false\nshow_hidden = true\n");
    TEST_CHECK(service.reload() && !service.settings().browser.showBackups && service.settings().browser.showHidden);
    const std::array on{luciole::OverrideEdit{luciole::BrowserPreference::ShowBackups, true}};
    TEST_CHECK(service.saveOverrides(on) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload());
    TEST_CHECK(restored.settings().browser.showBackups && restored.settings().browser.showHidden);
    const std::array off{luciole::OverrideEdit{luciole::BrowserPreference::ShowBackups, false}};
    TEST_CHECK(restored.saveOverrides(off) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(!restored.settings().browser.showBackups && restored.settings().browser.showHidden);
    const std::array reset{luciole::OverrideEdit{luciole::BrowserPreference::ShowBackups, std::nullopt}};
    TEST_CHECK(restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(!restored.settings().browser.showBackups && restored.settings().browser.showHidden);
    TEST_CHECK(!read(fixture.paths().overrides).contains("show_backups"));
    for (const auto* value : {"'false'", "0", "[]"}) {
      fixture.write("config.toml", std::format("[browser]\nshow_backups = {}\n", value));
      TEST_CHECK(!restored.reload() && !restored.settings().browser.showBackups);
    }
  }

  void testFilenameTruncation() {
    using Mode = luciole::FilenameTruncation;
    using State = luciole::AtomicWriteState;
    const auto key = luciole::BrowserPreference::FilenameTruncation;
    Fixture fixture;
    luciole::ConfigService settings(fixture.paths());
    TEST_CHECK(settings.reload() && settings.settings().browser.presentation.filenameTruncation == Mode::Middle);
    fixture.write("config.toml", "[browser]\nfilename_truncation = 'end'\n");
    TEST_CHECK(settings.reload() && settings.settings().browser.presentation.filenameTruncation == Mode::End);
    const std::array middle{luciole::OverrideEdit{key, Mode::Middle}};
    TEST_CHECK(settings.saveOverrides(middle) == State::Durable);
    luciole::ConfigService restarted(fixture.paths());
    TEST_CHECK(restarted.reload() && restarted.settings().browser.presentation.filenameTruncation == Mode::Middle);
    const auto saved = read(fixture.paths().overrides);
    TEST_CHECK(saved.contains("filename_truncation = \"middle\""));
    const std::array invalid{luciole::OverrideEdit{key, static_cast<Mode>(255)}};
    TEST_CHECK(restarted.saveOverrides(invalid) == State::NotCommitted && read(fixture.paths().overrides) == saved);
    const std::array reset{luciole::OverrideEdit{key, std::nullopt}};
    TEST_CHECK(restarted.saveOverrides(reset) == State::Durable);
    TEST_CHECK(restarted.settings().browser.presentation.filenameTruncation == Mode::End);
    TEST_CHECK(!read(fixture.paths().overrides).contains("filename_truncation"));
    for (const auto* value : {"'start'", "'Middle'", "true", "1", "[]"}) {
      fixture.write("config.toml", std::format("[browser]\nfilename_truncation = {}\n", value));
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
      TEST_CHECK(!restarted.reload() && restarted.settings().browser.presentation.filenameTruncation == Mode::End);
    }
    fixture.write("config.toml", "[browser]\nfilename_truncation = 'middle'\n");
    TEST_CHECK(restarted.reload() && restarted.settings().browser.presentation.filenameTruncation == Mode::Middle);
  }

  void testDropAction() {
    using Action = luciole::DropAction;
    using State = luciole::AtomicWriteState;
    const auto key = luciole::BrowserPreference::DropAction;
    Fixture fixture;
    luciole::ConfigService settings(fixture.paths());
    TEST_CHECK(settings.reload() && settings.settings().browser.dropAction == Action::Menu);
    fixture.write("config.toml", "[browser]\ndrop_action = 'copy'\n");
    TEST_CHECK(settings.reload() && settings.settings().browser.dropAction == Action::Copy);
    const std::array menu{luciole::OverrideEdit{key, Action::Menu}};
    TEST_CHECK(settings.saveOverrides(menu) == State::Durable);
    luciole::ConfigService restarted(fixture.paths());
    TEST_CHECK(restarted.reload() && restarted.settings().browser.dropAction == Action::Menu);
    const auto saved = read(fixture.paths().overrides);
    TEST_CHECK(saved.contains("drop_action = \"menu\""));
    const std::array invalid{luciole::OverrideEdit{key, static_cast<Action>(255)}};
    TEST_CHECK(restarted.saveOverrides(invalid) == State::NotCommitted && read(fixture.paths().overrides) == saved);
    const std::array reset{luciole::OverrideEdit{key, std::nullopt}};
    TEST_CHECK(restarted.saveOverrides(reset) == State::Durable);
    TEST_CHECK(restarted.settings().browser.dropAction == Action::Copy);
    TEST_CHECK(!read(fixture.paths().overrides).contains("drop_action"));
    for (const auto* value : {"'ask'", "'copy_here'", "true", "1", "[]"}) {
      fixture.write("config.toml", std::format("[browser]\ndrop_action = {}\n", value));
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
      TEST_CHECK(!restarted.reload() && restarted.settings().browser.dropAction == Action::Copy);
    }
    fixture.write("config.toml", "[browser]\ndrop_action = 'move'\n");
    TEST_CHECK(restarted.reload() && restarted.settings().browser.dropAction == Action::Move);
  }

  void testPresentationSettings() {
    using P = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService settings(fixture.paths());
    TEST_CHECK(settings.reload());
    TEST_CHECK(settings.settings().browser.presentation == luciole::BrowsePresentation{});
    fixture.write(
        "config.toml",
        "[browser]\ndensity = 'comfortable'\nshow_type_caption = true\nshow_emblems = false\nshow_hidden = true\n"
    );
    TEST_CHECK(settings.reload());
    TEST_CHECK(settings.settings().browser.presentation.density == luciole::ViewDensity::Comfortable);
    TEST_CHECK(settings.settings().browser.presentation.captions.type);
    const std::array edits{
        luciole::OverrideEdit{P::Density, luciole::ViewDensity::Compact},
        luciole::OverrideEdit{P::ShowSizeCaption, true}, luciole::OverrideEdit{P::ShowTypeCaption, false},
        luciole::OverrideEdit{P::ShowModifiedCaption, true}, luciole::OverrideEdit{P::ShowEmblems, true}
    };
    TEST_CHECK(settings.saveOverrides(edits) == State::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload());
    const luciole::BrowsePresentation expected{luciole::ViewDensity::Compact, {true, false, true}, true};
    TEST_CHECK(restored.settings().browser.presentation == expected && restored.settings().browser.showHidden);
    const auto before = read(fixture.paths().overrides);
    const std::array invalid{luciole::OverrideEdit{P::Density, static_cast<luciole::ViewDensity>(255)}};
    TEST_CHECK(restored.saveOverrides(invalid) == State::NotCommitted);
    TEST_CHECK(read(fixture.paths().overrides) == before);
    const std::array reset{
        luciole::OverrideEdit{P::Density, std::nullopt}, luciole::OverrideEdit{P::ShowSizeCaption, std::nullopt},
        luciole::OverrideEdit{P::ShowTypeCaption, std::nullopt},
        luciole::OverrideEdit{P::ShowModifiedCaption, std::nullopt}, luciole::OverrideEdit{P::ShowEmblems, std::nullopt}
    };
    TEST_CHECK(restored.saveOverrides(reset) == State::Durable);
    const luciole::BrowsePresentation inherited{luciole::ViewDensity::Comfortable, {false, true, false}, false};
    TEST_CHECK(restored.settings().browser.presentation == inherited && restored.settings().browser.showHidden);
    for (const auto* invalidValue :
         {"density = 'large'", "density = true", "show_size_caption = 'true'", "show_type_caption = 1",
          "show_modified_caption = []", "show_emblems = 'false'"}) {
      fixture.write("config.toml", std::format("[browser]\n{}\n", invalidValue));
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
      TEST_CHECK(!restored.reload() && restored.settings().browser.presentation == inherited);
    }
  }

  void testMillerSettings() {
    Fixture fixture;
    fixture.write("config.toml", "[browser]\nview_mode = 'miller'\nmiller_column_width = 300\n");
    luciole::ConfigService settings(fixture.paths());
    TEST_CHECK(
        settings.reload()
        && settings.settings().browser.viewMode == luciole::ViewMode::Miller
        && settings.settings().browser.millerColumnWidth == 300
    );
    using P = luciole::BrowserPreference;
    const std::array edits{
        luciole::OverrideEdit{P::ViewMode, luciole::ViewMode::Miller}, luciole::OverrideEdit{P::MillerColumnWidth, 417}
    };
    TEST_CHECK(settings.saveOverrides(edits) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(
        restored.reload()
        && restored.settings().browser.millerColumnWidth == 417
        && restored.settings().browser.viewMode == luciole::ViewMode::Miller
    );
    const auto before = read(fixture.paths().overrides);
    for (const int width : {179, 801}) {
      const std::array invalid{luciole::OverrideEdit{P::MillerColumnWidth, width}};
      TEST_CHECK(
          restored.saveOverrides(invalid) == luciole::AtomicWriteState::NotCommitted
          && read(fixture.paths().overrides) == before
      );
      fixture.write("config.toml", std::format("[browser]\nmiller_column_width = {}\n", width));
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "[browser]\nview_mode = 'miller'\nmiller_column_width = 300\n");
    const std::array reset{luciole::OverrideEdit{P::MillerColumnWidth, std::nullopt}};
    TEST_CHECK(
        restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable
        && restored.settings().browser.millerColumnWidth == 300
    );
  }

  void testMillerParents() {
    Fixture fixture;
    luciole::ConfigService settings(fixture.paths());
    TEST_CHECK(settings.reload() && settings.settings().browser.millerShowParents);
    using P = luciole::BrowserPreference;
    const std::array off{luciole::OverrideEdit{P::MillerShowParents, false}};
    TEST_CHECK(settings.saveOverrides(off) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && !restored.settings().browser.millerShowParents);
    fixture.write("config.toml", "[browser]\nmiller_show_parents = false\n");
    const std::array on{luciole::OverrideEdit{P::MillerShowParents, true}};
    TEST_CHECK(
        restored.saveOverrides(on) == luciole::AtomicWriteState::Durable
        && restored.settings().browser.millerShowParents
    );
    const std::array reset{luciole::OverrideEdit{P::MillerShowParents, std::nullopt}};
    TEST_CHECK(
        restored.saveOverrides(reset) == luciole::AtomicWriteState::Durable
        && !restored.settings().browser.millerShowParents
    );
    fixture.write("config.toml", "[browser]\nmiller_show_parents = 'true'\n");
    TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
  }

  void testSchemaAndDiagnostics() {
    Fixture fixture;
    fixture.write(
        "config.toml",
        "schema_version = 1\n[browser]\nshow_hidden = true\nsort_by = 'modified'\n"
        "sort_descending = true\ndirectories_first = false\nview_mode = 'grid'\n"
    );
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
    fixture.write("settings.toml", "invalid adjacent override");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
    fixture.write("config.toml", "unexpected = 1\n[browser]\nshow_hidden = 1\nsort_by = 'date'\nunknown = false\n");
    const auto errors = luciole::validateConfigFile(fixture.paths().base);
    TEST_CHECK(errors.size() == 4);
    bool foundLine = false;
    for (const auto& error : errors) {
      TEST_CHECK(error.file == fixture.paths().base && error.line > 0 && error.column > 0);
      if (error.key == "browser.show_hidden") {
        foundLine = error.line == 3 && error.format().contains("config.toml:3:") && error.message.contains("boolean");
      }
    }
    TEST_CHECK(foundLine);
    for (const auto& invalid :
         {"schema_version = 2", "schema_version = true", "schema_version = 1.0", "browser = false",
          "[browser]\nshow_hidden = 'true'", "[browser]\nsort_by = 1", "[browser]\nview_mode = true",
          "[browser]\nview_mode = 'icons'", "[browser]\nshow_hidden = true\nshow_hidden = false", "broken = [",
          "theme = {}"}) {
      fixture.write("config.toml", invalid);
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "");
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().base).empty());
    fixture.write("config.toml", "[browser]\nsort_by = 'type'\n");
    std::filesystem::remove(fixture.paths().overrides);
    const auto typed = luciole::loadSettings(fixture.paths());
    TEST_CHECK(typed && typed->browser.sort.field == luciole::SortField::Type);
  }

  void testUnknownSettings() {
    Fixture fixture;
    const std::string base = "schema_version = 1\nfuture_root = ['keep', 'these']\n[browser]\n"
                             "sort_by = 'size'\nshow_hidden = true\nfuture_setting = { value = 17 }\n";
    const std::string overrides =
        "schema_version = 1\n[future_section]\nenabled = true\n[browser]\n"
        "show_hidden = false\nshow_tab_bar_with_single_tab = true\nfuture_array = [1, 2, 3]\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", overrides);
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.issues().empty());
    TEST_CHECK(
        !config.settings().browser.showHidden && config.settings().browser.sort.field == luciole::SortField::Size
    );
    TEST_CHECK(read(fixture.paths().base) == base && read(fixture.paths().overrides) == overrides);
    const auto inherited = luciole::loadInheritedSettings(fixture.paths());
    TEST_CHECK(inherited && inherited->browser.showHidden);
    const auto diagnostics = luciole::validateConfigFile(fixture.paths().overrides);
    TEST_CHECK(diagnostics.size() == 3);
    TEST_CHECK(std::ranges::any_of(diagnostics, [](const auto& issue) {
      return issue.key == "browser.show_tab_bar_with_single_tab" && issue.line > 0 && issue.column > 0;
    }));
    const std::array edit{luciole::OverrideEdit{luciole::BrowserPreference::GridIconSize, 96}};
    TEST_CHECK(config.saveOverrides(edit) == luciole::AtomicWriteState::Durable);
    const auto saved = toml::parse(read(fixture.paths().overrides));
    TEST_CHECK(saved["browser"]["show_tab_bar_with_single_tab"].value<bool>() == true);
    TEST_CHECK(saved["future_section"]["enabled"].value<bool>() == true);
    TEST_CHECK(saved["browser"]["future_array"].as_array()->size() == 3);
    TEST_CHECK(saved["browser"]["future_array"][2].value<int>() == 3);
    const auto bytes = read(fixture.paths().overrides);
    TEST_CHECK(config.saveOverrides(edit) == luciole::AtomicWriteState::Unchanged);
    TEST_CHECK(read(fixture.paths().overrides) == bytes && read(fixture.paths().base) == base);
    TEST_CHECK(config.saveOverrides(luciole::inheritAllPreferences()) == luciole::AtomicWriteState::Durable);
    const auto reset = toml::parse(read(fixture.paths().overrides));
    TEST_CHECK(!reset["browser"]["grid_icon_size"] && !reset["browser"]["show_hidden"]);
    TEST_CHECK(reset["browser"]["show_tab_bar_with_single_tab"].value<bool>() == true);
    TEST_CHECK(reset["future_section"]["enabled"].value<bool>() == true);
    TEST_CHECK(config.reload() && config.settings().browser.showHidden);
    const auto lastGood = config.settings();
    fixture.write("settings.toml", "[browser]\nshow_tab_bar_with_single_tab = true\nshow_hidden = 42\n");
    TEST_CHECK(!config.reload() && config.settings() == lastGood);
    TEST_CHECK(config.issues().size() == 1 && config.issues().front().key == "browser.show_hidden");
    fixture.write("settings.toml", "[browser]\nshow_tab_bar_with_single_tab = true\nshow_hidden = false\n");
    TEST_CHECK(config.reload() && !config.settings().browser.showHidden && config.issues().empty());
  }

  void testLayersAndReload() {
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings() == luciole::Settings{});
    fixture.write(
        "config.toml", "[browser]\nshow_hidden = true\nsort_by = 'size'\nsort_descending = true\nview_mode = 'grid'\n"
    );
    TEST_CHECK(config.reload() && config.settings().browser.viewMode == luciole::ViewMode::Grid);
    fixture.write("settings.toml", "[browser]\nshow_hidden = false\ndirectories_first = false\nview_mode = 'list'\n");
    TEST_CHECK(config.reload());
    const auto& browser = config.settings().browser;
    TEST_CHECK(
        !browser.showHidden
        && browser.sort.field == luciole::SortField::Size
        && browser.sort.descending
        && !browser.sort.directoriesFirst
        && browser.viewMode == luciole::ViewMode::List
    );
    const auto lastGood = config.settings();
    fixture.write("settings.toml", "[browser]\nview_mode = 'unknown'\n");
    TEST_CHECK(!config.reload() && config.settings() == lastGood && !config.issues().empty());
    fixture.write("config.toml", "[browser]\nshow_hidden = 'not-a-boolean'\n");
    fixture.write("settings.toml", "[browser]\nshow_hidden = true\n");
    TEST_CHECK(!config.reload() && config.settings() == lastGood);
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::remove(fixture.paths().overrides);
    TEST_CHECK(config.reload() && config.issues().empty() && config.settings() == luciole::Settings{});
  }

  void testFilesAndLimits() {
    Fixture fixture;
    TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    fixture.write("target.toml", "[browser]\nshow_hidden = true\n");
    const auto target = fixture.directory / "target.toml";
    std::filesystem::create_symlink(target, fixture.paths().base);
    TEST_CHECK(::chmod(target.c_str(), 0444) == 0);
    luciole::ConfigService config(fixture.paths());
    const auto original = read(target);
    TEST_CHECK(config.reload() && config.settings().browser.showHidden);
    TEST_CHECK(read(target) == original && std::filesystem::is_symlink(fixture.paths().base));
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::create_symlink(fixture.directory / "missing-target", fixture.paths().base);
    TEST_CHECK(!config.reload());
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::create_directory(fixture.paths().base);
    TEST_CHECK(!config.reload());
    std::filesystem::remove(fixture.paths().base);
    TEST_CHECK(::mkfifo(fixture.paths().base.c_str(), 0600) == 0);
    TEST_CHECK(!config.reload());
    std::filesystem::remove(fixture.paths().base);
    std::string bounded(256 * 1024, '#');
    fixture.write("config.toml", bounded);
    TEST_CHECK(config.reload());
    bounded.push_back('#');
    fixture.write("config.toml", bounded);
    TEST_CHECK(!config.reload() && config.issues().front().message.contains("256 KiB"));
    if (::geteuid() != 0) {
      fixture.write("config.toml", "");
      TEST_CHECK(::chmod(fixture.paths().base.c_str(), 0000) == 0);
      const bool loaded = config.reload();
      ::chmod(fixture.paths().base.c_str(), 0600);
      TEST_CHECK(!loaded);
    }
  }

  void testEnvironmentPaths() {
    const char* previous = std::getenv("XDG_CONFIG_HOME");
    const std::optional<std::string> saved = previous != nullptr ? std::optional<std::string>(previous) : std::nullopt;
    ::setenv("XDG_CONFIG_HOME", "/tmp/luciole-settings-path-test", 1);
    const auto paths = luciole::ConfigPaths::fromEnvironment();
    TEST_CHECK(
        paths.base == "/tmp/luciole-settings-path-test/luciole/config.toml"
        && paths.overrides == "/tmp/luciole-settings-path-test/luciole/settings.toml"
    );
    ::setenv("XDG_CONFIG_HOME", "relative-invalid-path", 1);
    bool rejected = false;
    try {
      (void)luciole::ConfigPaths::fromEnvironment();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
    ::unsetenv("XDG_CONFIG_HOME");
    const auto fallback = luciole::ConfigPaths::fromEnvironment();
    TEST_CHECK(fallback.base == std::filesystem::path(std::getenv("HOME")) / ".config/luciole/config.toml");
    if (saved) {
      ::setenv("XDG_CONFIG_HOME", saved->c_str(), 1);
    }
  }

  void testBrowserDefaults() {
    Fixture fixture;
    fixture.write(
        "config.toml",
        "[browser]\nshow_hidden = true\nsort_by = 'size'\nsort_descending = true\n"
        "directories_first = false\nhidden_last = true\n"
    );
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(loaded.has_value());
    luciole::DirectoryModel model;
    model.setSort(loaded->browser.sort);
    model.setFilter({.showHidden = loaded->browser.showHidden, .text = {}});
    luciole::DirectorySnapshot data;
    data.location = luciole::Location::fromPath(fixture.directory.native());
    data.generation = 1;
    data.status = luciole::DirectoryStatus::Ready;
    data.entries = {
        {.id = 1,
         .place = luciole::Location::fromPath((fixture.directory / ".hidden").native()),
         .name = ".hidden",
         .hidden = true,
         .size = 20,
         .modifiedSeconds = std::nullopt},
        {.id = 2,
         .place = luciole::Location::fromPath((fixture.directory / "visible").native()),
         .name = "visible",
         .size = 5,
         .modifiedSeconds = std::nullopt},
    };
    model.applySnapshot(std::move(data));
    TEST_CHECK((model.rows() == std::vector<luciole::EntryId>{2, 1}));
  }

  void testGridIconSize() {
    Fixture fixture;
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    for (const int size : luciole::kGridIconSizes) {
      fixture.write("config.toml", "[browser]\ngrid_icon_size = " + std::to_string(size) + "\n");
      const auto loaded = luciole::loadSettings(fixture.paths());
      TEST_CHECK(loaded && loaded->browser.gridIconSize == size);
    }
    for (const auto* value : {"0", "31", "49", "129", "257", "-32", "64.0", "true", "'64'", "9223372036854775807"}) {
      fixture.write("config.toml", std::string("[browser]\ngrid_icon_size = ") + value + "\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "[browser]\ngrid_icon_size = 64\n");
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 96\nshow_hidden = true\n");
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.gridIconSize == 96);
    for (const int size : {32, 256}) {
      const std::array zoom{luciole::OverrideEdit{Preference::GridIconSize, size}};
      TEST_CHECK(config.saveOverrides(zoom) == State::Durable);
      const auto loaded = luciole::loadSettings(fixture.paths());
      TEST_CHECK(loaded && loaded->browser.gridIconSize == size && loaded->browser.showHidden);
    }
    const auto previous = read(fixture.paths().overrides);
    const std::array invalid{luciole::OverrideEdit{Preference::GridIconSize, 33}};
    TEST_CHECK(config.saveOverrides(invalid) == State::NotCommitted && read(fixture.paths().overrides) == previous);
    const std::array reset{luciole::OverrideEdit{Preference::GridIconSize, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(reset) == State::Durable
        && config.settings().browser.gridIconSize == 64
        && !read(fixture.paths().overrides).contains("grid_icon_size")
    );
  }

  void testRowIconSizes() {
    using Preference = luciole::BrowserPreference;
    struct Case {
      std::string key;
      luciole::PreferenceKey<int> preference;
      int luciole::BrowserSettings::* field;
    };
    for (const auto& [key, preference, field] : {
             Case{"list_icon_size", Preference::ListIconSize, &luciole::BrowserSettings::listIconSize},
             Case{"miller_icon_size", Preference::MillerIconSize, &luciole::BrowserSettings::millerIconSize},
         }) {
      Fixture fixture;
      fixture.write("config.toml", "");
      const auto defaults = luciole::loadSettings(fixture.paths());
      TEST_CHECK(defaults && (defaults->browser.*field) == 0);
      for (const int size : luciole::kRowIconSizes) {
        fixture.write("config.toml", "[browser]\n" + key + " = " + std::to_string(size) + "\n");
        const auto loaded = luciole::loadSettings(fixture.paths());
        TEST_CHECK(loaded && (loaded->browser.*field) == size);
      }
      for (const auto* value : {"15", "21", "65", "-16", "24.0", "true", "'24'"}) {
        fixture.write("config.toml", "[browser]\n" + key + " = " + value + "\n");
        TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
      }
      fixture.write("config.toml", "");
      luciole::ConfigService config(fixture.paths());
      TEST_CHECK(config.reload());
      const std::array zoom{luciole::OverrideEdit{preference, 48}};
      TEST_CHECK(config.saveOverrides(zoom) == luciole::AtomicWriteState::Durable);
      const auto saved = luciole::loadSettings(fixture.paths());
      TEST_CHECK(saved && (saved->browser.*field) == 48);
    }
  }

  void testSidebarWidth() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.sidebarWidth == 206);
    fixture.write("config.toml", "[browser]\nsidebar_width = 280\nshow_hidden = true\n");
    fixture.write("settings.toml", "[browser]\nsidebar_width = 160\nview_mode = 'grid'\n");
    TEST_CHECK(config.reload() && config.settings().browser.sidebarWidth == 160);
    for (const auto* invalid : {"159", "401", "true", "200.0", "'200'"}) {
      fixture.write("settings.toml", std::string("[browser]\nsidebar_width = ") + invalid + "\n");
      TEST_CHECK(!config.reload() && config.settings().browser.sidebarWidth == 160);
      TEST_CHECK(config.issues().front().key == "browser.sidebar_width");
    }
    fixture.write("settings.toml", "[browser]\nview_mode = 'grid'\n");
    const std::array width{luciole::OverrideEdit{Preference::SidebarWidth, 400}};
    TEST_CHECK(config.saveOverrides(width) == State::Durable);
    luciole::ConfigService reloaded(fixture.paths());
    TEST_CHECK(
        reloaded.reload()
        && reloaded.settings().browser.sidebarWidth == 400
        && reloaded.settings().browser.viewMode == luciole::ViewMode::Grid
        && reloaded.settings().browser.showHidden
    );
    const auto before = read(fixture.paths().overrides);
    for (const auto& value :
         {luciole::OverrideEdit{Preference::SidebarWidth, 159}, luciole::OverrideEdit{Preference::SidebarWidth, 401}}) {
      const std::array invalid{value};
      TEST_CHECK(config.saveOverrides(invalid) == State::NotCommitted && read(fixture.paths().overrides) == before);
    }
    const std::array inherit{luciole::OverrideEdit{Preference::SidebarWidth, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(inherit) == State::Durable
        && config.settings().browser.sidebarWidth == 280
        && !read(fixture.paths().overrides).contains("sidebar_width")
    );
    TEST_CHECK(read(fixture.paths().base) == "[browser]\nsidebar_width = 280\nshow_hidden = true\n");
  }

  void testPreviewVisibility() {
    using P = luciole::BrowserPreference;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.showPreview);
    fixture.write("config.toml", "[browser]\nshow_preview = true\nshow_hidden = true\n");
    TEST_CHECK(config.reload() && config.settings().browser.showPreview);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{P::ShowPreview, false}})
        == luciole::AtomicWriteState::Durable
    );
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(loaded && !loaded->browser.showPreview && loaded->browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{P::ShowPreview, std::nullopt}})
            == luciole::AtomicWriteState::Durable
        && config.settings().browser.showPreview
    );
    // File picker windows keep their own preview visibility, hidden by default.
    TEST_CHECK(!config.settings().browser.chooserShowPreview);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{P::ChooserShowPreview, true}})
        == luciole::AtomicWriteState::Durable
    );
    TEST_CHECK(config.settings().browser.chooserShowPreview && config.settings().browser.showPreview);
    // The picker's layout is separate too, and it has no Columns view.
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{P::ChooserViewMode, luciole::ViewMode::Grid}})
        == luciole::AtomicWriteState::Durable
    );
    TEST_CHECK(
        config.settings().browser.chooserViewMode == luciole::ViewMode::Grid
        && config.settings().browser.viewMode == luciole::ViewMode::List
    );
    fixture.write("config.toml", "[browser]\nchooser_view_mode = 'miller'\n");
    TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    fixture.write("config.toml", "[browser]\nshow_preview = 'true'\n");
    TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    fixture.write("config.toml", "[browser]\npreview_width = 444\nshow_preview = true\n");
    TEST_CHECK(config.reload() && config.settings().browser.previewWidth == 444);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{P::PreviewWidth, 555}})
        == luciole::AtomicWriteState::Durable
    );
    TEST_CHECK(luciole::loadSettings(fixture.paths())->browser.previewWidth == 555);
    const auto before = read(fixture.paths().overrides);
    for (int width : {207, 801}) {
      TEST_CHECK(
          config.saveOverrides(std::array{luciole::OverrideEdit{P::PreviewWidth, width}})
              == luciole::AtomicWriteState::NotCommitted
          && read(fixture.paths().overrides) == before
      );
    }
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{P::PreviewWidth, std::nullopt}})
            == luciole::AtomicWriteState::Durable
        && config.settings().browser.previewWidth == 444
    );
    for (const auto* value : {"207", "801", "true", "'320'", "320.0"}) {
      fixture.write("config.toml", std::string("[browser]\npreview_width = ") + value + "\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
  }
  void testSidebarVisibility() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.showSidebar);
    const std::string base = "[browser]\nshow_sidebar = false\nsidebar_width = 280\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\nshow_sidebar = true\ngrid_icon_size = 64\n");
    TEST_CHECK(config.reload() && config.settings().browser.showSidebar);
    const std::array hide{luciole::OverrideEdit{Preference::ShowSidebar, false}};
    TEST_CHECK(config.saveOverrides(hide) == State::Durable);
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(
        loaded
        && !loaded->browser.showSidebar
        && loaded->browser.sidebarWidth == 280
        && loaded->browser.gridIconSize == 64
    );
    const std::array show{luciole::OverrideEdit{Preference::ShowSidebar, true}};
    TEST_CHECK(config.saveOverrides(show) == State::Durable && config.settings().browser.showSidebar);
    const std::array inherit{luciole::OverrideEdit{Preference::ShowSidebar, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(inherit) == State::Durable
        && !config.settings().browser.showSidebar
        && !read(fixture.paths().overrides).contains("show_sidebar")
    );
    TEST_CHECK(read(fixture.paths().base) == base);
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\nshow_sidebar = ") + value + "\n");
      const auto issues = luciole::validateConfigFile(fixture.paths().base);
      TEST_CHECK(issues.size() == 1 && issues.front().key == "browser.show_sidebar");
    }
  }

  void testNetworkVisibility() {
    using P = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.showNetwork);
    const std::string base = "[browser]\nshow_network = false\n";
    fixture.write("config.toml", base);
    TEST_CHECK(config.reload() && !config.settings().browser.showNetwork);
    TEST_CHECK(config.saveOverrides(std::array{luciole::OverrideEdit{P::ShowNetwork, true}}) == State::Durable);
    TEST_CHECK(luciole::loadSettings(fixture.paths())->browser.showNetwork);
    TEST_CHECK(config.saveOverrides(std::array{luciole::OverrideEdit{P::ShowNetwork, false}}) == State::Durable);
    TEST_CHECK(!config.settings().browser.showNetwork);
    TEST_CHECK(config.saveOverrides(std::array{luciole::OverrideEdit{P::ShowNetwork, std::nullopt}}) == State::Durable);
    TEST_CHECK(!config.settings().browser.showNetwork && !read(fixture.paths().overrides).contains("show_network"));
    TEST_CHECK(read(fixture.paths().base) == base);
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\nshow_network = ") + value + "\n");
      const auto issues = luciole::validateConfigFile(fixture.paths().base);
      TEST_CHECK(issues.size() == 1 && issues.front().key == "browser.show_network");
    }
  }

  void testAlwaysShowTabBar() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.alwaysShowTabBar);
    fixture.write("config.toml", "[browser]\nalways_show_tab_bar = false\nshow_hidden = true\n");
    TEST_CHECK(config.reload() && !config.settings().browser.alwaysShowTabBar && config.settings().browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::AlwaysShowTabBar, true}}) == State::Durable
    );
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(loaded && loaded->browser.alwaysShowTabBar && loaded->browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::AlwaysShowTabBar, std::nullopt}})
            == State::Durable
        && !config.settings().browser.alwaysShowTabBar
        && !read(fixture.paths().overrides).contains("always_show_tab_bar")
    );
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\nalways_show_tab_bar = ") + value + "\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
  }

  void testShowCloseButtons() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.showCloseButtons);
    fixture.write("config.toml", "[browser]\nshow_close_buttons = false\nshow_hidden = true\n");
    TEST_CHECK(config.reload() && !config.settings().browser.showCloseButtons && config.settings().browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::ShowCloseButtons, true}}) == State::Durable
    );
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(loaded && loaded->browser.showCloseButtons && loaded->browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::ShowCloseButtons, std::nullopt}})
            == State::Durable
        && !config.settings().browser.showCloseButtons
        && !read(fixture.paths().overrides).contains("show_close_buttons")
    );
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\nshow_close_buttons = ") + value + "\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
  }

  void testContextMenuIcons() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.contextMenuIcons);
    fixture.write("config.toml", "[browser]\ncontext_menu_icons = false\nshow_hidden = true\n");
    TEST_CHECK(config.reload() && !config.settings().browser.contextMenuIcons && config.settings().browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::ContextMenuIcons, true}}) == State::Durable
    );
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(loaded && loaded->browser.contextMenuIcons && loaded->browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::ContextMenuIcons, std::nullopt}})
            == State::Durable
        && !config.settings().browser.contextMenuIcons
        && !read(fixture.paths().overrides).contains("context_menu_icons")
    );
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\ncontext_menu_icons = ") + value + "\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
  }

  void testAlwaysShowStatusBar() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && !config.settings().browser.alwaysShowStatusBar);
    fixture.write("config.toml", "[browser]\nalways_show_status_bar = true\nshow_hidden = true\n");
    TEST_CHECK(
        config.reload() && config.settings().browser.alwaysShowStatusBar && config.settings().browser.showHidden
    );
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::AlwaysShowStatusBar, false}})
        == State::Durable
    );
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(loaded && !loaded->browser.alwaysShowStatusBar && loaded->browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{luciole::OverrideEdit{Preference::AlwaysShowStatusBar, std::nullopt}})
            == State::Durable
        && config.settings().browser.alwaysShowStatusBar
        && !read(fixture.paths().overrides).contains("always_show_status_bar")
    );
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\nalways_show_status_bar = ") + value + "\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
  }

  void testDetailsColumnVisibility() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    struct Column {
      const char* key;
      luciole::PreferenceKey<bool> preference;
      bool luciole::DetailsColumnVisibility::* member;
    };
    const std::array columns{
        Column{"show_type_column", Preference::ShowTypeColumn, &luciole::DetailsColumnVisibility::type},
        Column{"show_size_column", Preference::ShowSizeColumn, &luciole::DetailsColumnVisibility::size},
        Column{"show_modified_column", Preference::ShowModifiedColumn, &luciole::DetailsColumnVisibility::modified},
    };
    for (const auto& column : columns) {
      Fixture fixture;
      luciole::ConfigService config(fixture.paths());
      TEST_CHECK(config.reload() && config.settings().browser.columns == luciole::DetailsColumnVisibility{});
      const auto base = std::string("[browser]\n") + column.key + " = false\nshow_hidden = true\n";
      fixture.write("config.toml", base);
      fixture.write("settings.toml", std::string("[browser]\n") + column.key + " = true\ngrid_icon_size = 64\n");
      TEST_CHECK(config.reload() && config.settings().browser.columns.*column.member);
      const std::array hide{luciole::OverrideEdit{column.preference, false}};
      TEST_CHECK(config.saveOverrides(hide) == State::Durable);
      const auto loaded = luciole::loadSettings(fixture.paths());
      TEST_CHECK(
          loaded
          && !(loaded->browser.columns.*column.member)
          && loaded->browser.showHidden
          && loaded->browser.gridIconSize == 64
      );
      const std::array inherit{luciole::OverrideEdit{column.preference, std::nullopt}};
      TEST_CHECK(
          config.saveOverrides(inherit) == State::Durable
          && !(config.settings().browser.columns.*column.member)
          && !read(fixture.paths().overrides).contains(column.key)
      );
      const std::array show{luciole::OverrideEdit{column.preference, true}};
      TEST_CHECK(config.saveOverrides(show) == State::Durable && config.settings().browser.columns.*column.member);
      TEST_CHECK(read(fixture.paths().base) == base);
      for (const auto* value : {"0", "'false'", "[]"}) {
        fixture.write("config.toml", std::string("[browser]\n") + column.key + " = " + value + "\n");
        const auto issues = luciole::validateConfigFile(fixture.paths().base);
        TEST_CHECK(issues.size() == 1 && issues.front().key == std::string("browser.") + column.key);
      }
    }
  }

  void testInteractionModes() {
    using A = luciole::ActivationMode;
    using T = luciole::TypingMode;
    using P = luciole::BrowserPreference;
    using S = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(
        config.reload()
        && config.settings().browser.activationMode == A::DoubleClick
        && config.settings().browser.typingMode == T::Search
    );
    const std::string base =
        "[browser]\nactivation_mode = 'single_click'\ntyping_mode = 'typeahead'\nshow_hidden = true\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 96\n");
    for (const auto activation : {A::SingleClick, A::DoubleClick}) {
      for (const auto typing : {T::Search, T::Filter, T::Typeahead}) {
        const std::array edits{
            luciole::OverrideEdit{P::ActivationMode, activation}, luciole::OverrideEdit{P::TypingMode, typing}
        };
        TEST_CHECK(config.saveOverrides(edits) == S::Durable);
        const auto loaded = luciole::loadSettings(fixture.paths());
        TEST_CHECK(
            loaded
            && loaded->browser.activationMode == activation
            && loaded->browser.typingMode == typing
            && loaded->browser.showHidden
            && loaded->browser.gridIconSize == 96
        );
      }
    }
    const auto saved = read(fixture.paths().overrides);
    for (const auto& bad : std::array{
             luciole::OverrideEdit{P::ActivationMode, static_cast<A>(99)},
             luciole::OverrideEdit{P::TypingMode, static_cast<T>(99)}
         }) {
      TEST_CHECK(
          config.saveOverrides(std::span(&bad, 1)) == S::NotCommitted && read(fixture.paths().overrides) == saved
      );
    }
    const std::array inherit{
        luciole::OverrideEdit{P::ActivationMode, std::nullopt}, luciole::OverrideEdit{P::TypingMode, std::nullopt}
    };
    TEST_CHECK(
        config.saveOverrides(inherit) == S::Durable
        && config.settings().browser.activationMode == A::SingleClick
        && config.settings().browser.typingMode == T::Typeahead
        && read(fixture.paths().base) == base
    );
    const auto lastGood = config.settings();
    for (const auto* key : {"activation_mode", "typing_mode"}) {
      for (const auto* bad : {"true", "1", "[]", "'single'", "'Search'", "'double-click'"}) {
        fixture.write("config.toml", std::string("[browser]\n") + key + " = " + bad + "\n");
        const auto issues = luciole::validateConfigFile(fixture.paths().base);
        TEST_CHECK(
            issues.size() == 1
            && issues.front().key == std::string("browser.") + key
            && !config.reload()
            && config.settings() == lastGood
        );
      }
    }
  }

  void testDetailsColumnWidths() {
    using P = luciole::BrowserPreference;
    using S = luciole::AtomicWriteState;
    constexpr std::array preferences{P::NameColumnWidth,     P::TypeColumnWidth,     P::SizeColumnWidth,
                                     P::ModifiedColumnWidth, P::AccessedColumnWidth, P::CreatedColumnWidth,
                                     P::OwnerColumnWidth,    P::GroupColumnWidth,    P::PermissionsColumnWidth,
                                     P::LocationColumnWidth};
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.columnWidths == luciole::kDefaultDetailsColumnWidths);
    for (std::size_t index = 0; index < preferences.size(); ++index) {
      const std::string key(luciole::kDetailsColumnWidthKeys[index]);
      const std::string base = "[browser]\n" + key + " = 300\nshow_hidden = true\n";
      fixture.write("config.toml", base);
      fixture.write("settings.toml", "[browser]\ngrid_icon_size = 64\n");
      TEST_CHECK(config.reload() && config.settings().browser.columnWidths[index] == 300);
      for (const int width :
           {luciole::minimumDetailsColumnWidth(index), luciole::maximumDetailsColumnWidth(index),
            luciole::kDefaultDetailsColumnWidths[index]}) {
        const std::array edit{luciole::OverrideEdit{preferences[index], width}};
        TEST_CHECK(config.saveOverrides(edit) == S::Durable);
        const auto loaded = luciole::loadSettings(fixture.paths());
        TEST_CHECK(
            loaded
            && loaded->browser.columnWidths[index] == width
            && loaded->browser.showHidden
            && loaded->browser.gridIconSize == 64
        );
      }
      const auto saved = read(fixture.paths().overrides);
      for (const auto& bad : std::array{
               luciole::OverrideEdit{preferences[index], -1},
               luciole::OverrideEdit{preferences[index], luciole::maximumDetailsColumnWidth(index) + 1}
           }) {
        TEST_CHECK(
            config.saveOverrides(std::span(&bad, 1)) == S::NotCommitted && read(fixture.paths().overrides) == saved
        );
      }
      const std::array inherit{luciole::OverrideEdit{preferences[index], std::nullopt}};
      TEST_CHECK(
          config.saveOverrides(inherit) == S::Durable
          && config.settings().browser.columnWidths[index] == 300
          && !read(fixture.paths().overrides).contains(key)
          && read(fixture.paths().base) == base
      );
      const auto lastGood = config.settings();
      for (const auto* invalid : {"-1", "1", "63", "16385", "9223372036854775807", "true", "160.0", "'160'", "[]"}) {
        fixture.write("config.toml", "[browser]\n" + key + " = " + invalid + "\n");
        const auto issues = luciole::validateConfigFile(fixture.paths().base);
        TEST_CHECK(
            issues.size() == 1
            && issues.front().key == "browser." + key
            && !config.reload()
            && config.settings() == lastGood
        );
      }
      if (index != 0) {
        fixture.write("config.toml", "[browser]\n" + key + " = 0\n");
        TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
      }
    }
  }

  void testExtendedColumns() {
    using P = luciole::BrowserPreference;
    const std::array keys{P::ShowAccessedColumn, P::ShowCreatedColumn,     P::ShowOwnerColumn,
                          P::ShowGroupColumn,    P::ShowPermissionsColumn, P::ShowLocationColumn};
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload());
    for (std::size_t i = 0; i < keys.size(); ++i) {
      TEST_CHECK(!luciole::preferenceValue(config.settings().browser, keys[i]));
      const auto column = static_cast<luciole::DetailsColumn>(i + 3);
      const auto key = "show_" + std::string(luciole::detailsColumnName(column)) + "_column";
      fixture.write("config.toml", "[browser]\n" + key + "=true\nshow_hidden=true\n");
      TEST_CHECK(config.reload());
      TEST_CHECK(luciole::preferenceValue(config.settings().browser, keys[i]));
      const std::array off{luciole::OverrideEdit{keys[i], false}};
      TEST_CHECK(config.saveOverrides(off) == luciole::AtomicWriteState::Durable);
      auto loaded = luciole::loadSettings(fixture.paths());
      TEST_CHECK(loaded && !luciole::preferenceValue(loaded->browser, keys[i]) && loaded->browser.showHidden);
      const std::array reset{luciole::OverrideEdit{keys[i], std::nullopt}};
      TEST_CHECK(config.saveOverrides(reset) == luciole::AtomicWriteState::Durable);
      TEST_CHECK(luciole::preferenceValue(config.settings().browser, keys[i]));
      fixture.write("config.toml", "[browser]\n" + key + "='true'\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "[browser]\ncolumn_order=['owner','type','size','modified']\n");
    TEST_CHECK(config.reload());
    TEST_CHECK(config.settings().browser.columnOrder.front() == luciole::DetailsColumn::Owner);
    TEST_CHECK(luciole::validDetailsColumnOrder(config.settings().browser.columnOrder));
    auto order = luciole::kDefaultDetailsColumnOrder;
    std::ranges::reverse(order);
    const std::array edit{luciole::OverrideEdit{P::ColumnOrder, order}};
    TEST_CHECK(config.saveOverrides(edit) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(luciole::loadSettings(fixture.paths())->browser.columnOrder == order);
    for (const auto field :
         {luciole::SortField::Accessed, luciole::SortField::Created, luciole::SortField::Owner,
          luciole::SortField::Group, luciole::SortField::Permissions, luciole::SortField::Location}) {
      const std::array sort{luciole::OverrideEdit{P::SortBy, field}};
      TEST_CHECK(config.saveOverrides(sort) == luciole::AtomicWriteState::Durable);
      TEST_CHECK(luciole::loadSettings(fixture.paths())->browser.sort.field == field);
    }
  }

  void testDetailsColumnOrder() {
    using Column = luciole::DetailsColumn;
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.columnOrder == luciole::kDefaultDetailsColumnOrder);
    const luciole::DetailsColumnOrder baseOrder{Column::Modified, Column::Type,        Column::Size,
                                                Column::Accessed, Column::Created,     Column::Owner,
                                                Column::Group,    Column::Permissions, Column::Location};
    const std::string base = "[browser]\ncolumn_order = ['modified', 'type', 'size']\nshow_size_column = false\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 64\n");
    TEST_CHECK(config.reload() && config.settings().browser.columnOrder == baseOrder);
    auto order = luciole::kDefaultDetailsColumnOrder;
    do {
      const std::array edit{luciole::OverrideEdit{Preference::ColumnOrder, order}};
      TEST_CHECK(config.saveOverrides(edit) == State::Durable);
      const auto loaded = luciole::loadSettings(fixture.paths());
      TEST_CHECK(
          loaded
          && loaded->browser.columnOrder == order
          && !loaded->browser.columns.size
          && loaded->browser.gridIconSize == 64
      );
    } while (std::ranges::next_permutation(order.begin(), order.begin() + 3).found);
    const auto saved = read(fixture.paths().overrides);
    for (const auto& edit : std::array{
             luciole::OverrideEdit{
                 Preference::ColumnOrder,
                 luciole::DetailsColumnOrder{
                     Column::Type, Column::Type, Column::Size, Column::Accessed, Column::Created, Column::Owner,
                     Column::Group, Column::Permissions, Column::Location
                 }
             },
             luciole::OverrideEdit{
                 Preference::ColumnOrder,
                 luciole::DetailsColumnOrder{Column::Type, Column::Size, static_cast<Column>(99)}
             }
         }) {
      TEST_CHECK(
          config.saveOverrides(std::span(&edit, 1)) == State::NotCommitted && read(fixture.paths().overrides) == saved
      );
    }
    const std::array inherit{luciole::OverrideEdit{Preference::ColumnOrder, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(inherit) == State::Durable
        && config.settings().browser.columnOrder == baseOrder
        && !read(fixture.paths().overrides).contains("column_order")
        && read(fixture.paths().base) == base
    );
    const auto lastGood = config.settings();
    for (const auto* invalid :
         {"true", "'type'", "[]", "['type', 'size']", "['type', 'size', 'size']", "['name', 'type', 'size']",
          "['Type', 'size', 'modified']", "['type', 'size', 1]", "['type', 'size', 'modified', 'name']"}) {
      fixture.write("config.toml", std::string("[browser]\ncolumn_order = ") + invalid + "\n");
      const auto issues = luciole::validateConfigFile(fixture.paths().base);
      TEST_CHECK(
          issues.size() == 1
          && issues.front().key == "browser.column_order"
          && !config.reload()
          && config.settings() == lastGood
      );
    }
  }

  void testThumbnailSettings() {
    using P = luciole::BrowserPreference;
    Fixture fixture;
    fixture.write(
        "config.toml", "[browser]\nthumbnail_policy = 'all'\nthumbnail_max_file_mib = 128\nthumbnail_cache_mib = 256\n"
    );
    luciole::ConfigService config(fixture.paths());
    TEST_CHECK(
        (config.reload()
         && config.settings().browser.thumbnails == luciole::ThumbnailOptions{luciole::ThumbnailPolicy::All, 128, 256})
    );
    const std::array edits{
        luciole::OverrideEdit{P::ThumbnailPolicy, luciole::ThumbnailPolicy::Never},
        luciole::OverrideEdit{P::ThumbnailMaxFileMib, 512}, luciole::OverrideEdit{P::ThumbnailCacheMib, 512}
    };
    TEST_CHECK(config.saveOverrides(edits) == luciole::AtomicWriteState::Durable);
    const auto loaded = luciole::loadSettings(fixture.paths());
    TEST_CHECK(
        (loaded && loaded->browser.thumbnails == luciole::ThumbnailOptions{luciole::ThumbnailPolicy::Never, 512, 512})
    );
    const std::array reset{
        luciole::OverrideEdit{P::ThumbnailPolicy, std::nullopt},
        luciole::OverrideEdit{P::ThumbnailMaxFileMib, std::nullopt},
        luciole::OverrideEdit{P::ThumbnailCacheMib, std::nullopt}
    };
    TEST_CHECK(
        (config.saveOverrides(reset) == luciole::AtomicWriteState::Durable
         && config.settings().browser.thumbnails == luciole::ThumbnailOptions{luciole::ThumbnailPolicy::All, 128, 256})
    );
    for (const auto* invalid :
         {"thumbnail_policy = true", "thumbnail_policy = 'remote'", "thumbnail_max_file_mib = 0",
          "thumbnail_max_file_mib = 1025", "thumbnail_cache_mib = 15", "thumbnail_cache_mib = 8193",
          "thumbnail_max_file_mib = 1.5"}) {
      fixture.write("config.toml", std::string("[browser]\n") + invalid + "\n");
      TEST_CHECK(!luciole::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "");
    for (const auto preference : {P::ThumbnailMaxFileMib, P::ThumbnailCacheMib}) {
      const std::array invalid{luciole::OverrideEdit{preference, -1}};
      TEST_CHECK(config.saveOverrides(invalid) == luciole::AtomicWriteState::NotCommitted);
    }
  }

  void testOverrideSaving() {
    using Preference = luciole::BrowserPreference;
    using State = luciole::AtomicWriteState;
    Fixture fixture;
    fixture.write("base-target.toml", "[browser]\nshow_hidden = false\nsort_by = 'size'\n");
    const auto baseTarget = fixture.directory / "base-target.toml";
    std::filesystem::create_symlink(baseTarget, fixture.paths().base);
    TEST_CHECK(::chmod(baseTarget.c_str(), 0444) == 0);
    const auto baseContent = read(baseTarget);
    luciole::ConfigService first(fixture.paths());
    luciole::ConfigService second(fixture.paths());
    TEST_CHECK(first.reload() && second.reload());
    const std::array show{luciole::OverrideEdit{Preference::ShowHidden, true}};
    const std::array descending{luciole::OverrideEdit{Preference::SortDescending, true}};
    TEST_CHECK(first.saveOverrides(show) == State::Durable && first.settings().browser.showHidden);
    // The second instance still has its earlier cache. Saving another key must
    // reread the sidecar under the lock rather than overwrite the first edit.
    TEST_CHECK(second.saveOverrides(descending) == State::Durable);
    TEST_CHECK(
        second.settings().browser.showHidden
        && second.settings().browser.sort.descending
        && second.settings().browser.sort.field == luciole::SortField::Size
    );
    TEST_CHECK(luciole::validateConfigFile(fixture.paths().overrides).empty());
    const auto saved = read(fixture.paths().overrides);
    TEST_CHECK(second.saveOverrides(descending) == State::Unchanged && read(fixture.paths().overrides) == saved);
    const std::array disable{
        luciole::OverrideEdit{Preference::DirectoriesFirst, false},
        luciole::OverrideEdit{Preference::SortDescending, false},
    };
    TEST_CHECK(
        second.saveOverrides(disable) == State::Durable
        && second.settings().browser.showHidden
        && !second.settings().browser.sort.directoriesFirst
        && !second.settings().browser.sort.descending
    );
    const std::array inherit{luciole::OverrideEdit{Preference::ShowHidden, std::nullopt}};
    TEST_CHECK(second.saveOverrides(inherit) == State::Durable && !second.settings().browser.showHidden);
    TEST_CHECK(!read(fixture.paths().overrides).contains("show_hidden"));
    TEST_CHECK(read(baseTarget) == baseContent && std::filesystem::is_symlink(fixture.paths().base));

    const auto lastGood = second.settings();
    fixture.write("settings.toml", "[browser]\nshow_hidden = 42\n");
    const auto invalid = read(fixture.paths().overrides);
    TEST_CHECK(
        second.saveOverrides(show) == State::NotCommitted
        && second.settings() == lastGood
        && read(fixture.paths().overrides) == invalid
        && !second.issues().empty()
    );
    std::filesystem::remove(fixture.paths().overrides);
    std::filesystem::create_symlink(baseTarget, fixture.paths().overrides);
    TEST_CHECK(second.saveOverrides(show) == State::NotCommitted && read(baseTarget) == baseContent);
    std::filesystem::remove(fixture.paths().overrides);
    const std::array invalidEdits{luciole::OverrideEdit{Preference::SortBy, static_cast<luciole::SortField>(99)}};
    TEST_CHECK(
        second.saveOverrides(invalidEdits) == State::NotCommitted && !std::filesystem::exists(fixture.paths().overrides)
    );
    const std::array sort{luciole::OverrideEdit{Preference::SortBy, luciole::SortField::Type}};
    TEST_CHECK(
        second.saveOverrides(sort) == State::Durable && second.settings().browser.sort.field == luciole::SortField::Type
    );
    const std::array grid{luciole::OverrideEdit{Preference::ViewMode, luciole::ViewMode::Grid}};
    TEST_CHECK(second.saveOverrides(grid) == State::Durable);
    luciole::ConfigService reloaded(fixture.paths());
    TEST_CHECK(
        reloaded.reload()
        && reloaded.settings().browser.viewMode == luciole::ViewMode::Grid
        && reloaded.settings().browser.sort.field == luciole::SortField::Type
    );
    const auto gridBytes = read(fixture.paths().overrides);
    const std::array invalidMode{luciole::OverrideEdit{Preference::ViewMode, static_cast<luciole::ViewMode>(99)}};
    TEST_CHECK(
        second.saveOverrides(invalidMode) == State::NotCommitted && read(fixture.paths().overrides) == gridBytes
    );
    const std::array resetView{luciole::OverrideEdit{Preference::ViewMode, std::nullopt}};
    TEST_CHECK(
        second.saveOverrides(resetView) == State::Durable
        && second.settings().browser.viewMode == luciole::ViewMode::List
        && !read(fixture.paths().overrides).contains("view_mode")
    );
    luciole::ConfigService aliased({.base = fixture.paths().base, .overrides = fixture.paths().base});
    TEST_CHECK(aliased.saveOverrides(show) == State::NotCommitted && read(baseTarget) == baseContent);
    Fixture reverseAlias;
    reverseAlias.write("settings.toml", "[browser]\nshow_hidden = false\n");
    const auto aliasedContent = read(reverseAlias.paths().overrides);
    std::filesystem::create_symlink(reverseAlias.paths().overrides, reverseAlias.paths().base);
    luciole::ConfigService sameFile(reverseAlias.paths());
    TEST_CHECK(
        sameFile.saveOverrides(show) == State::NotCommitted && read(reverseAlias.paths().overrides) == aliasedContent
    );

    Fixture invalidBase;
    invalidBase.write("config.toml", "[browser]\nshow_hidden = true\n");
    luciole::ConfigService protectedConfig(invalidBase.paths());
    TEST_CHECK(protectedConfig.saveOverrides(descending) == State::Durable);
    const auto before = read(invalidBase.paths().overrides);
    const auto beforeSettings = protectedConfig.settings();
    invalidBase.write("config.toml", "schema_version = 999\n");
    TEST_CHECK(
        protectedConfig.saveOverrides(sort) == State::NotCommitted
        && protectedConfig.settings() == beforeSettings
        && read(invalidBase.paths().overrides) == before
    );
  }

  void testKeybinds() {
    using K = luciole::KeybindCommand;
    const auto chord = [](std::uint32_t sym, std::uint32_t modifiers) { return luciole::KeybindChord{sym, modifiers}; };
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    const auto& defaults = service.settings().browser.keybinds;
    TEST_CHECK(luciole::keybindConfigValid(defaults));
    TEST_CHECK(defaults.chords(K::Search)[0] == chord(XKB_KEY_f, luciole::KeybindMod::Ctrl));
    TEST_CHECK(defaults.chords(K::Search)[1] == chord(XKB_KEY_k, luciole::KeybindMod::Ctrl));
    TEST_CHECK(!defaults.chords(K::Search)[2]);
    TEST_CHECK(defaults.chords(K::FocusPrevious)[0] == chord(XKB_KEY_Tab, luciole::KeybindMod::Shift));
    TEST_CHECK(defaults.chords(K::MoveFirst)[0] == chord(XKB_KEY_Home, 0));
    TEST_CHECK(defaults.chords(K::MoveLast)[0] == chord(XKB_KEY_End, 0));
    TEST_CHECK(defaults.chords(K::MovePageUp)[0] == chord(XKB_KEY_Page_Up, 0));
    TEST_CHECK(defaults.chords(K::MovePageDown)[0] == chord(XKB_KEY_Page_Down, 0));
    TEST_CHECK(defaults.chords(K::NextTab)[1] == chord(XKB_KEY_Page_Down, luciole::KeybindMod::Ctrl));
    TEST_CHECK(defaults.chords(K::PreviousTab)[1] == chord(XKB_KEY_Page_Up, luciole::KeybindMod::Ctrl));
    TEST_CHECK(!luciole::keybindMatches(defaults, K::MoveDown, XKB_KEY_Down, luciole::KeybindMod::Shift));
    TEST_CHECK(defaults.chords(K::Settings)[0] == chord(XKB_KEY_comma, luciole::KeybindMod::Ctrl));
    TEST_CHECK(defaults.chords(K::ZoomIn)[1] == chord(XKB_KEY_plus, luciole::KeybindMod::Ctrl));
    TEST_CHECK(defaults.chords(K::Terminal)[0] == chord(XKB_KEY_F4, 0));
    TEST_CHECK(defaults.chords(K::SelectTab1)[0] == chord(XKB_KEY_1, luciole::KeybindMod::Alt));
    TEST_CHECK(defaults.chords(K::SelectTab9)[0] == chord(XKB_KEY_9, luciole::KeybindMod::Alt));
    TEST_CHECK(defaults.chords(K::Validate)[1] == chord(XKB_KEY_space, 0));
    TEST_CHECK(luciole::keybindCommands(luciole::KeybindScope::Controls).size() == 12);
    TEST_CHECK(
        luciole::keybindCommands(luciole::KeybindScope::Controls).size()
            + luciole::keybindCommands(luciole::KeybindScope::FileBrowser).size()
            + luciole::keybindCommands(luciole::KeybindScope::TabsAndWindows).size()
        == luciole::kKeybindCommandCount
    );

    TEST_CHECK(luciole::parseKeybindChord("Ctrl+,") == chord(XKB_KEY_comma, luciole::KeybindMod::Ctrl));
    TEST_CHECK(luciole::parseKeybindChord("Ctrl++") == chord(XKB_KEY_plus, luciole::KeybindMod::Ctrl));
    TEST_CHECK(luciole::parseKeybindChord("Shift+Tab") == chord(XKB_KEY_Tab, luciole::KeybindMod::Shift));
    TEST_CHECK(!luciole::parseKeybindChord("Super+G"));
    TEST_CHECK(!luciole::parseKeybindChord("Super_L"));
    TEST_CHECK(!luciole::parseKeybindChord("Ctrl+"));
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_comma, luciole::KeybindMod::Ctrl)) == "Ctrl+,");
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_plus, luciole::KeybindMod::Ctrl)) == "Ctrl++");
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_space, 0)) == "Space");
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_Return, 0)) == "Enter");
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_Page_Up, 0)) == "Page Up");
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_Page_Down, 0)) == "Page Down");
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_BackSpace, 0)) == "Backspace");
    TEST_CHECK(luciole::keybindChordLabel(chord(XKB_KEY_ISO_Left_Tab, luciole::KeybindMod::Shift)) == "Shift+Tab");

    TEST_CHECK(luciole::keybindMatches(defaults, K::Copy, XKB_KEY_c, luciole::KeybindMod::Ctrl));
    TEST_CHECK(!luciole::keybindMatches(defaults, K::Copy, XKB_KEY_C, luciole::KeybindMod::Ctrl));
    TEST_CHECK(
        luciole::keybindMatches(
            defaults, K::CreateFolder, XKB_KEY_N, luciole::KeybindMod::Ctrl | luciole::KeybindMod::Shift
        )
    );
    TEST_CHECK(luciole::keybindMatches(defaults, K::FocusPrevious, XKB_KEY_ISO_Left_Tab, luciole::KeybindMod::Shift));
    TEST_CHECK(luciole::keybindMatches(defaults, K::Validate, XKB_KEY_space, 0));
    TEST_CHECK(!luciole::keybindMatches(defaults, K::Validate, XKB_KEY_space, luciole::KeybindMod::Ctrl));

    auto compact = luciole::KeybindConfig{};
    luciole::setKeybindChord(compact, K::Search, 0, std::nullopt);
    TEST_CHECK(compact.chords(K::Search)[0] == chord(XKB_KEY_k, luciole::KeybindMod::Ctrl));
    TEST_CHECK(!compact.chords(K::Search)[1]);
    luciole::setKeybindChord(compact, K::Search, 1, chord(XKB_KEY_g, luciole::KeybindMod::Ctrl));
    TEST_CHECK(compact.chords(K::Search)[1] == chord(XKB_KEY_g, luciole::KeybindMod::Ctrl));

    TEST_CHECK(
        luciole::keybindConflict(defaults, K::Search, 0, chord(XKB_KEY_l, luciole::KeybindMod::Ctrl)) == K::EditLocation
    );
    TEST_CHECK(!luciole::keybindConflict(defaults, K::Search, 0, chord(XKB_KEY_Tab, 0)));
    luciole::setActiveKeybinds(defaults);
    TEST_CHECK(luciole::activeKeybinds() == defaults);
    const auto extended =
        luciole::keybindBestMatch(luciole::keybindMovementCommands(), XKB_KEY_Down, luciole::KeybindMod::Shift);
    TEST_CHECK(extended && extended->command == K::MoveDown && extended->extraModifiers == luciole::KeybindMod::Shift);

    auto barePrintable = defaults;
    luciole::setKeybindChord(barePrintable, K::MoveDown, 0, chord(XKB_KEY_j, 0));
    TEST_CHECK(!luciole::keybindConfigValid(barePrintable));
    auto shiftedPrintable = defaults;
    luciole::setKeybindChord(shiftedPrintable, K::MoveDown, 0, chord(XKB_KEY_j, luciole::KeybindMod::Shift));
    TEST_CHECK(!luciole::keybindConfigValid(shiftedPrintable));
    auto modifiedPrintable = defaults;
    luciole::setKeybindChord(modifiedPrintable, K::MoveDown, 0, chord(XKB_KEY_j, luciole::KeybindMod::Ctrl));
    TEST_CHECK(luciole::keybindConfigValid(modifiedPrintable));
    auto unicodePrintable = defaults;
    luciole::setKeybindChord(unicodePrintable, K::MoveDown, 0, chord(XKB_KEY_eacute, 0));
    TEST_CHECK(!luciole::keybindConfigValid(unicodePrintable));

    auto specific = defaults;
    luciole::setKeybindChord(specific, K::MovePageDown, 0, chord(XKB_KEY_Down, luciole::KeybindMod::Shift));
    luciole::setActiveKeybinds(specific);
    const auto best =
        luciole::keybindBestMatch(luciole::keybindMovementCommands(), XKB_KEY_Down, luciole::KeybindMod::Shift);
    TEST_CHECK(best && best->command == K::MovePageDown && best->extraModifiers == 0);
    luciole::setActiveKeybinds(defaults);

    fixture.write("config.toml", "[browser.keybinds]\nsearch = [\"Ctrl+G\"]\n");
    TEST_CHECK(service.reload());
    const auto& partial = service.settings().browser.keybinds;
    TEST_CHECK(partial.chords(K::Search)[0] == chord(XKB_KEY_g, luciole::KeybindMod::Ctrl));
    TEST_CHECK(!partial.chords(K::Search)[1]);
    TEST_CHECK(partial.chords(K::EditLocation)[0] == chord(XKB_KEY_l, luciole::KeybindMod::Ctrl));

    fixture.write("config.toml", "[browser.keybinds]\nsearch = []\n");
    TEST_CHECK(service.reload());
    const auto& cleared = service.settings().browser.keybinds;
    TEST_CHECK(!cleared.chords(K::Search)[0]);
    TEST_CHECK(cleared.chords(K::EditLocation)[0] == chord(XKB_KEY_l, luciole::KeybindMod::Ctrl));

    // Controls and the file browser can share a chord; the same scope cannot.
    fixture.write("config.toml", "[browser.keybinds]\nsearch = [\"Tab\"]\n");
    TEST_CHECK(service.reload());
    TEST_CHECK(service.settings().browser.keybinds.chords(K::Search)[0] == chord(XKB_KEY_Tab, 0));
    fixture.write("config.toml", "[browser.keybinds]\nedit_location = [\"Ctrl+F\"]\n");
    TEST_CHECK(!service.reload());
    TEST_CHECK(service.settings().browser.keybinds.chords(K::Search)[0] == chord(XKB_KEY_Tab, 0));

    fixture.write("config.toml", "[browser.keybinds]\nsearch = [\"Super+G\"]\n");
    TEST_CHECK(!service.reload());
    fixture.write("config.toml", "[browser.keybinds]\nmove_down = [\"j\"]\n");
    TEST_CHECK(!service.reload());
    fixture.write("config.toml", "[browser.keybinds]\nunknown_command = [\"Ctrl+G\"]\n");
    TEST_CHECK(!service.reload());
    fixture.write(
        "config.toml", "[browser.keybinds]\nsearch = [\"Ctrl+G\", \"Ctrl+H\", \"Ctrl+J\", \"Ctrl+N\", \"Ctrl+M\"]\n"
    );
    TEST_CHECK(!service.reload());

    fixture.write("config.toml", "[browser]\n");
    TEST_CHECK(service.reload());
    auto edited = service.settings().browser.keybinds;
    luciole::setKeybindChord(edited, K::Search, 0, chord(XKB_KEY_g, luciole::KeybindMod::Ctrl));
    const std::array edit{luciole::OverrideEdit{luciole::BrowserPreference::Keybinds, edited}};
    TEST_CHECK(service.saveOverrides(edit) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService reloaded(fixture.paths());
    TEST_CHECK(reloaded.reload());
    TEST_CHECK(
        reloaded.settings().browser.keybinds.chords(K::Search)[0] == chord(XKB_KEY_g, luciole::KeybindMod::Ctrl)
    );
    TEST_CHECK(
        reloaded.settings().browser.keybinds.chords(K::Search)[1] == chord(XKB_KEY_k, luciole::KeybindMod::Ctrl)
    );
    const auto saved = read(fixture.paths().overrides);
    TEST_CHECK(saved.contains("[browser.keybinds]"));
    TEST_CHECK(saved.contains("search"));
    TEST_CHECK(!saved.contains("cancel"));

    const auto inherit = luciole::inheritAllPreferences();
    TEST_CHECK(reloaded.saveOverrides(inherit) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(reloaded.reload());
    TEST_CHECK(
        reloaded.settings().browser.keybinds.chords(K::Search)[0] == chord(XKB_KEY_f, luciole::KeybindMod::Ctrl)
    );

    Fixture inheritedFixture;
    inheritedFixture.write("config.toml", "[browser.keybinds]\nsearch = [\"Ctrl+G\"]\n");
    luciole::ConfigService inheritedService(inheritedFixture.paths());
    TEST_CHECK(inheritedService.reload());
    const auto builtin = luciole::KeybindConfig{};
    const std::array inheritedEdit{luciole::OverrideEdit{luciole::BrowserPreference::Keybinds, builtin}};
    TEST_CHECK(inheritedService.saveOverrides(inheritedEdit) == luciole::AtomicWriteState::Durable);
    const auto inheritedSaved = read(inheritedFixture.paths().overrides);
    TEST_CHECK(inheritedSaved.contains("search"));
    luciole::ConfigService inheritedReloaded(inheritedFixture.paths());
    TEST_CHECK(inheritedReloaded.reload());
    TEST_CHECK(
        inheritedReloaded.settings().browser.keybinds.chords(K::Search)[0]
        == chord(XKB_KEY_f, luciole::KeybindMod::Ctrl)
    );
    TEST_CHECK(
        inheritedReloaded.settings().browser.keybinds.chords(K::Search)[1]
        == chord(XKB_KEY_k, luciole::KeybindMod::Ctrl)
    );

    Fixture layeredFixture;
    layeredFixture.write("config.toml", "[browser.keybinds]\nsearch = [\"Ctrl+G\"]\n");
    layeredFixture.write("settings.toml", "[browser.keybinds]\ncut = [\"Alt+X\"]\n");
    luciole::ConfigService layeredService(layeredFixture.paths());
    TEST_CHECK(layeredService.reload());
    TEST_CHECK(
        layeredService.settings().browser.keybinds.chords(K::Search)[0] == chord(XKB_KEY_g, luciole::KeybindMod::Ctrl)
    );
    TEST_CHECK(
        layeredService.settings().browser.keybinds.chords(K::Cut)[0] == chord(XKB_KEY_x, luciole::KeybindMod::Alt)
    );
    const std::array unrelatedEdit{luciole::OverrideEdit{luciole::BrowserPreference::ShowHidden, true}};
    TEST_CHECK(layeredService.saveOverrides(unrelatedEdit) == luciole::AtomicWriteState::Durable);
    const auto layeredSaved = read(layeredFixture.paths().overrides);
    TEST_CHECK(layeredSaved.contains("cut"));
    TEST_CHECK(!layeredSaved.contains("search"));
    TEST_CHECK(layeredService.reload());
    TEST_CHECK(
        layeredService.settings().browser.keybinds.chords(K::Search)[0] == chord(XKB_KEY_g, luciole::KeybindMod::Ctrl)
    );
    TEST_CHECK(
        layeredService.settings().browser.keybinds.chords(K::Cut)[0] == chord(XKB_KEY_x, luciole::KeybindMod::Alt)
    );
  }
} // namespace

int main() {
  try {
    testBackgroundOpacity();
    testCornerRadiusScale();
    testTerminalCommand();
    testSchemaAndDiagnostics();
    testPresentationSettings();
    testFilenameTruncation();
    testDropAction();
    testBackupPreference();
    testNewTabPreference();
    testSingleClickLocationEditPreference();
    testDeletionConfirmationPreference();
    testPermanentDeletionMenuPreference();
    testRestoreLastWindowPreference();
    testSingleWindowPreference();
    testMillerSettings();
    testMillerParents();
    testUnknownSettings();
    testLayersAndReload();
    testFilesAndLimits();
    testEnvironmentPaths();
    testBrowserDefaults();
    testGridIconSize();
    testRowIconSizes();
    testSidebarWidth();
    testSidebarVisibility();
    testNetworkVisibility();
    testAlwaysShowTabBar();
    testShowCloseButtons();
    testContextMenuIcons();
    testAlwaysShowStatusBar();
    testPreviewVisibility();
    testDetailsColumnVisibility();
    testExtendedColumns();
    testDetailsColumnOrder();
    testDetailsColumnWidths();
    testInteractionModes();
    testThumbnailSettings();
    testKeybinds();
    testOverrideSaving();
    std::println("settings checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "settings test: {}", error.what());
    return 1;
  }
}
