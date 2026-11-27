#include "browser/action_registry.h"
#include "browser/selection_tools.h"
#include "config/keybinds.h"
#include "core/input/key_modifiers.h"
#include "directory_fixture.h"
#include "fs/archive_type.h"
#include "fs/gio_interop.h"
#include "test_check.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/browser/file_view.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <print>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace std::chrono_literals;
  using A = luciole::BrowserAction;
  using R = luciole::ActionRegistry;
  luciole::DirectoryEntry
  entry(luciole::EntryId id, const std::string& name, luciole::FileType type = luciole::FileType::Regular) {
    return {
        .id = id,
        .place = luciole::Location::fromPath("/tmp/action-fixture/" + name),
        .name = name,
        .type = type,
        .size = {},
        .modifiedSeconds = {}
    };
  }
  luciole::DirectorySnapshot snapshot() {
    return {
        .generation = 1,
        .location = luciole::Location::fromPath("/tmp/action-fixture"),
        .status = luciole::DirectoryStatus::Ready,
        .entries =
            {entry(1, "alpha.txt"), entry(2, "Beta.txt"), entry(3, "boulder.png"), entry(4, "café.txt"),
             entry(5, "[literal].txt"), entry(6, "folder", luciole::FileType::Directory)},
        .error = {},
        .monitorError = {}
    };
  }
  void testPointerActivation() {
    luciole::FileIconCache icons;
    luciole::ThumbnailService thumbnails;
    for (const auto mode : {luciole::ViewMode::List, luciole::ViewMode::Grid, luciole::ViewMode::Miller}) {
      for (const auto policy : {luciole::ActivationMode::SingleClick, luciole::ActivationMode::DoubleClick}) {
        luciole::BrowserController browser({}, false);
        auto& model = browser.model();
        model.applySnapshot(snapshot());
        std::uint32_t modifiers = KeyMod::Alt;
        luciole::FileViewAdapter adapter(
            browser, icons, thumbnails, mode, [&] { return modifiers; }, [] { return true; }
        );
        adapter.setActivationMode(policy);
        std::vector<luciole::EntryId> activated, dragged;
        adapter.setActivateCallback([&](luciole::EntryId id) { activated.push_back(id); });
        adapter.setDragCallback([&](const auto& ids) { dragged = ids; }, [] {});
        const auto index = [&](luciole::EntryId id) {
          const auto found = std::ranges::find(model.rows(), id);
          TEST_CHECK(found != model.rows().end());
          return static_cast<std::size_t>(found - model.rows().begin());
        };
        const auto press = [&](luciole::EntryId id) {
          TEST_CHECK(adapter.onPointerPress(index(id), 100, 15, 300, 36));
        };
        const auto release = [&](std::optional<std::size_t> row) {
          const auto result = adapter.onPointerRelease(row);
          if (result.activateIndex) {
            adapter.onActivate(*result.activateIndex);
          }
        };
        const auto click = [&](luciole::EntryId id) {
          press(id);
          release(index(id));
        };
        modifiers = 0;
        click(1);
        if (policy == luciole::ActivationMode::DoubleClick) {
          TEST_CHECK(activated.empty());
          click(1);
        }
        TEST_CHECK(activated == std::vector<luciole::EntryId>{1});
        modifiers = KeyMod::Alt;
        for (const auto id : {luciole::EntryId{1}, luciole::EntryId{6}}) {
          adapter.onPointerCancel();
          activated.clear();
          model.select(2);
          press(id);
          if (mode == luciole::ViewMode::Miller) {
            TEST_CHECK(model.selection().selected.contains(2) && !model.selection().selected.contains(id));
          }
          release(index(id));
          if (policy == luciole::ActivationMode::DoubleClick) {
            TEST_CHECK(activated.empty());
            click(id);
          }
          TEST_CHECK(activated == std::vector<luciole::EntryId>{id});
        }
        adapter.onPointerCancel();
        activated.clear();
        model.select(1);
        model.select(2, luciole::SelectionMode::Toggle);
        press(1);
        TEST_CHECK(!adapter.onPointerDrag(index(1), 130, 15, 300, 36));
        TEST_CHECK(
            dragged.size() == 2 && model.selection().selected.contains(1) && model.selection().selected.contains(2)
        );
        release(index(1));
        TEST_CHECK(activated.empty());
        adapter.onPointerCancel();
        press(6);
        TEST_CHECK(!adapter.onPointerDrag(index(6), 130, 15, 300, 36));
        TEST_CHECK(dragged == std::vector<luciole::EntryId>{6});
        release(index(6));
        TEST_CHECK(activated.empty());
        adapter.onPointerCancel();
        press(1);
        release(index(2));
        TEST_CHECK(activated.empty());
        press(1);
        adapter.onPointerCancel();
        release(index(1));
        TEST_CHECK(activated.empty());
        for (const auto chord :
             {KeyMod::Ctrl | KeyMod::Alt, KeyMod::Shift | KeyMod::Alt, KeyMod::Super | KeyMod::Alt}) {
          modifiers = chord;
          click(1);
          click(1);
          TEST_CHECK(activated.empty());
        }
        adapter.onPointerCancel();
        adapter.setActivationMode(luciole::ActivationMode::DoubleClick);
        modifiers = 0;
        click(1);
        modifiers = KeyMod::Alt;
        click(1);
        TEST_CHECK(activated.empty());
        modifiers = 0;
        click(1);
        TEST_CHECK(activated.empty());
        modifiers = KeyMod::Alt;
        press(1);
        modifiers = 0;
        release(index(1));
        TEST_CHECK(activated.empty());
        modifiers = KeyMod::Alt;
        click(1);
        click(1);
        TEST_CHECK(activated == std::vector<luciole::EntryId>{1});
      }
    }
  }
  void testPendingActionFacts() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    test::editEntries(data, [](auto& entries) {
      entries[0].metadataPending = true;
      entries[0].queriedDetails = 0;
    });
    model.applySnapshot(data);
    TEST_CHECK(model.select(1));
    luciole::ActionContext context{
        .model = model, .context = 42, .query = {}, .pinned = {}, .clipboardAvailable = true
    };
    const auto request = R::capture(context);
    TEST_CHECK(R::current(context, request, luciole::ActionScope::Targets));
    for (const auto action : {A::Open, A::CopyFiles, A::Rename, A::Trash, A::DeletePermanently, A::Properties}) {
      TEST_CHECK(!R::state(action, context, request).enabled);
    }
    test::editEntries(data, [](auto& entries) { entries[0].metadataPending = false; });
    ++data.generation;
    model.applySnapshot(data);
    // Hidden owner/group/permissions columns are not prerequisites for file actions.
    TEST_CHECK(R::state(A::CopyFiles, context, request).enabled && R::state(A::Rename, context, request).enabled);
    test::editEntries(data, [](auto& entries) { entries[0].metadataFailed = true; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CopyFiles, context, request).enabled);
    test::editEntries(data, [](auto& entries) {
      entries[0].metadataFailed = false;
      entries[0].place = luciole::Location::fromPath("/tmp/action-fixture/replaced");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::current(context, request, luciole::ActionScope::Targets));
  }

  void testFileClipboardActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    model.applySnapshot(data);
    luciole::ActionContext c{
        .model = model, .context = 12, .query = {}, .pinned = {}, .clipboardAvailable = true, .clipboardHasFiles = true
    };
    TEST_CHECK(!R::state(A::CopyFiles, c, R::capture(c)).enabled);
    model.select(1);
    const auto request = R::capture(c);
    TEST_CHECK(
        R::state(A::CutFiles, c, request).enabled
        && R::state(A::CopyFiles, c, request).enabled
        && R::state(A::PasteFiles, c, request).enabled
    );
    c.clipboardHasFiles = false;
    TEST_CHECK(!R::state(A::PasteFiles, c, request).enabled);
    c.clipboardHasImage = true;
    TEST_CHECK(R::state(A::PasteFiles, c, request).enabled);
    c.clipboardHasImage = false;
    c.clipboardHasFiles = true;
    c.operationsBusy = true;
    TEST_CHECK(!R::state(A::PasteFiles, c, request).enabled);
    c.operationsBusy = false;
    c.query = "search";
    TEST_CHECK(!R::state(A::PasteFiles, c, R::capture(c)).enabled);
    c.query = {};
    data.metadata.access.write = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::PasteFiles, c, request).enabled);
    test::editEntries(data, [&](auto& entries) { entries[0].access.rename = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CutFiles, c, request).enabled && R::state(A::CopyFiles, c, request).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries[0].place = luciole::Location::fromUri("sftp://example.invalid/source");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(R::state(A::CopyFiles, c, R::capture(c)).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries[0].place = luciole::Location::fromUri("smb://example.invalid/share/source");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(R::state(A::CopyFiles, c, R::capture(c)).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries[0].place = luciole::Location::fromUri("https://example.invalid/file");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CopyFiles, c, R::capture(c)).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries[0].place = luciole::Location::fromPath("/tmp/action-fixture/broken");
      entries[0].symlink = true;
      entries[0].access.read = false;
      entries[0].access.rename = true;
      entries[0].access.remove = true;
    });
    ++data.generation;
    model.applySnapshot(data);
    model.select(2, luciole::SelectionMode::Toggle);
    TEST_CHECK(R::state(A::CopyFiles, c, R::capture(c)).enabled && R::state(A::CutFiles, c, R::capture(c)).enabled);
    test::editEntries(data, [&](auto& entries) { entries[0].access.remove = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(R::state(A::CopyFiles, c, R::capture(c)).enabled && !R::state(A::CutFiles, c, R::capture(c)).enabled);
    test::editEntries(data, [&](auto& entries) { entries[0].symlink = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CopyFiles, c, R::capture(c)).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries[0].symlink = true;
      entries[0].place = luciole::Location::fromUri("sftp://example.invalid/broken");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CopyFiles, c, R::capture(c)).enabled);
  }
  void testTerminalActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    model.applySnapshot(data);
    model.select(6);
    luciole::ActionContext context{.model = model, .context = 11, .query = {}, .pinned = {}};
    const auto request = R::capture(context);
    TEST_CHECK(!R::state(A::Terminal, context, request).enabled);
    TEST_CHECK(!R::state(A::TargetTerminal, context, request).enabled);
    context.terminalAvailable = true;
    TEST_CHECK(R::state(A::Terminal, context, request).enabled);
    TEST_CHECK(R::state(A::TargetTerminal, context, request).enabled);
    model.select(1);
    TEST_CHECK(R::state(A::Terminal, context, R::capture(context)).enabled);
    TEST_CHECK(!R::state(A::TargetTerminal, context, R::capture(context)).enabled);
    model.select(6);
    test::editEntries(data, [&](auto& entries) { entries.back().access.execute = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(R::state(A::Terminal, context, request).enabled);
    TEST_CHECK(!R::state(A::TargetTerminal, context, request).enabled);
    data.metadata.access.execute = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Terminal, context, request).enabled);
    data.metadata.access.execute = true;
    data.location = luciole::Location::fromUri("sftp://example.invalid/folder");
    test::editEntries(data, [&](auto& entries) {
      entries.back().place = luciole::Location::fromUri("sftp://example.invalid/folder/child");
      entries.back().access.execute = true;
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Terminal, context, R::capture(context)).enabled);
    TEST_CHECK(!R::state(A::TargetTerminal, context, R::capture(context)).enabled);
    TEST_CHECK(!R::state(A::TargetTerminal, context, request).enabled);
    data.location = luciole::Location::trash();
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Terminal, context, R::capture(context)).enabled);
  }
  void testExecutionActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    test::editEntries(data, [&](auto& entries) { entries.front().access.execute = true; });
    model.applySnapshot(data);
    model.select(1);
    luciole::ActionContext context{.model = model, .context = 11, .query = {}, .pinned = {}};
    auto request = R::capture(context);
    TEST_CHECK(R::state(A::RunProgram, context, request).enabled);
    TEST_CHECK(!R::state(A::RunInTerminal, context, request).enabled);
    context.terminalAvailable = true;
    TEST_CHECK(R::state(A::RunInTerminal, context, request).enabled);
    TEST_CHECK(R::state(A::Scripts, context, request).enabled);
    model.select(6);
    TEST_CHECK(!R::state(A::RunProgram, context, R::capture(context)).enabled);
    model.select(1);
    test::editEntries(data, [&](auto& entries) { entries.front().access.execute = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::RunProgram, context, request).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries.front().place = luciole::Location::fromPath("/tmp/action-fixture/test.desktop");
    });
    ++data.generation;
    model.applySnapshot(data);
    request = R::capture(context);
    TEST_CHECK(R::state(A::RunProgram, context, request).enabled);
    TEST_CHECK(!R::state(A::RunInTerminal, context, request).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries.front().place = luciole::Location::fromUri("sftp://server/test.desktop");
    });
    ++data.generation;
    model.applySnapshot(data);
    request = R::capture(context);
    TEST_CHECK(!R::state(A::RunProgram, context, request).enabled);
    TEST_CHECK(!R::state(A::Scripts, context, request).enabled);
  }
  void testTrashActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    data.location = luciole::Location::trash();
    test::editEntries(data, [&](auto& entries) {
      entries.erase(entries.begin() + 1, entries.end());
      entries[0].place = luciole::Location::fromUri("trash:///alpha.txt");
      entries[0].setIdentity(luciole::FileIdentity{"trash-volume", "item-1"});
      entries[0].setTrash(
          luciole::TrashInfo{.original = luciole::Location::fromPath("/tmp/original/alpha.txt"), .deletionDate = {}}
      );
    });
    model.applySnapshot(data);
    model.select(1);
    luciole::ActionContext context{.model = model, .context = 33, .query = {}, .pinned = {}};
    context.clipboardAvailable = true;
    const auto request = R::capture(context);
    TEST_CHECK(
        R::state(A::Restore, context, request).enabled && R::state(A::DeletePermanently, context, request).enabled
    );
    test::editEntries(data, [&](auto& entries) { entries[0].access.rename = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(R::state(A::CutFiles, context, request).enabled && !R::state(A::CopyFiles, context, request).enabled);
    context.operationsBusy = true;
    TEST_CHECK(!R::state(A::CutFiles, context, request).enabled);
    TEST_CHECK(
        !R::state(A::Restore, context, request).enabled && !R::state(A::DeletePermanently, context, request).enabled
    );
    context.operationsBusy = false;
    test::editEntries(data, [&](auto& entries) { entries[0].setTrash(std::nullopt); });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(
        !R::state(A::Restore, context, request).enabled && R::state(A::DeletePermanently, context, request).enabled
    );
    TEST_CHECK(R::state(A::CutFiles, context, request).enabled);
    test::editEntries(data, [&](auto& entries) { entries[0].access.remove = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::DeletePermanently, context, request).enabled);
    TEST_CHECK(!R::state(A::CutFiles, context, request).enabled);
    test::editEntries(data, [&](auto& entries) { entries[0].access.remove = true; });
    data.location = luciole::Location::fromUri("trash:///folder");
    test::editEntries(data, [&](auto& entries) {
      entries[0].place = luciole::Location::fromUri("trash:///folder/alpha.txt");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(model.select(1));
    TEST_CHECK(!R::state(A::DeletePermanently, context, R::capture(context)).enabled);
    data.location = luciole::Location::fromPath("/tmp/action-fixture");
    test::editEntries(data, [&](auto& entries) {
      entries[0].place = luciole::Location::fromPath("/tmp/action-fixture/alpha.txt");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(model.select(1));
    TEST_CHECK(R::state(A::DeletePermanently, context, R::capture(context)).enabled);
    context.operationsBusy = true;
    TEST_CHECK(!R::state(A::DeletePermanently, context, R::capture(context)).enabled);
    context.operationsBusy = false;
    test::editEntries(data, [](auto& entries) { entries[0].access.remove = false; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::DeletePermanently, context, R::capture(context)).enabled);
  }
  void testPermanentDeletionMenuEntries() {
    TEST_CHECK((luciole::itemMenuDeletionActions(false, false) == std::vector{A::Trash}));
    TEST_CHECK((luciole::itemMenuDeletionActions(false, true) == std::vector{A::Trash, A::DeletePermanently}));
    TEST_CHECK((luciole::itemMenuDeletionActions(true, false) == std::vector{A::Restore, A::DeletePermanently}));
    TEST_CHECK((luciole::itemMenuDeletionActions(true, true) == std::vector{A::Restore, A::DeletePermanently}));
  }
  void testEmptyTrashAvailability() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    data.location = luciole::Location::trash();
    data.entries = {};
    model.applySnapshot(data);
    luciole::ActionContext context{.model = model, .context = 35, .query = {}, .canEmptyTrash = true, .pinned = {}};
    const auto state = [&] { return R::state(A::EmptyTrash, context, R::capture(context)); };
    TEST_CHECK(!state().enabled && state().reason == "Trash is already empty.");
    auto hidden = entry(1, ".hidden");
    hidden.place = luciole::Location::trash().child(hidden.name);
    hidden.hidden = true;
    data.entries = {hidden};
    ++data.generation;
    model.applySnapshot(data);
    model.setFilter({});
    TEST_CHECK(model.rows().empty() && state().enabled);
    model.setFilter({.showHidden = true, .text = "unmatched"});
    TEST_CHECK(model.rows().empty() && state().enabled);
    context.operationsBusy = true;
    TEST_CHECK(!state().enabled);
    context.operationsBusy = false;
    context.canEmptyTrash = false;
    TEST_CHECK(!state().enabled);
    context.canEmptyTrash = true;
    data.status = luciole::DirectoryStatus::Loading;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!state().enabled);
    data.status = luciole::DirectoryStatus::Error;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!state().enabled);
    data.status = luciole::DirectoryStatus::Ready;
    data.entries = {};
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!state().enabled);
    context.query = "search without matches";
    TEST_CHECK(state().enabled);
    context.query = {};
    data.location = luciole::Location::trash().child("nested");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(state().enabled);
    data.location = luciole::Location::fromPath("/tmp/action-fixture/Trash");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(state().enabled);
    data.location = luciole::Location::trash();
    data.entries = {hidden};
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(state().enabled);
  }
  void testLinkActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    test::editEntries(data, [&](auto& entries) { entries[0].setIdentity(luciole::FileIdentity{"native", "source"}); });
    model.applySnapshot(data);
    model.select(1);
    luciole::ActionContext context{.model = model, .context = 44, .query = {}, .pinned = {}};
    const auto enabled = [&] { return R::state(A::CreateLink, context, R::capture(context)).enabled; };
    TEST_CHECK(enabled());
    context.operationsBusy = true;
    TEST_CHECK(!enabled());
    context.operationsBusy = false;
    data.metadata.filesystem.readOnly = true;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(enabled());
    for (const auto type : {luciole::FileType::Directory, luciole::FileType::Symlink}) {
      test::editEntries(data, [&](auto& entries) {
        entries[0].type = type;
        entries[0].symlink = type == luciole::FileType::Symlink;
      });
      ++data.generation;
      model.applySnapshot(data);
      TEST_CHECK(enabled());
    }
    test::editEntries(data, [&](auto& entries) { entries[0].type = luciole::FileType::Special; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!enabled());
    test::editEntries(data, [&](auto& entries) {
      entries[0].type = luciole::FileType::Regular;
      entries[0].symlink = false;
    });
    for (const auto& uri : {"trash:///source", "sftp://example.invalid/source"}) {
      test::editEntries(data, [&](auto& entries) { entries[0].place = luciole::Location::fromUri(uri); });
      ++data.generation;
      model.applySnapshot(data);
      TEST_CHECK(!enabled());
    }
    test::editEntries(data, [&](auto& entries) {
      entries[0].place = luciole::Location::fromPath("/tmp/action-fixture/alpha.txt");
    });
    test::editEntries(data, [&](auto& entries) { entries[0].setIdentity(std::nullopt); });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!enabled());
    test::editEntries(data, [&](auto& entries) { entries[0].setIdentity(luciole::FileIdentity{"native", "source"}); });
    ++data.generation;
    model.applySnapshot(data);
    context.query = "source";
    TEST_CHECK(enabled());
    model.selectAll();
    TEST_CHECK(!enabled());
  }
  void testPreviewActions() {
    luciole::DirectoryModel model;
    model.applySnapshot(snapshot());
    luciole::ActionContext context{.model = model, .context = 1, .query = {}, .pinned = {}};
    TEST_CHECK(!R::state(A::QuickPreview, context, R::capture(context)).enabled);
    model.select(1);
    const auto request = R::capture(context);
    TEST_CHECK(R::state(A::OpenWith, context, request).enabled && R::state(A::QuickPreview, context, request).enabled);
    model.select(3, luciole::SelectionMode::Toggle);
    const auto batch = R::capture(context);
    TEST_CHECK(batch.targets.size() == 2 && R::state(A::OpenWith, context, batch).enabled);
    TEST_CHECK(!R::state(A::QuickPreview, context, batch).enabled);
    auto changed = snapshot();
    test::editEntries(changed, [&](auto& entries) { entries[2].access.read = false; });
    ++changed.generation;
    model.applySnapshot(changed);
    TEST_CHECK(!R::state(A::OpenWith, context, R::capture(context)).enabled);
    model.select(6);
    TEST_CHECK(
        !R::state(A::OpenWith, context, R::capture(context)).enabled
        && R::state(A::QuickPreview, context, R::capture(context)).enabled
    );
    model.selectAll();
    TEST_CHECK(!R::state(A::QuickPreview, context, R::capture(context)).enabled);
    ++context.context;
    TEST_CHECK(!R::state(A::OpenWith, context, request).enabled);
  }
  void testArchiveActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    test::editEntries(data, [&](auto& entries) { entries.push_back(entry(7, "sample.TAR.GZ")); });
    model.applySnapshot(data);
    model.select(7);
    luciole::ActionContext c{.model = model, .context = 9, .query = {}, .pinned = {}};
    const auto request = R::capture(c);
    TEST_CHECK(
        R::state(A::Compress, c, request).enabled
        && R::state(A::ExtractHere, c, request).enabled
        && R::state(A::ExtractTo, c, request).enabled
        && R::state(A::BrowseArchive, c, request).enabled
    );
    TEST_CHECK(R::openAction(c, request) == A::BrowseArchive);
    c.operationsBusy = true;
    TEST_CHECK(!R::state(A::BrowseArchive, c, request).enabled && !R::state(A::Compress, c, request).enabled);
    // A running job refuses Browse Archive instead of letting Open fall back to another application.
    TEST_CHECK(R::openAction(c, request) == A::BrowseArchive);
    c.operationsBusy = false;
    c.chooser = true;
    TEST_CHECK(R::openAction(c, request) == A::Open);
    c.chooser = false;
    test::editEntries(data, [&](auto& entries) { entries.back().symlink = true; });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::ExtractTo, c, request).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries.back().symlink = false;
      entries.back().access.read = false;
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Compress, c, request).enabled);
    model.select(1);
    const auto plain = R::capture(c);
    TEST_CHECK(R::state(A::Compress, c, plain).enabled && !R::state(A::BrowseArchive, c, plain).enabled);
    TEST_CHECK(R::openAction(c, plain) == A::Open);
    model.select(6);
    TEST_CHECK(R::state(A::Compress, c, R::capture(c)).enabled);
    test::editEntries(data, [&](auto& entries) {
      entries.back().place = luciole::Location::fromUri("sftp://example.invalid/sample.tar.gz");
      entries.back().access.read = true;
    });
    ++data.generation;
    model.applySnapshot(data);
    model.select(7);
    TEST_CHECK(!R::state(A::Compress, c, R::capture(c)).enabled);
    TEST_CHECK(R::openAction(c, R::capture(c)) == A::Open);
  }
  void testBatchArchiveActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    test::editEntries(data, [](auto& entries) {
      entries.push_back(entry(7, "one.zip"));
      entries.push_back(entry(8, "two.tar"));
    });
    model.applySnapshot(data);
    model.select(7);
    model.select(8, luciole::SelectionMode::Toggle);
    luciole::ActionContext context{.model = model, .context = 9, .query = {}, .pinned = {}};
    const auto request = R::capture(context);
    TEST_CHECK(request.targets.size() == 2);
    TEST_CHECK(R::state(A::ExtractHere, context, request).enabled && R::state(A::ExtractTo, context, request).enabled);
    TEST_CHECK(!R::state(A::BrowseArchive, context, request).enabled);
    context.operationsBusy = true;
    TEST_CHECK(!R::state(A::ExtractHere, context, request).enabled);
    context.operationsBusy = false;
    context.chooser = true;
    TEST_CHECK(!R::state(A::ExtractTo, context, request).enabled);
    context.chooser = false;
    model.select(1, luciole::SelectionMode::Toggle);
    TEST_CHECK(!R::state(A::ExtractHere, context, R::capture(context)).enabled);
    model.select(1, luciole::SelectionMode::Toggle);
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto invalid = data;
      test::editEntries(invalid, [mode](auto& entries) {
        auto& target = entries.back();
        if (mode == 0) {
          target.symlink = true;
        }
        if (mode == 1) {
          target.access.read = false;
        }
        if (mode == 2) {
          target.place = luciole::Location::fromUri("sftp://example.invalid/two.tar");
        }
        if (mode == 3) {
          target.type = luciole::FileType::Directory;
        }
      });
      ++invalid.generation;
      model.applySnapshot(invalid);
      TEST_CHECK(!R::state(A::ExtractTo, context, R::capture(context)).enabled);
    }
    model.applySnapshot(data);
    model.clearSelection();
    TEST_CHECK(!R::state(A::ExtractHere, context, R::capture(context)).enabled);
  }

  void testSplitArchiveActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    test::editEntries(data, [&](auto& entries) {
      entries.push_back(entry(7, "download.ZIP.001"));
      entries.push_back(entry(8, "download.ZIP.002"));
      entries.back().contentType = "application/zip";
    });
    model.applySnapshot(data);
    model.select(7);
    luciole::ActionContext context{.model = model, .context = 1, .query = {}, .pinned = {}};
    auto request = R::capture(context);
    for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
      TEST_CHECK(R::state(action, context, request).enabled);
    }
    model.select(8);
    request = R::capture(context);
    for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
      TEST_CHECK(!R::state(action, context, request).enabled);
    }
    TEST_CHECK(luciole::archiveFolderName(data.entries[6].location()) == "download");
    TEST_CHECK(!luciole::isSupportedArchive(luciole::Location::fromUri("sftp://example.invalid/test.zip.001"), {}));
    TEST_CHECK(!luciole::splitZipPart(luciole::Location::fromPath("/tmp/test.zip.01")));
    TEST_CHECK(!luciole::splitZipPart(luciole::Location::fromPath("/tmp/test.zip.001.bak")));
  }
  void testCompressedArchiveActions() {
    luciole::DirectoryModel sevenZipModel;
    auto sevenZipData = snapshot();
    test::editEntries(sevenZipData, [&](auto& entries) { entries.push_back(entry(7, "package.7Z")); });
    sevenZipModel.applySnapshot(sevenZipData);
    sevenZipModel.select(7);
    luciole::ActionContext sevenZipContext{.model = sevenZipModel, .context = 1, .query = {}, .pinned = {}};
    const auto sevenZipRequest = R::capture(sevenZipContext);
    for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
      TEST_CHECK(R::state(action, sevenZipContext, sevenZipRequest).enabled);
    }
    TEST_CHECK(luciole::archiveFolderName(sevenZipData.entries.back().location()) == "package");
    TEST_CHECK(luciole::isSupportedArchive(luciole::Location::fromPath("/tmp/opaque"), "application/x-7z-compressed"));
    TEST_CHECK(
        !luciole::isSupportedArchive(luciole::Location::fromPath("/tmp/split.7z.001"), "application/x-7z-compressed")
    );
    TEST_CHECK(luciole::isSupportedArchive(luciole::Location::fromPath("/tmp/supported.RAR"), {}));
    TEST_CHECK(luciole::archiveFolderName(luciole::Location::fromPath("/tmp/supported.RAR")) == "supported");
    for (const auto* const mime : {"application/vnd.rar", "application/x-rar", "application/x-rar-compressed"}) {
      TEST_CHECK(luciole::isSupportedArchive(luciole::Location::fromPath("/tmp/opaque-rar"), mime));
      for (const auto* const name : {"set.part1.rar", "set.part002.RAR", "set.rar.001", "set.r00", "set.s01"}) {
        TEST_CHECK(!luciole::isSupportedArchive(luciole::Location::fromPath(std::string("/tmp/") + name), mime));
      }
    }
    luciole::DirectoryModel rarModel;
    auto rarData = snapshot();
    test::editEntries(rarData, [&](auto& entries) { entries.push_back(entry(7, "package.rar")); });
    rarModel.applySnapshot(rarData);
    rarModel.select(7);
    luciole::ActionContext rarContext{.model = rarModel, .context = 1, .query = {}, .pinned = {}};
    for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
      TEST_CHECK(R::state(action, rarContext, R::capture(rarContext)).enabled);
    }
    TEST_CHECK(!luciole::isSupportedArchive(luciole::Location::fromUri("sftp://example.invalid/package.rar"), {}));
    for (const auto compression :
         {luciole::ArchiveCompression::Bzip2, luciole::ArchiveCompression::Zstd, luciole::ArchiveCompression::Xz}) {
      const bool bzip = compression == luciole::ArchiveCompression::Bzip2;
      const char* const name = bzip                          ? "package.TAR.BZ2"
          : compression == luciole::ArchiveCompression::Zstd ? "package.TAR.ZST"
                                                             : "package.TAR.XZ";
      luciole::DirectoryModel model;
      auto data = snapshot();
      test::editEntries(data, [&](auto& entries) { entries.push_back(entry(7, name)); });
      model.applySnapshot(data);
      model.select(7);
      luciole::ActionContext context{.model = model, .context = 1, .query = {}, .pinned = {}};
      const auto request = R::capture(context);
      for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
        TEST_CHECK(R::state(action, context, request).enabled == luciole::archiveCompressionAvailable(compression));
      }
      TEST_CHECK(luciole::archiveFolderName(data.entries.back().location()) == "package");
      const auto opaque = luciole::Location::fromPath("/tmp/opaque-container");
      TEST_CHECK(
          luciole::isSupportedArchive(
              opaque,
              bzip                                                   ? "application/x-bzip2-compressed-tar"
                  : compression == luciole::ArchiveCompression::Zstd ? "application/x-zstd-compressed-tar"
                                                                     : "application/x-xz-compressed-tar"
          )
          == luciole::archiveCompressionAvailable(compression)
      );
    }
    for (const char* const name : {"a.tbz2", "a.tbz", "a.tzst", "a.txz"}) {
      TEST_CHECK(luciole::archiveFolderName(luciole::Location::fromPath(std::string("/tmp/") + name)) == "a");
    }
    for (const auto& [name, compression] :
         {std::pair{"/tmp/plain.bz2", luciole::ArchiveCompression::Bzip2},
          std::pair{"/tmp/plain.zst", luciole::ArchiveCompression::Zstd},
          std::pair{"/tmp/plain.xz", luciole::ArchiveCompression::Xz}}) {
      TEST_CHECK(
          luciole::isSupportedArchive(luciole::Location::fromPath(name), {})
          == luciole::archiveCompressionAvailable(compression)
      );
    }
  }
  void testSearchTrashActions() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    data.entries = {entry(1, "match.txt"), entry(2, "match.txt")};
    test::editEntries(data, [](auto& entries) {
      entries[0].place = luciole::Location::fromPath("/tmp/action-fixture/first/match.txt");
      entries[1].place = luciole::Location::fromPath("/tmp/action-fixture/second/match.txt");
    });
    model.applySnapshot(data);
    model.selectAll();
    luciole::ActionContext context{.model = model, .context = 79, .query = "match", .pinned = {}};
    const auto request = R::capture(context);
    TEST_CHECK(request.targets.size() == 2 && R::state(A::Trash, context, request).enabled);
    TEST_CHECK(R::state(A::DeletePermanently, context, request).enabled);
    context.operationsBusy = true;
    TEST_CHECK(!R::state(A::Trash, context, request).enabled);
    context.operationsBusy = false;
    context.query = "different";
    TEST_CHECK(!R::state(A::Trash, context, request).enabled);
    context.query = "match";
    for (const auto field : {0, 1, 2, 3, 4}) {
      auto denied = data;
      ++denied.generation;
      test::editEntries(denied, [field](auto& entries) {
        if (field == 0) {
          entries[1].access.trash = false;
        }
        if (field == 1) {
          entries[1].metadataPending = true;
        }
        if (field == 2) {
          entries[1].metadataFailed = true;
        }
        if (field == 3) {
          entries[1].place = luciole::Location::fromUri("sftp://example.invalid/match.txt");
        }
        if (field == 4) {
          entries[1].place = luciole::Location::fromUri("trash:///match.txt");
        }
      });
      model.applySnapshot(denied);
      TEST_CHECK(!R::state(A::Trash, context, R::capture(context)).enabled);
      data.generation = denied.generation + 1;
      model.applySnapshot(data);
      model.selectAll();
    }
    test::editEntries(data, [](auto& entries) {
      entries[1].place = luciole::Location::fromPath("/tmp/action-fixture/second/replaced.txt");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Trash, context, request).enabled);
    model.clearSelection();
    TEST_CHECK(!R::state(A::Trash, context, R::capture(context)).enabled);
  }

  void testCapabilitiesAndTargets() {
    luciole::DirectoryModel model;
    auto data = snapshot();
    model.applySnapshot(data);
    model.select(2);
    luciole::ActionContext c{
        .model = model, .context = 4, .query = {}, .pinsEditable = true, .pinned = [](const luciole::Location&) {
          return false;
        }
    };
    const auto request = R::capture(c);
    TEST_CHECK(
        R::state(A::Open, c, request).enabled
        && R::state(A::Rename, c, request).enabled
        && R::state(A::Trash, c, request).enabled
        && R::state(A::NewDocument, c, request).enabled
    );
    model.select(3);
    TEST_CHECK(R::state(A::Rename, c, request).enabled && request.targets.front().id == 2);
    model.setSort({luciole::SortField::Name, true, false});
    TEST_CHECK(R::state(A::Open, c, request).enabled);
    c.operationsBusy = true;
    const auto busy = R::state(A::Rename, c, request);
    TEST_CHECK(!R::state(A::NewDocument, c, request).enabled);
    TEST_CHECK(!busy.enabled && !busy.reason.empty() && R::state(A::Properties, c, request).enabled);
    c.operationsBusy = false;
    data.metadata.filesystem.readOnly = true;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(
        !R::state(A::Rename, c, request).enabled
        && !R::state(A::CreateFile, c, request).enabled
        && !R::state(A::NewDocument, c, request).enabled
    );
    data.metadata.filesystem.readOnly = false;
    test::editEntries(data, [&](auto& entries) {
      entries[1].access.rename = false;
      entries[1].access.trash = false;
      entries[1].access.read = false;
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(
        !R::state(A::Rename, c, request).enabled
        && !R::state(A::Trash, c, request).enabled
        && !R::state(A::Open, c, request).enabled
    );
    test::editEntries(data, [&](auto& entries) { entries[1].access = {}; });
    data.metadata.access.write = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CreateFolder, c, request).enabled && !R::state(A::NewDocument, c, request).enabled);
    c.query = "txt";
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    auto search = R::capture(c);
    search.targets = request.targets;
    TEST_CHECK(R::state(A::Trash, c, search).enabled && R::state(A::Rename, c, search).enabled);
    c.query = {};
    ++c.context;
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    c.context = request.context;
    test::editEntries(data, [&](auto& entries) {
      entries[1].place = luciole::Location::fromPath("/tmp/action-fixture/renamed.txt");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    test::editEntries(data, [&](auto& entries) { entries.erase(entries.begin() + 1); });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Properties, c, request).enabled);
    test::editEntries(data, [&](auto& entries) { entries.push_back(entry(20, "Beta.txt")); });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    model.select(6);
    const auto folder = R::capture(c);
    TEST_CHECK(R::state(A::Pin, c, folder).enabled && !R::state(A::Unpin, c, folder).enabled);
    TEST_CHECK(R::state(A::PinCurrent, c, folder).enabled && !R::state(A::UnpinCurrent, c, folder).enabled);
    c.pinned = [](const luciole::Location&) { return true; };
    TEST_CHECK(!R::state(A::Pin, c, folder).enabled && R::state(A::Unpin, c, folder).enabled);
    TEST_CHECK(!R::state(A::PinCurrent, c, folder).enabled && R::state(A::UnpinCurrent, c, folder).enabled);
    auto networkFolder = folder;
    networkFolder.folder = luciole::Location::network();
    TEST_CHECK(
        !R::state(A::PinCurrent, c, networkFolder).enabled && !R::state(A::UnpinCurrent, c, networkFolder).enabled
    );
    auto unpinned = c;
    unpinned.pinned = [](const luciole::Location&) { return false; };
    TEST_CHECK(
        !R::state(A::PinCurrent, unpinned, networkFolder).enabled
        && !R::state(A::UnpinCurrent, unpinned, networkFolder).enabled
    );
    test::editEntries(data, [&](auto& entries) {
      for (auto& item : entries) {
        item.setIdentity(luciole::FileIdentity{"native", std::to_string(item.id)});
      }
    });
    ++data.generation;
    model.applySnapshot(data);
    model.selectAll();
    const auto multiple = R::capture(c);
    TEST_CHECK(
        !R::state(A::Open, c, multiple).enabled
        && R::state(A::Rename, c, multiple).enabled
        && R::state(A::Properties, c, multiple).enabled
    );
    test::editEntries(data, [&](auto& entries) {
      entries.front().place = luciole::Location::fromPath("/tmp/another-folder/alpha");
    });
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Rename, c, R::capture(c)).enabled);
    data.status = luciole::DirectoryStatus::Cancelled;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::SelectAll, c, R::capture(c)).enabled);
    auto remote = snapshot();
    remote.location = luciole::Location::fromUri("sftp://example.test/files");
    remote.entries = {entry(90, "remote")};
    test::editEntries(remote, [&](auto& entries) {
      entries.front().place = luciole::Location::fromUri("sftp://example.test/files/remote");
    });
    remote.generation = ++data.generation;
    model.applySnapshot(remote);
    model.select(90);
    TEST_CHECK(!R::state(A::Rename, c, R::capture(c)).enabled && R::state(A::Open, c, R::capture(c)).enabled);
  }
  void testRenameHistoryActions() {
    luciole::DirectoryModel model;
    model.applySnapshot(snapshot());
    luciole::ActionContext c{.model = model, .query = {}, .pinned = {}};
    const auto request = R::capture(c);
    TEST_CHECK(!R::state(A::Undo, c, request).enabled && !R::state(A::Redo, c, request).enabled);
    c.canUndo = true;
    TEST_CHECK(R::state(A::Undo, c, request).enabled && !R::state(A::Redo, c, request).enabled);
    c.operationsBusy = true;
    TEST_CHECK(!R::state(A::Undo, c, request).enabled);
    c.operationsBusy = false;
    c.canUndo = false;
    c.canRedo = true;
    ++c.context;
    TEST_CHECK(R::state(A::Redo, c, request).enabled);
    TEST_CHECK(luciole::shortcutMatches(A::Undo, XKB_KEY_z, KeyMod::Ctrl));
    TEST_CHECK(luciole::shortcutMatches(A::Redo, XKB_KEY_Z, KeyMod::Ctrl | KeyMod::Shift));
  }
  void testChooserGate() {
    luciole::DirectoryModel model;
    model.applySnapshot(snapshot());
    TEST_CHECK(model.select(1));
    luciole::ActionContext context{.model = model, .context = 7, .query = {}, .pinned = {}, .chooser = true};
    const auto request = R::capture(context);
    const auto trash = R::state(A::Trash, context, request);
    TEST_CHECK(!trash.enabled && trash.reason == "Not available while choosing files.");
    TEST_CHECK(R::state(A::Open, context, request).enabled);
    TEST_CHECK(R::state(A::SelectAll, context, request).enabled);
    TEST_CHECK(luciole::chooserAction(A::CopyUri) && !luciole::chooserAction(A::Miller));
    context.singleSelection = true;
    const auto all = R::state(A::SelectAll, context, request);
    TEST_CHECK(!all.enabled && all.reason == "Only one item can be chosen.");
    TEST_CHECK(!R::state(A::InvertSelection, context, request).enabled);
    TEST_CHECK(!R::state(A::SelectPattern, context, request).enabled);
    TEST_CHECK(R::state(A::ClearSelection, context, request).enabled);
  }

  void testBulkSelection() {
    luciole::DirectoryModel model;
    model.applySnapshot(snapshot());
    model.select(2);
    const auto focus = model.selection().focused, anchor = model.selection().anchor;
    model.invertSelection();
    TEST_CHECK(
        model.selection().selected.size() == 5
        && !model.selection().selected.contains(2)
        && model.selection().focused == focus
        && model.selection().anchor == anchor
    );
    model.invertSelection();
    TEST_CHECK(model.selectedIds() == std::vector<luciole::EntryId>{2});
    TEST_CHECK(
        !model.selectPattern("*.TXT")
        && model.selection().selected.size() == 4
        && model.selection().focused == focus
        && model.selection().anchor == anchor
    );
    TEST_CHECK(!model.selectPattern("CAFE\xCC\x81.???") && model.selectedIds() == std::vector<luciole::EntryId>{4});
    TEST_CHECK(!model.selectPattern("[literal].txt") && model.selectedIds() == std::vector<luciole::EntryId>{5});
    const auto before = model.selectedIds();
    TEST_CHECK(
        model.selectPattern("").has_value()
        && model.selectPattern(std::string(257, '*')).has_value()
        && model.selectPattern(std::string("a\0b", 3)).has_value()
        && model.selectPattern(std::string(1, '\xFF')).has_value()
        && model.selectedIds() == before
    );
    model.setFilter({false, "Beta"});
    model.selectPattern("*");
    TEST_CHECK(model.selectedIds() == std::vector<luciole::EntryId>{2});
    model.invertSelection();
    TEST_CHECK(model.selectedIds().empty());
  }
  void testTypeahead() {
    luciole::DirectoryModel model;
    model.applySnapshot(snapshot());
    luciole::TypeaheadSelection input;
    const auto now = luciole::TypeaheadSelection::Clock::now();
    TEST_CHECK(input.append(model, "b", now) == 2 && input.append(model, "B", now + 20ms) == 3 && input.text() == "b");
    TEST_CHECK(input.append(model, "e", now + 40ms) == 2);
    TEST_CHECK(!input.append(model, "x", now + 60ms) && model.selection().focused == 2);
    TEST_CHECK(input.erase(model, 1, now + 80ms) == 2 && input.text() == "be");
    TEST_CHECK(input.append(model, "caf", now + 2s) == 4 && input.text() == "caf");
    TEST_CHECK(input.append(model, "é", now + 4s, true) == 4 && input.text() == "café");
    const auto prior = input.text();
    input.erase(model, 1, now + 4100ms);
    TEST_CHECK(input.text() == prior);
    TEST_CHECK(input.erase(model, 2, now + 4200ms) == 4 && input.text() == "caf");
    TEST_CHECK(input.expire(now + 6s) && input.text().empty());
    TEST_CHECK(!input.append(model, std::string(1, '\xFF'), now + 7s) && input.text().empty());
    model.setFilter({false, "alpha"});
    TEST_CHECK(!input.append(model, "b", now + 8s));
  }
  void testActionTable() {
    TEST_CHECK(R::definitions().size() == luciole::kBrowserActionCount);
    for (std::size_t index = 0; index < luciole::kBrowserActionCount; ++index) {
      const auto action = R::definitions()[index].action;
      TEST_CHECK(&R::definition(action) == &R::definitions()[index]);
      TEST_CHECK(R::fromMenuId(R::menuId(action)) == action);
    }
    TEST_CHECK(R::definition(A::MenuBack).label == "Back");
    TEST_CHECK(!R::fromMenuId(-1));
    TEST_CHECK(!R::fromMenuId(static_cast<int>(luciole::kBrowserActionCount)));
    TEST_CHECK(!R::fromMenuId(9000));
  }
  void testShortcuts() {
    for (const auto& definition : R::definitions()) {
      TEST_CHECK(luciole::actionShortcutLabel(definition.action).empty() == !definition.keybind);
    }
    TEST_CHECK(luciole::shortcutMatches(A::Settings, XKB_KEY_comma, KeyMod::Ctrl));
    TEST_CHECK(luciole::shortcutMatches(A::Back, XKB_KEY_Left, KeyMod::Alt));
    TEST_CHECK(luciole::shortcutMatches(A::Open, XKB_KEY_Return, 0));
    TEST_CHECK(luciole::shortcutMatches(A::QuickPreview, XKB_KEY_space, 0));
    TEST_CHECK(luciole::shortcutMatches(A::Properties, XKB_KEY_Return, KeyMod::Alt));
    TEST_CHECK(luciole::shortcutMatches(A::EditLocation, XKB_KEY_l, KeyMod::Ctrl));
    TEST_CHECK(
        luciole::shortcutMatches(A::CreateFolder, XKB_KEY_N, KeyMod::Ctrl | KeyMod::Shift)
        && luciole::shortcutMatches(A::CreateFolder, XKB_KEY_n, KeyMod::Ctrl | KeyMod::Shift)
        && !luciole::shortcutMatches(A::CreateFolder, XKB_KEY_n, KeyMod::Ctrl)
    );
    TEST_CHECK(
        luciole::shortcutMatches(A::CopyFiles, XKB_KEY_c, KeyMod::Ctrl)
        && !luciole::shortcutMatches(A::CopyFiles, XKB_KEY_C, KeyMod::Ctrl)
        && !luciole::shortcutMatches(A::CopyFiles, XKB_KEY_c, KeyMod::Ctrl | KeyMod::Alt)
    );
    TEST_CHECK(luciole::shortcutMatches(A::Terminal, XKB_KEY_F4, 0));
    TEST_CHECK(!luciole::shortcutMatches(A::Terminal, XKB_KEY_F4, KeyMod::Shift));
    TEST_CHECK(!luciole::shortcutMatches(A::Terminal, XKB_KEY_F4, KeyMod::Ctrl));
    TEST_CHECK(!luciole::shortcutMatches(A::Root, 0, 0));
    TEST_CHECK(luciole::shortcutAction({A::Grid, A::List, A::Miller}, XKB_KEY_2, KeyMod::Ctrl) == A::List);
    TEST_CHECK(luciole::shortcutAction({A::DeletePermanently, A::Trash}, XKB_KEY_Delete, 0) == A::Trash);
    TEST_CHECK(
        luciole::shortcutAction({A::DeletePermanently, A::Trash}, XKB_KEY_Delete, KeyMod::Shift) == A::DeletePermanently
    );
    TEST_CHECK(!luciole::shortcutAction({A::DeletePermanently, A::Trash}, XKB_KEY_Delete, KeyMod::Ctrl));
    TEST_CHECK(!luciole::shortcutAction({A::Back, A::Forward}, XKB_KEY_Left, 0));
    // Actions follow the active keybind config.
    {
      auto rebind = luciole::KeybindConfig{};
      luciole::setKeybindChord(rebind, luciole::KeybindCommand::Search, 0, luciole::parseKeybindChord("Ctrl+J"));
      luciole::setKeybindChord(rebind, luciole::KeybindCommand::Search, 1, std::nullopt);
      luciole::setActiveKeybinds(rebind);
      TEST_CHECK(luciole::shortcutMatches(A::Search, XKB_KEY_j, KeyMod::Ctrl));
      TEST_CHECK(!luciole::shortcutMatches(A::Search, XKB_KEY_f, KeyMod::Ctrl));
      TEST_CHECK(luciole::actionShortcutLabel(A::Search) == "Ctrl+J");
      luciole::setActiveKeybinds(luciole::KeybindConfig{});
    }
    TEST_CHECK(luciole::shortcutMatches(A::Search, XKB_KEY_f, KeyMod::Ctrl));
  }
  void testClipboardShortcutBindings() {
    using K = luciole::KeybindCommand;
    TEST_CHECK(luciole::shortcutMatches(A::CopyTargetPath, XKB_KEY_c, KeyMod::Ctrl | KeyMod::Alt));
    TEST_CHECK(luciole::shortcutMatches(A::CopyTargetUri, XKB_KEY_u, KeyMod::Ctrl | KeyMod::Alt));
    TEST_CHECK(!luciole::shortcutMatches(A::CopyTargetPath, XKB_KEY_c, KeyMod::Ctrl));
    auto rebound = luciole::KeybindConfig{};
    luciole::setKeybindChord(rebound, K::CopyTargetPath, 0, luciole::parseKeybindChord("Alt+P"));
    luciole::setKeybindChord(rebound, K::CopyTargetUri, 0, std::nullopt);
    luciole::setActiveKeybinds(rebound);
    TEST_CHECK(luciole::shortcutMatches(A::CopyTargetPath, XKB_KEY_p, KeyMod::Alt));
    TEST_CHECK(!luciole::shortcutMatches(A::CopyTargetPath, XKB_KEY_c, KeyMod::Ctrl | KeyMod::Alt));
    TEST_CHECK(luciole::actionShortcutLabel(A::CopyTargetPath) == "Alt+P");
    TEST_CHECK(!luciole::shortcutMatches(A::CopyTargetUri, XKB_KEY_u, KeyMod::Ctrl | KeyMod::Alt));
    TEST_CHECK(luciole::actionShortcutLabel(A::CopyTargetUri).empty());
    luciole::setActiveKeybinds(luciole::KeybindConfig{});
  }

} // namespace
int main() {
  try {
    testActionTable();
    testShortcuts();
    testClipboardShortcutBindings();
    testPointerActivation();
    testTerminalActions();
    testArchiveActions();
    testBatchArchiveActions();
    testSplitArchiveActions();
    testCompressedArchiveActions();
    testExecutionActions();
    testTrashActions();
    testPermanentDeletionMenuEntries();
    testEmptyTrashAvailability();
    testLinkActions();
    testFileClipboardActions();
    testPendingActionFacts();
    testPreviewActions();
    testCapabilitiesAndTargets();
    testSearchTrashActions();
    testRenameHistoryActions();
    testBulkSelection();
    testChooserGate();
    testTypeahead();
    std::println("browser action checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "browser action test: {}", error.what());
    return 1;
  }
}
