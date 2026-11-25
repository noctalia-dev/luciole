#include "app/operation_hub.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "operations/copy_receipt.h"
#include "operations/trash_item.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::ActivityState;
  struct Fixture {
    fs::path root;
    explicit Fixture(const fs::path& parent = {}) {
      if (parent.empty()) {
        const luciole::GString path(g_dir_make_tmp("luciole-activity-XXXXXX", nullptr));
        TEST_CHECK(path != nullptr);
        root = path.get();
      } else {
        auto pattern = (parent / "luciole-activity-XXXXXX").native();
        TEST_CHECK(mkdtemp(pattern.data()));
        root = pattern;
      }
      fs::create_directory(root / "destination");
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
    luciole::Location at(const std::string& name) const { return luciole::Location::fromPath((root / name).native()); }
    void write(const std::string& name) const { std::ofstream(root / name) << name; }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        m_source.dispatch(fds, first);
      }
    }

  private:
    GlibPollSource m_source;
  };
  struct Owner final : OperationHub::Owner {
    unsigned changes = 0;
    void jobChanged(OperationJob&) override {}
    void transfersFinished(const std::vector<luciole::TransferResult>&) override {}
    void activityChanged() override { ++changes; }
  };
  struct Workspace {
    luciole::TabWorkspace model;
    luciole::WindowId window = model.addWindow();
    luciole::TabId page = model.addTab(window);
    OperationHub hub{model};
    Owner owner;
    Workspace() { hub.attach(page, owner); }
    ~Workspace() { hub.detach(page); }
  };
  luciole::DirectoryEntry entry(const luciole::Location& location) {
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(location).get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info);
    luciole::DirectoryEntry result{
        .place = location,
        .name = *location.basename(),
        .type = luciole::fileType(g_file_info_get_file_type(info.get())),
        .symlink = g_file_info_get_is_symlink(info.get()) != FALSE,
        .size = {},
        .modifiedSeconds = {}
    };
    result.setDisplayName(*location.basename());
    result.setIdentity(luciole::readFileIdentity(info.get()));
    return result;
  }
  void batchRenameHistory(Pump& pump) {
    Fixture fixture;
    fixture.write("a");
    fixture.write("b");
    Workspace workspace;
    const auto otherWindow = workspace.model.addWindow();
    const auto other = workspace.model.addTab(otherWindow);
    Owner otherOwner;
    workspace.hub.attach(other, otherOwner);
    auto job = workspace.hub.create<luciole::BatchRenameService>(workspace.page);
    auto& service = job->service<luciole::BatchRenameService>();
    TEST_CHECK(service.prepare({entry(fixture.at("a")), entry(fixture.at("b"))}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
    TEST_CHECK(service.apply());
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.batchRenameHistory());
    TEST_CHECK(!workspace.hub.canRedo());
    job.reset();
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(otherWindow);
    TEST_CHECK(workspace.hub.canUndo());
    fixture.write("b");
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    TEST_CHECK(workspace.hub.historyReplayBusy() && !workspace.hub.canUndo());
    const auto undoActivity = workspace.hub.activities().front();
    TEST_CHECK(undoActivity.title == "Undo batch rename");
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && !workspace.hub.canRedo());
    TEST_CHECK(fs::exists(fixture.root / "a_001") && !fs::exists(fixture.root / "a"));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Failed);
    TEST_CHECK(fs::remove(fixture.root / "b"));
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndo() && workspace.hub.canRedo());
    TEST_CHECK(fs::exists(fixture.root / "a") && fs::exists(fixture.root / "b"));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    TEST_CHECK(workspace.hub.activities().front().title == "Redo batch rename");
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && !workspace.hub.canRedo());
    TEST_CHECK(fs::exists(fixture.root / "a_001") && fs::exists(fixture.root / "b_002"));
    fixture.write("b_002");
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
    workspace.hub.detach(other);
  }

  void renameHistoryReplacement(Pump& pump) {
    Fixture fixture;
    fixture.write("a");
    fixture.write("b");
    fixture.write("c");
    fixture.write("d");
    Workspace workspace;
    auto first = workspace.hub.create<luciole::BatchRenameService>(workspace.page);
    auto second = workspace.hub.create<luciole::BatchRenameService>(workspace.page);
    TEST_CHECK(first->service<luciole::BatchRenameService>().prepare({entry(fixture.at("a")), entry(fixture.at("b"))}));
    TEST_CHECK(
        second->service<luciole::BatchRenameService>().prepare({entry(fixture.at("c")), entry(fixture.at("d"))})
    );
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(first->service<luciole::BatchRenameService>().apply());
    TEST_CHECK(second->service<luciole::BatchRenameService>().apply());
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo());
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(fs::exists(fixture.root / "a_001") && fs::exists(fixture.root / "b_002"));
    TEST_CHECK(fs::exists(fixture.root / "c") && fs::exists(fixture.root / "d"));
    TEST_CHECK(workspace.hub.canRedo());
    auto single = workspace.hub.create<luciole::RenameService>(workspace.page);
    TEST_CHECK(single->service<luciole::RenameService>().prepare(fixture.at("c")));
    pump.until([&] { return !single->running(); });
    TEST_CHECK(workspace.hub.canRedo());
    TEST_CHECK(single->service<luciole::RenameService>().rename("single"));
    TEST_CHECK(!workspace.hub.canRedo());
    pump.until([&] { return !single->running(); });
    TEST_CHECK(workspace.hub.canUndo() && !workspace.hub.batchRenameHistory());
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(fs::exists(fixture.root / "c"));
  }
  void creationHistory(Pump& pump) {
    Fixture fixture;
    Workspace workspace;
    auto job = workspace.hub.create<luciole::CreateService>(workspace.page);
    auto& service = job->service<luciole::CreateService>();
    TEST_CHECK(service.prepare(fixture.at("destination"), luciole::CreateKind::EmptyFile));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.create("created"));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.historyLabel(false) == "Undo Create File");
    const auto otherWindow = workspace.model.addWindow();
    const auto other = workspace.model.addTab(otherWindow);
    Owner otherOwner;
    workspace.hub.attach(other, otherOwner);
    job.reset();
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(otherWindow);
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    TEST_CHECK(workspace.hub.historyReplayBusy() && !workspace.hub.canUndo());
    const auto activity = workspace.hub.activities().front();
    TEST_CHECK(activity.title == "Undo: Create file" && activity.canCancel);
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!fs::exists(fixture.root / "destination/created") && workspace.hub.canRedo());
    fs::create_symlink("missing", fixture.root / "destination/created");
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canRedo() && fs::read_symlink(fixture.root / "destination/created") == "missing");
    TEST_CHECK(fs::remove(fixture.root / "destination/created"));
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && fs::is_regular_file(fixture.root / "destination/created"));
    fixture.write("destination/created");
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
    TEST_CHECK(fs::is_regular_file(fixture.root / "destination/created"));
    workspace.hub.detach(other);
  }

  void creationHistoryReplacement(Pump& pump) {
    Fixture fixture;
    fixture.write("source");
    Workspace workspace;
    auto first = workspace.hub.create<luciole::CreateService>(workspace.page);
    auto second = workspace.hub.create<luciole::CreateService>(workspace.page);
    TEST_CHECK(
        first->service<luciole::CreateService>().prepare(fixture.at("destination"), luciole::CreateKind::Folder)
    );
    TEST_CHECK(
        second->service<luciole::CreateService>().prepare(fixture.at("destination"), luciole::CreateKind::EmptyFile)
    );
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(first->service<luciole::CreateService>().create("older"));
    TEST_CHECK(second->service<luciole::CreateService>().create("newer"));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.historyLabel(false) == "Undo Create File");
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(fs::is_directory(fixture.root / "destination/older") && !fs::exists(fixture.root / "destination/newer"));
    TEST_CHECK(workspace.hub.canRedo());
    auto rename = workspace.hub.create<luciole::RenameService>(workspace.page);
    TEST_CHECK(rename->service<luciole::RenameService>().prepare(fixture.at("source")));
    pump.until([&] { return !rename->running(); });
    TEST_CHECK(workspace.hub.canRedo());
    TEST_CHECK(rename->service<luciole::RenameService>().rename("renamed"));
    TEST_CHECK(!workspace.hub.canRedo());
    pump.until([&] { return !rename->running(); });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.historyLabel(false) == "Undo Rename");
    auto create = workspace.hub.create<luciole::CreateService>(workspace.page);
    TEST_CHECK(
        create->service<luciole::CreateService>().prepare(fixture.at("destination"), luciole::CreateKind::Folder)
    );
    pump.until([&] { return !create->running(); });
    TEST_CHECK(workspace.hub.canUndo());
    TEST_CHECK(create->service<luciole::CreateService>().create("last"));
    pump.until([&] { return !create->running(); });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.historyLabel(false) == "Undo Create Folder");
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!fs::exists(fixture.root / "destination/last") && fs::exists(fixture.root / "renamed"));
  }

  void moveHistory(Pump& pump) {
    Fixture fixture;
    fixture.write("a");
    fixture.write("b");
    Workspace workspace;
    const auto otherWindow = workspace.model.addWindow();
    const auto other = workspace.model.addTab(otherWindow);
    Owner otherOwner;
    workspace.hub.attach(other, otherOwner);
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    TEST_CHECK(job->service<luciole::TransferService>().start(
        {{fixture.at("a"), fixture.at("b")}, fixture.at("destination"), luciole::TransferAction::Move}
    ));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.historyLabel(false) == "Undo Move");
    TEST_CHECK(!job->service<luciole::TransferService>().receipt());
    job.reset();
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(otherWindow);
    fixture.write("b");
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    TEST_CHECK(workspace.hub.historyReplayBusy() && !workspace.hub.canUndo());
    TEST_CHECK(workspace.hub.activities().front().title == "Undo: Move files");
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && !workspace.hub.canRedo());
    TEST_CHECK(!fs::exists(fixture.root / "a") && fs::exists(fixture.root / "destination/a"));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Failed);
    TEST_CHECK(fs::remove(fixture.root / "b"));
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canRedo() && workspace.hub.historyLabel(true) == "Redo Move");
    TEST_CHECK(fs::exists(fixture.root / "a") && fs::exists(fixture.root / "b"));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    TEST_CHECK(workspace.hub.activities().front().title == "Redo: Move files");
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && !workspace.hub.canRedo());
    fixture.write("destination/b");
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
    TEST_CHECK(fs::exists(fixture.root / "destination/a") && fs::exists(fixture.root / "destination/b"));
    workspace.hub.detach(other);
  }

  void moveHistoryReplacement(Pump& pump) {
    Fixture fixture;
    fixture.write("a");
    fixture.write("b");
    Workspace workspace;
    auto first = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto second = workspace.hub.create<luciole::TransferService>(workspace.page);
    TEST_CHECK(first->service<luciole::TransferService>().start(
        {{fixture.at("a")}, fixture.at("destination"), luciole::TransferAction::Move}
    ));
    TEST_CHECK(second->service<luciole::TransferService>().start(
        {{fixture.at("b")}, fixture.at("destination"), luciole::TransferAction::Move}
    ));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.historyLabel(false) == "Undo Move");
    TEST_CHECK(
        !first->service<luciole::TransferService>().receipt() && !second->service<luciole::TransferService>().receipt()
    );
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(fs::exists(fixture.root / "destination/a") && fs::exists(fixture.root / "b"));
    TEST_CHECK(workspace.hub.canRedo());
    auto create = workspace.hub.create<luciole::CreateService>(workspace.page);
    TEST_CHECK(
        create->service<luciole::CreateService>().prepare(fixture.at("destination"), luciole::CreateKind::Folder)
    );
    pump.until([&] { return !create->running(); });
    TEST_CHECK(workspace.hub.canRedo());
    TEST_CHECK(create->service<luciole::CreateService>().create("newer"));
    TEST_CHECK(!workspace.hub.canRedo());
    pump.until([&] { return !create->running(); });
    TEST_CHECK(workspace.hub.historyLabel(false) == "Undo Create Folder");
    auto copy = workspace.hub.create<luciole::TransferService>(workspace.page);
    TEST_CHECK(copy->service<luciole::TransferService>().start(
        {{fixture.at("b")}, fixture.at("destination"), luciole::TransferAction::Copy}
    ));
    pump.until([&] { return !copy->running(); });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.historyLabel(false) == "Undo Copy");
  }

  void copyHistory(Pump& pump) {
    for (const auto action : {luciole::TransferAction::Copy, luciole::TransferAction::Link}) {
      Fixture fixture;
      fixture.write("a");
      fs::create_directory(fixture.root / "folder");
      fixture.write("folder/inside");
      Workspace workspace;
      const auto otherWindow = workspace.model.addWindow();
      const auto other = workspace.model.addTab(otherWindow);
      Owner otherOwner;
      workspace.hub.attach(other, otherOwner);
      auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
      TEST_CHECK(job->service<luciole::TransferService>().start(
          {{fixture.at("a"), fixture.at("folder")}, fixture.at("destination"), action}
      ));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canUndo() && !job->service<luciole::TransferService>().copyReceipt());
      TEST_CHECK(
          workspace.hub.historyLabel(false)
          == (action == luciole::TransferAction::Copy ? "Undo Copy" : "Undo Create Links")
      );
      job.reset();
      TEST_CHECK(workspace.model.close(workspace.page));
      workspace.hub.detach(workspace.page);
      workspace.hub.update(otherWindow);
      TEST_CHECK(workspace.hub.replayHistory(other, false));
      TEST_CHECK(
          workspace.hub.activities().front().title
          == (action == luciole::TransferAction::Copy ? "Undo: Copy files" : "Undo: Create links")
      );
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "destination/a"));
      auto* undo =
          workspace.hub.newest(other, [](const OperationJob& item) { return item.kind() == JobKind::Transfer; });
      TEST_CHECK(undo);
      const fs::path retained = *undo->service<luciole::TransferService>().results()[0].destination->nativePath();
      TEST_CHECK(fs::exists(retained));
      fixture.write("destination/a");
      TEST_CHECK(workspace.hub.replayHistory(other, true));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "destination/folder"));
      TEST_CHECK(fs::remove(fixture.root / "destination/a"));
      TEST_CHECK(workspace.hub.replayHistory(other, true));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canUndo() && fs::exists(fixture.root / "destination/folder"));
      TEST_CHECK(workspace.hub.replayHistory(other, false));
      pump.until([&] { return !workspace.hub.busy(); });
      undo = workspace.hub.newest(other, [](const OperationJob& item) { return item.kind() == JobKind::Transfer; });
      const fs::path discarded = *undo->service<luciole::TransferService>().results()[0].destination->nativePath();
      auto create = workspace.hub.create<luciole::CreateService>(other);
      TEST_CHECK(
          create->service<luciole::CreateService>().prepare(fixture.at("destination"), luciole::CreateKind::Folder)
      );
      pump.until([&] { return !create->running(); });
      TEST_CHECK(workspace.hub.canRedo());
      TEST_CHECK(create->service<luciole::CreateService>().create("newer"));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(!fs::exists(discarded.parent_path()) && fs::exists(fixture.root / "a"));
      TEST_CHECK(workspace.hub.historyLabel(false) == "Undo Create Folder");
      workspace.hub.shutdown();
      pump.until([&] { return !workspace.hub.busy(); });
      workspace.hub.detach(other);
    }
  }
  void overwriteHistory(Pump& pump, const fs::path& parent = {}) {
    for (const auto action :
         {luciole::TransferAction::Copy, luciole::TransferAction::Move, luciole::TransferAction::Link}) {
      for (bool cross : {false, true}) {
        if (cross && (!parent.empty() || ::access("/dev/shm", W_OK) != 0)) {
          continue;
        }
        for (unsigned shape = 0; shape < 5; ++shape) {
          Fixture source(parent), target(cross ? fs::path("/dev/shm") : parent);
          const auto incoming = source.root / "item", destination = target.root / "destination/item";
          const bool folder = shape < 4 && (shape & 1U), oldFolder = shape < 4 && (shape & 2U);
          if (folder) {
            fs::create_directory(incoming);
            std::ofstream(incoming / "new") << "incoming bytes";
          } else if (shape == 4) {
            fs::create_symlink("missing-new", incoming);
          } else {
            std::ofstream(incoming) << "incoming bytes";
          }
          if (oldFolder) {
            fs::create_directory(destination);
            std::ofstream(destination / "old") << "old bytes";
          } else if (shape == 4) {
            fs::create_symlink("missing-old", destination);
          } else {
            std::ofstream(destination) << "old bytes";
          }
          if (shape == 1) {
            fs::create_hard_link(destination, target.root / "outside-alias");
          }
          const auto old = luciole::checkedFd(::open(destination.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
          std::ofstream(source.root / "canary") << "unrelated bytes";
          Workspace workspace;
          auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
          auto& service = job->service<luciole::TransferService>();
          TEST_CHECK(service.start({{source.at("item")}, target.at("destination"), action}));
          std::uint64_t answered = 0;
          pump.until([&] {
            if (const auto conflict = service.conflict(); conflict && conflict->id != answered) {
              answered = conflict->id;
              TEST_CHECK(service.resolve(answered, luciole::TransferConflictChoice::Replace));
            }
            return !workspace.hub.busy();
          });
          TEST_CHECK(workspace.hub.canUndo());
          TEST_CHECK(luciole::statFd(old.get()).st_nlink == (shape == 1 ? 1U : 0U));
          const auto noBackups = [&] {
            for (const auto& entry : fs::recursive_directory_iterator(target.root)) {
              TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-"));
            }
          };
          const auto verify = [&](const fs::path& path, bool linked) {
            if (linked) {
              TEST_CHECK(fs::read_symlink(path) == incoming);
            } else if (folder) {
              std::string bytes;
              std::getline(std::ifstream(path / "new"), bytes);
              TEST_CHECK(bytes == "incoming bytes" && !fs::exists(path / "old"));
            } else if (shape == 4) {
              TEST_CHECK(fs::read_symlink(path) == "missing-new");
            } else {
              std::string bytes;
              std::getline(std::ifstream(path), bytes);
              TEST_CHECK(bytes == "incoming bytes");
            }
          };
          noBackups();
          verify(destination, action == luciole::TransferAction::Link);
          if (shape == 1) {
            std::ofstream(target.root / "outside-alias") << "edited outside alias";
          }
          for (int cycle = 0; cycle < 2; ++cycle) {
            TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
            pump.until([&] { return !workspace.hub.busy(); });
            TEST_CHECK(workspace.hub.canRedo());
            TEST_CHECK(fs::symlink_status(destination).type() == fs::file_type::not_found);
            verify(incoming, false);
            TEST_CHECK(workspace.hub.replayHistory(workspace.page, true));
            pump.until([&] { return !workspace.hub.busy(); });
            TEST_CHECK(workspace.hub.canUndo());
            verify(destination, action == luciole::TransferAction::Link);
            noBackups();
          }
          std::string bytes;
          std::getline(std::ifstream(source.root / "canary"), bytes);
          TEST_CHECK(bytes == "unrelated bytes");
          workspace.hub.shutdown();
          pump.until([&] { return !workspace.hub.busy(); });
          workspace.hub.detach(workspace.page);
          noBackups();
        }
      }
    }
  }

  void replacementMergeHistory(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "tree");
    fs::create_directory(fixture.root / "destination/tree");
    fixture.write("tree/document");
    fixture.write("tree/new");
    fixture.write("destination/tree/document");
    fixture.write("destination/tree/keep");
    Workspace workspace;
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    TEST_CHECK(service.start({{fixture.at("tree")}, fixture.at("destination"), luciole::TransferAction::Copy}));
    std::uint64_t answeredConflict = 0;
    pump.until([&] {
      if (const auto conflict = service.conflict(); conflict && conflict->id != answeredConflict) {
        answeredConflict = conflict->id;
        TEST_CHECK(service.resolve(
            conflict->id,
            conflict->canMerge ? luciole::TransferConflictChoice::Merge : luciole::TransferConflictChoice::Replace
        ));
      }
      return !workspace.hub.busy();
    });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.historyLabel(false) == "Undo Copy");
    const auto otherWindow = workspace.model.addWindow();
    const auto other = workspace.model.addTab(otherWindow);
    Owner otherOwner;
    workspace.hub.attach(other, otherOwner);
    job.reset();
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(otherWindow);
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "destination/tree/new"));
    TEST_CHECK(fs::exists(fixture.root / "destination/tree/keep"));
    TEST_CHECK(workspace.hub.activities().front().title == "Undo: Copy files");
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && fs::exists(fixture.root / "destination/tree/new"));
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!fs::exists(fixture.root / "destination/tree/document"));
    fixture.write("destination/tree/document");
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndo() && workspace.hub.canRedo());
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Failed);
    TEST_CHECK(!fs::exists(fixture.root / "destination/tree/new"));
    TEST_CHECK(fs::exists(fixture.root / "tree/new"));
    TEST_CHECK(fs::remove(fixture.root / "destination/tree/document"));
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && fs::exists(fixture.root / "destination/tree/new"));
    workspace.hub.shutdown();
    pump.until([&] { return !workspace.hub.busy(); });
    workspace.hub.detach(other);
  }

  void moveReplacementMergeHistory(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "tree");
    fs::create_directory(fixture.root / "destination/tree");
    fixture.write("tree/document");
    fixture.write("tree/new");
    fixture.write("destination/tree/document");
    fixture.write("destination/tree/keep");
    Workspace workspace;
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    TEST_CHECK(service.start({{fixture.at("tree")}, fixture.at("destination"), luciole::TransferAction::Move}));
    std::uint64_t answeredConflict = 0;
    pump.until([&] {
      if (const auto conflict = service.conflict(); conflict && conflict->id != answeredConflict) {
        answeredConflict = conflict->id;
        TEST_CHECK(service.resolve(
            conflict->id,
            conflict->canMerge ? luciole::TransferConflictChoice::Merge : luciole::TransferConflictChoice::Replace
        ));
      }
      return !workspace.hub.busy();
    });
    TEST_CHECK(workspace.hub.canUndo() && workspace.hub.historyLabel(false) == "Undo Move");
    const auto otherWindow = workspace.model.addWindow();
    const auto other = workspace.model.addTab(otherWindow);
    Owner otherOwner;
    workspace.hub.attach(other, otherOwner);
    job.reset();
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(otherWindow);
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "destination/tree/new"));
    TEST_CHECK(fs::exists(fixture.root / "destination/tree/keep"));
    TEST_CHECK(workspace.hub.activities().front().title == "Undo: Move files");
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(
        workspace.hub.canUndo()
        && fs::exists(fixture.root / "destination/tree/new")
        && !fs::exists(fixture.root / "tree")
    );
    TEST_CHECK(workspace.hub.replayHistory(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!fs::exists(fixture.root / "destination/tree/document"));
    fixture.write("destination/tree/document");
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndo() && workspace.hub.canRedo());
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Failed);
    TEST_CHECK(!fs::exists(fixture.root / "destination/tree/new"));
    TEST_CHECK(fs::exists(fixture.root / "tree/new"));
    TEST_CHECK(fs::remove(fixture.root / "destination/tree/document"));
    TEST_CHECK(workspace.hub.replayHistory(other, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && fs::exists(fixture.root / "destination/tree/new"));
    workspace.hub.shutdown();
    pump.until([&] { return !workspace.hub.busy(); });
    workspace.hub.detach(other);
  }

  void repeatedTransferHistory(Pump& pump) {
    for (const auto action : {luciole::TransferAction::Copy, luciole::TransferAction::Move}) {
      Fixture fixture;
      fs::create_directory(fixture.root / "one");
      fs::create_directory(fixture.root / "two");
      fixture.write("one/item");
      fixture.write("two/item");
      fixture.write("destination/item");
      fs::create_hard_link(fixture.root / "destination/item", fixture.root / "alias");
      Workspace workspace;
      auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
      auto& service = job->service<luciole::TransferService>();
      TEST_CHECK(service.start({{fixture.at("one/item"), fixture.at("two/item")}, fixture.at("destination"), action}));
      std::uint64_t answeredConflict = 0;
      pump.until([&] {
        if (const auto conflict = service.conflict(); conflict && conflict->id != answeredConflict) {
          answeredConflict = conflict->id;
          TEST_CHECK(service.resolve(conflict->id, luciole::TransferConflictChoice::Replace));
        }
        return !workspace.hub.busy();
      });
      if (action == luciole::TransferAction::Move) {
        TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
        TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
        workspace.hub.shutdown();
        pump.until([&] { return !workspace.hub.busy(); });
        workspace.hub.detach(workspace.page);
        continue;
      }
      TEST_CHECK(workspace.hub.canUndo());
      const auto otherWindow = workspace.model.addWindow(), other = workspace.model.addTab(otherWindow);
      Owner otherOwner;
      workspace.hub.attach(other, otherOwner);
      job.reset();
      TEST_CHECK(workspace.model.close(workspace.page));
      workspace.hub.detach(workspace.page);
      workspace.hub.update(otherWindow);
      TEST_CHECK(workspace.hub.replayHistory(other, false));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "destination/item"));
      TEST_CHECK(fs::exists(fixture.root / "one/item") && fs::exists(fixture.root / "two/item"));
      TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
      TEST_CHECK(workspace.hub.replayHistory(other, true));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canUndo());
      TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
      workspace.hub.shutdown();
      pump.until([&] { return !workspace.hub.busy(); });
      workspace.hub.detach(other);
      for (const auto& path : fs::recursive_directory_iterator(fixture.root)) {
        TEST_CHECK(!path.path().filename().native().starts_with(".luciole-create-"));
      }
      std::ifstream input(fixture.root / "alias");
      std::string contents;
      std::getline(input, contents);
      TEST_CHECK(contents == "destination/item");
    }
  }
  void overlappingTransferHistory(Pump& pump) {
    for (const auto action : {luciole::TransferAction::Copy, luciole::TransferAction::Move}) {
      Fixture fixture;
      fs::create_directories(fixture.root / "one/tree/sub");
      fs::create_directories(fixture.root / "two/tree/sub");
      fixture.write("one/tree/sub/item");
      fixture.write("one/tree/keep");
      fixture.write("two/tree/sub/item");
      fixture.write("two/tree/new");
      Workspace workspace;
      auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
      auto& service = job->service<luciole::TransferService>();
      TEST_CHECK(service.start({{fixture.at("one/tree"), fixture.at("two/tree")}, fixture.at("destination"), action}));
      std::uint64_t answeredConflict = 0;
      pump.until([&] {
        if (const auto conflict = service.conflict(); conflict && conflict->id != answeredConflict) {
          answeredConflict = conflict->id;
          TEST_CHECK(service.resolve(
              conflict->id,
              conflict->canMerge ? luciole::TransferConflictChoice::Merge : luciole::TransferConflictChoice::Replace
          ));
        }
        return !workspace.hub.busy();
      });
      if (action == luciole::TransferAction::Move) {
        TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
        TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
        workspace.hub.shutdown();
        pump.until([&] { return !workspace.hub.busy(); });
        workspace.hub.detach(workspace.page);
        continue;
      }
      TEST_CHECK(
          workspace.hub.canUndo()
          && workspace.hub.historyLabel(false) == (action == luciole::TransferAction::Copy ? "Undo Copy" : "Undo Move")
      );
      const auto otherWindow = workspace.model.addWindow(), other = workspace.model.addTab(otherWindow);
      Owner otherOwner;
      workspace.hub.attach(other, otherOwner);
      job.reset();
      TEST_CHECK(workspace.model.close(workspace.page));
      workspace.hub.detach(workspace.page);
      workspace.hub.update(otherWindow);
      TEST_CHECK(workspace.hub.replayHistory(other, false));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "destination/tree"));
      TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
      TEST_CHECK(workspace.hub.replayHistory(other, true));
      pump.until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canUndo() && fs::exists(fixture.root / "destination/tree/keep"));
      TEST_CHECK(fs::exists(fixture.root / "destination/tree/new"));
      TEST_CHECK(fs::exists(fixture.root / "one/tree/sub/item") == (action == luciole::TransferAction::Copy));
      TEST_CHECK(fs::exists(fixture.root / "two/tree/sub/item") == (action == luciole::TransferAction::Copy));
      TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
      workspace.hub.shutdown();
      pump.until([&] { return !workspace.hub.busy(); });
      workspace.hub.detach(other);
      for (const auto& entry : fs::recursive_directory_iterator(fixture.root)) {
        TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
      }
    }
  }

  void ancestorTransferHistory(Pump& pump, luciole::TransferAction action) {
    Fixture fixture;
    for (const auto* name : {"one/tree", "two/tree", "destination/tree"}) {
      fs::create_directories(fixture.root / name);
    }
    fixture.write("one/tree/item");
    fixture.write("two/tree/final");
    fixture.write("destination/tree/item");
    fixture.write("destination/tree/keep");
    fs::create_hard_link(fixture.root / "destination/tree/item", fixture.root / "alias");
    Workspace workspace;
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    TEST_CHECK(service.start({{fixture.at("one/tree"), fixture.at("two/tree")}, fixture.at("destination"), action}));
    std::uint64_t answered = 0;
    unsigned conflicts = 0;
    pump.until([&] {
      if (const auto conflict = service.conflict(); conflict && conflict->id != answered) {
        answered = conflict->id;
        TEST_CHECK(service.resolve(
            conflict->id,
            ++conflicts == 1 ? luciole::TransferConflictChoice::Merge : luciole::TransferConflictChoice::Replace
        ));
      }
      return !workspace.hub.busy();
    });
    TEST_CHECK(conflicts == 3);
    if (action == luciole::TransferAction::Move) {
      TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
      TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
      TEST_CHECK(fs::exists(fixture.root / "destination/tree/final"));
      TEST_CHECK(!fs::exists(fixture.root / "one/tree") && !fs::exists(fixture.root / "two/tree"));
      workspace.hub.shutdown();
      pump.until([&] { return !workspace.hub.busy(); });
      workspace.hub.detach(workspace.page);
      return;
    }
    TEST_CHECK(
        conflicts == 3
        && workspace.hub.canUndo()
        && workspace.hub.historyLabel(false) == (action == luciole::TransferAction::Copy ? "Undo Copy" : "Undo Move")
    );
    const auto window = workspace.model.addWindow(), page = workspace.model.addTab(window);
    Owner owner;
    workspace.hub.attach(page, owner);
    job.reset();
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(window);
    TEST_CHECK(workspace.hub.replayHistory(page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "destination/tree"));
    TEST_CHECK(!fs::exists(fixture.root / "destination/tree/final"));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
    TEST_CHECK(workspace.hub.replayHistory(page, true));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && fs::exists(fixture.root / "destination/tree/final"));
    TEST_CHECK(!fs::exists(fixture.root / "destination/tree/item"));
    TEST_CHECK(fs::exists(fixture.root / "one/tree/item") == (action == luciole::TransferAction::Copy));
    TEST_CHECK(fs::exists(fixture.root / "two/tree/final") == (action == luciole::TransferAction::Copy));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
    workspace.hub.shutdown();
    pump.until([&] { return !workspace.hub.busy(); });
    workspace.hub.detach(page);
    for (const auto& entry : fs::recursive_directory_iterator(fixture.root)) {
      TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
    }
    std::string contents;
    std::ifstream input(fixture.root / "alias");
    std::getline(input, contents);
    TEST_CHECK(contents == "destination/tree/item");
  }

  void copyCleanupReport(Pump& pump) {
    Fixture fixture;
    fixture.write("a");
    Workspace workspace;
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    TEST_CHECK(job->service<luciole::TransferService>().start(
        {{fixture.at("a")}, fixture.at("destination"), luciole::TransferAction::Copy}
    ));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    const auto* undo =
        workspace.hub.newest(workspace.page, [](const OperationJob& item) { return item.kind() == JobKind::Transfer; });
    const fs::path retained = *undo->service<luciole::TransferService>().results()[0].destination->nativePath();
    std::ofstream(retained) << "changed retained data";
    workspace.hub.shutdown();
    pump.until([&] { return !workspace.hub.busy(); });
    workspace.hub.update(workspace.window);
    const auto activities = workspace.hub.activities();
    TEST_CHECK(activities.front().title == "Discard undo data" && activities.front().state == ActivityState::Failed);
    TEST_CHECK(activities.front().report && activities.front().report->contains(retained.native()));
    TEST_CHECK(fs::exists(retained) && fs::exists(fixture.root / "a"));
    TEST_CHECK(!activities.front().canCancel && !activities.front().canRetry);
  }

  void transferLifecycle(Pump& pump) {
    Fixture fixture;
    fixture.write("success");
    fixture.write("missing");
    fixture.write("destination/missing");
    Workspace workspace;
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    TEST_CHECK(workspace.hub.activities().empty());
    TEST_CHECK(service.start(
        {{fixture.at("success"), fixture.at("missing")}, fixture.at("destination"), luciole::TransferAction::Copy}
    ));
    auto live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && !live[0].finished && live[0].canCancel && !live[0].progress);
    pump.until([&] { return service.conflict().has_value(); });
    const auto destination = fixture.root / "destination";
    const auto held = fixture.root / "held-destination";
    // A changed destination identity forces failure for both root and ordinary users.
    fs::rename(destination, held);
    fs::create_directory(destination);
    TEST_CHECK(service.resolve(service.conflict()->id, luciole::TransferConflictChoice::Replace));
    pump.until([&] { return !service.busy(); });
    auto result = workspace.hub.activities();
    TEST_CHECK(
        result.size() == 1 && result[0].finished && result[0].state == ActivityState::Partial && result[0].canRetry
    );
    TEST_CHECK(service.results().size() == 2);
    TEST_CHECK(service.results()[0].outcome == luciole::TransferOutcome::Copied);
    TEST_CHECK(service.results()[1].outcome == luciole::TransferOutcome::Failed);
    TEST_CHECK(service.results()[1].error && service.results()[1].error->kind == luciole::FileErrorKind::Changed);
    TEST_CHECK(fs::is_regular_file(held / "success") && fs::is_empty(destination));
    TEST_CHECK(result[0].report && result[0].report->contains("missing"));
    TEST_CHECK(!workspace.hub.cancelActivity(job->id()));
    // Restore the same directory identity so the captured failed item can retry.
    TEST_CHECK(fs::remove(destination));
    fs::rename(held, destination);
    TEST_CHECK(service.retryFailed());
    live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && live[0].id == job->id() && !live[0].finished);
    std::uint64_t answeredConflict = 0;
    pump.until([&] {
      if (const auto conflict = service.conflict(); conflict && conflict->id != answeredConflict) {
        answeredConflict = conflict->id;
        TEST_CHECK(service.resolve(conflict->id, luciole::TransferConflictChoice::Replace));
      }
      return !service.busy();
    });
    result = workspace.hub.activities();
    TEST_CHECK(result.size() == 1 && result[0].state == ActivityState::Succeeded && !result[0].canRetry);
    TEST_CHECK(fs::is_regular_file(fixture.root / "destination/missing"));
    TEST_CHECK(workspace.owner.changes >= 4);
    workspace.hub.clearFinishedActivities();
    TEST_CHECK(workspace.hub.activities().empty());
  }
  void conflictCancelAndRehome(Pump& pump) {
    Fixture fixture;
    fixture.write("conflict");
    fixture.write("destination/conflict");
    Workspace workspace;
    const auto other = workspace.model.addTab(workspace.window);
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    TEST_CHECK(service.start({{fixture.at("conflict")}, fixture.at("destination"), luciole::TransferAction::Copy}));
    pump.until([&] { return service.conflict().has_value(); });
    auto live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && live[0].state == ActivityState::Waiting && live[0].canReview && live[0].canCancel);
    workspace.hub.clearFinishedActivities();
    TEST_CHECK(workspace.hub.activities().size() == 1 && service.busy());
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(workspace.window);
    TEST_CHECK(workspace.hub.owner(*job) == other && service.conflict());
    TEST_CHECK(workspace.hub.cancelActivity(job->id()));
    pump.until([&] { return !service.busy(); });
    auto result = workspace.hub.activities();
    TEST_CHECK(result.size() == 1 && result[0].state == ActivityState::Cancelled && result[0].finished);
    TEST_CHECK(!workspace.hub.cancelActivity(999999));
    TEST_CHECK(!workspace.hub.activityReport(999999));
  }
  void historyAndInspection(Pump& pump) {
    Fixture fixture;
    Workspace workspace;
    auto inspection = workspace.hub.create<luciole::PermissionService>(workspace.page, JobScope::Page);
    TEST_CHECK(inspection->service<luciole::PermissionService>().prepare(fixture.at("destination")));
    pump.until([&] { return !inspection->running(); });
    TEST_CHECK(workspace.hub.activities().empty());
    for (unsigned index = 0; index < 16; ++index) {
      auto job = workspace.hub.create<luciole::CreateService>(workspace.page, JobScope::Page);
      auto& service = job->service<luciole::CreateService>();
      TEST_CHECK(service.prepare(fixture.at("destination"), luciole::CreateKind::EmptyFile));
      pump.until([&] { return !service.busy(); });
      const auto ready = jobActivity(*job);
      TEST_CHECK(ready && !ready->finished && ready->canReview);
      TEST_CHECK(service.create(std::to_string(index)));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded);
      job.reset();
      workspace.hub.update(workspace.window);
    }
    const auto results = workspace.hub.activities();
    TEST_CHECK(results.size() == 12);
    TEST_CHECK(workspace.hub.find(results[0].id) == nullptr);
    TEST_CHECK(workspace.hub.activityReport(results[0].id) && results[0].state == ActivityState::Succeeded);
    TEST_CHECK(g_utf8_validate(results[0].report->data(), static_cast<gssize>(results[0].report->size()), nullptr));
    workspace.hub.clearFinishedActivities();
    TEST_CHECK(workspace.hub.activities().empty());
  }
  void boundedReports(Pump& pump) {
    Fixture fixture;
    Workspace workspace;
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    std::vector<luciole::Location> sources;
    for (unsigned index = 0; index < 600; ++index) {
      std::string name;
      for (unsigned letter = 0; letter < 60; ++letter) {
        name += "文";
      }
      sources.push_back(fixture.at(name + std::to_string(index)));
    }
    TEST_CHECK(service.start({std::move(sources), fixture.at("destination"), luciole::TransferAction::Copy}));
    pump.until([&] { return !service.busy(); });
    const auto report = workspace.hub.activityReport(job->id());
    TEST_CHECK(report && report->size() <= 64 * 1024 && report->contains("Report truncated."));
    TEST_CHECK(g_utf8_validate(report->data(), static_cast<gssize>(report->size()), nullptr));
    TEST_CHECK(workspace.hub.activities()[0].state == ActivityState::Failed);
  }
  void trashMoveNotifications(Pump& pump, bool provider) {
    struct Observer final : OperationHub::Owner {
      std::vector<luciole::TransferResult> effects;
      unsigned notifications = 0;
      void jobChanged(OperationJob&) override {}
      void activityChanged() override {}
      void transfersFinished(const std::vector<luciole::TransferResult>& results) override {
        effects = results;
        ++notifications;
      }
    } observer;
    Fixture fixture;
    fixture.write("source");
    auto source = luciole::Location::fromUri("trash:///missing-notification-source");
    if (provider) {
      const auto original = fixture.at("source");
      TEST_CHECK(g_file_trash(luciole::gioFile(original).get(), nullptr, nullptr));
      const auto items = luciole::listTrashItems(luciole::CancelToken{});
      const auto found = std::ranges::find_if(items, [&](const auto& item) {
        return item.identity.original == original.nativePath();
      });
      TEST_CHECK(found != items.end());
      source = found->location;
    }
    Workspace workspace;
    const auto other = workspace.model.addTab(workspace.model.addWindow());
    workspace.hub.attach(other, observer);
    const auto job = workspace.hub.create<luciole::RestoreService>(workspace.page);
    auto& service = job->service<luciole::RestoreService>();
    TEST_CHECK(service.move({source}, fixture.at("destination")));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(observer.notifications == 1 && observer.effects.size() == 1);
    TEST_CHECK(observer.effects[0].source == source);
    TEST_CHECK(luciole::transferAffectsLocation(observer.effects, luciole::Location::trash()));
    if (provider) {
      TEST_CHECK(observer.effects[0].outcome == luciole::TransferOutcome::Moved);
      TEST_CHECK(observer.effects[0].destination == fixture.at("destination/source"));
      TEST_CHECK(luciole::transferAffectsLocation(observer.effects, fixture.at("destination")));
      TEST_CHECK(fs::exists(fixture.root / "destination/source") && !fs::exists(fixture.root / "source"));
    } else {
      TEST_CHECK(observer.effects[0].outcome == luciole::TransferOutcome::Failed);
      TEST_CHECK(fs::exists(fixture.root / "source"));
    }
    workspace.hub.detach(other);
  }
  void preparedJobCompletion(Pump& pump) {
    Fixture fixture;
    fixture.write("keep");
    unsigned completions = 0;
    {
      Workspace workspace;
      const auto job = workspace.hub.create<luciole::TrashService>(workspace.page);
      auto& service = job->service<luciole::TrashService>();
      job->whenFinished([&](OperationJob& done) {
        ++completions;
        TEST_CHECK(done.service<luciole::TrashService>().snapshot().status == luciole::TrashStatus::Cancelled);
      });
      TEST_CHECK(service.prepare({fixture.at("keep")}));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.canTrash() && completions == 0);
      service.cancel();
      workspace.hub.update(workspace.window);
      TEST_CHECK(completions == 1 && fs::exists(fixture.root / "keep"));
      workspace.hub.update(workspace.window);
      TEST_CHECK(completions == 1);

      const auto failed = workspace.hub.create<luciole::TrashService>(workspace.page);
      auto& invalid = failed->service<luciole::TrashService>();
      failed->whenFinished([&](OperationJob& done) {
        ++completions;
        TEST_CHECK(done.service<luciole::TrashService>().snapshot().status == luciole::TrashStatus::Error);
      });
      TEST_CHECK(invalid.prepare({fixture.at("missing")}));
      pump.until([&] { return !invalid.busy(); });
      workspace.hub.update(workspace.window);
      TEST_CHECK(completions == 2);

      const auto pending = workspace.hub.create<luciole::TrashService>(workspace.page);
      auto& prepared = pending->service<luciole::TrashService>();
      pending->whenFinished([&](OperationJob&) { ++completions; });
      TEST_CHECK(prepared.prepare({fixture.at("keep")}));
      pump.until([&] { return !prepared.busy(); });
      TEST_CHECK(prepared.canTrash() && completions == 2);
      // Teardown releases the outstanding drop without starting a prepared mutation.
    }
    TEST_CHECK(completions == 3 && fs::exists(fixture.root / "keep"));
  }
  void cancelledArchive(Pump& pump) {
    Fixture fixture;
    fixture.write("input");
    Workspace workspace;
    auto job = workspace.hub.create<luciole::ArchiveService>(workspace.page);
    auto& archive = job->service<luciole::ArchiveService>();
    TEST_CHECK(archive.compress({fixture.at("input")}, fixture.at("output.zip"), luciole::ArchiveFormat::Zip));
    TEST_CHECK(workspace.hub.cancelActivity(job->id()));
    pump.until([&] { return !archive.busy(); });
    const auto results = workspace.hub.activities();
    TEST_CHECK(results.size() == 1 && results[0].finished && results[0].state == ActivityState::Cancelled);
  }
} // namespace

int main(int argc, char** argv) {
  Pump pump;
  if (argc == 3 && std::string_view(argv[1]) == "--overwrite-filesystem") {
    overwriteHistory(pump, argv[2]);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--trash-provider") {
    trashMoveNotifications(pump, true);
    return 0;
  }
  TEST_CHECK(argc == 1);
  overwriteHistory(pump);
  trashMoveNotifications(pump, false);
  transferLifecycle(pump);
  conflictCancelAndRehome(pump);
  historyAndInspection(pump);
  cancelledArchive(pump);
  preparedJobCompletion(pump);
  boundedReports(pump);
  batchRenameHistory(pump);
  renameHistoryReplacement(pump);
  creationHistory(pump);
  creationHistoryReplacement(pump);
  moveHistory(pump);
  moveHistoryReplacement(pump);
  copyHistory(pump);
  replacementMergeHistory(pump);
  moveReplacementMergeHistory(pump);
  repeatedTransferHistory(pump);
  overlappingTransferHistory(pump);
  ancestorTransferHistory(pump, luciole::TransferAction::Copy);
  ancestorTransferHistory(pump, luciole::TransferAction::Move);
  copyCleanupReport(pump);
  pump.until([] { return !luciole::CopyReceipt::cleanupPending(); });
}
