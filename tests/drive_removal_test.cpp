#include "app/drive_removal.h"
#include "app/operation_hub.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "operations/copy_receipt.h"
#include "test_check.h"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/mount.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  bool realMounts = false;
  void until(const std::function<bool()>& done) {
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto first = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
      source.dispatch(fds, first);
    }
  }
  struct Fixture {
    fs::path root;
    bool mounted = false;
    Fixture() {
      const luciole::GString path(g_dir_make_tmp("luciole-removal-XXXXXX", nullptr));
      TEST_CHECK(path);
      root = path.get();
      fs::create_directory(root / "volume");
      if (realMounts) {
        TEST_CHECK(::mount("tmpfs", (root / "volume").c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "size=16m") == 0);
        mounted = true;
      }
    }
    ~Fixture() {
      if (mounted) {
        (void)::umount((root / "volume").c_str());
      }
      std::error_code error;
      fs::remove_all(root, error);
    }
    luciole::Location at(const std::string& name) const { return luciole::Location::fromPath((root / name).native()); }
    void write(const std::string& name, std::string_view content = "payload") const {
      std::ofstream(root / name) << content;
    }
    std::size_t held() const {
      std::size_t count = 0;
      for (const auto& file : fs::directory_iterator("/proc/self/fd")) {
        std::error_code error;
        const auto target = fs::read_symlink(file.path(), error).native();
        count +=
            !error && (target == (root / "volume").native() || target.starts_with((root / "volume").native() + "/"));
      }
      return count;
    }
    luciole::DriveRemoveResult remove() {
      if (realMounts) {
        if (::umount((root / "volume").c_str()) == 0) {
          mounted = false;
          return {};
        }
        TEST_CHECK(errno == EBUSY);
      } else if (held() == 0) {
        return {};
      }
      return {.error = "Busy", .busy = true, .blockers = {{.pid = getpid(), .name = "Luciole"}}};
    }
  };
  struct Workspace {
    luciole::TabWorkspace model;
    luciole::WindowId window = model.addWindow();
    luciole::TabId page = model.addTab(window);
    OperationHub hub{model};
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
        .size = {},
        .modifiedSeconds = {}
    };
    result.setIdentity(luciole::readFileIdentity(info.get()));
    return result;
  }
  std::optional<luciole::FileError> prepare(OperationHub& hub, const luciole::Location& root) {
    bool finished = false;
    std::optional<luciole::FileError> error;
    hub.prepareDriveRemoval(root, [&](std::optional<luciole::FileError> result) {
      error = std::move(result);
      finished = true;
    });
    until([&] { return finished; });
    return error;
  }
  enum class History {
    Copy,
    CopyRedo,
    Replace,
    ReplaceRedo,
    Move,
    CrossMove,
    CrossMoveRedo,
    Create,
    Rename,
    BatchRename
  };
  void retainedHistory(History kind) {
    Fixture fixture;
    Workspace workspace;
    std::shared_ptr<OperationJob> job;
    if (kind == History::Create) {
      job = workspace.hub.create<luciole::CreateService>(workspace.page);
      auto& service = job->service<luciole::CreateService>();
      TEST_CHECK(service.prepare(fixture.at("volume"), luciole::CreateKind::EmptyFile));
      until([&] { return !service.busy(); });
      TEST_CHECK(!prepare(workspace.hub, fixture.at("volume")));
      TEST_CHECK(service.create("created"));
    } else if (kind == History::Rename) {
      fixture.write("volume/input");
      job = workspace.hub.create<luciole::RenameService>(workspace.page);
      auto& service = job->service<luciole::RenameService>();
      TEST_CHECK(service.prepare(fixture.at("volume/input")));
      until([&] { return !service.busy(); });
      TEST_CHECK(!prepare(workspace.hub, fixture.at("volume")));
      TEST_CHECK(service.rename("renamed"));
    } else if (kind == History::BatchRename) {
      fixture.write("volume/a");
      fixture.write("volume/b");
      job = workspace.hub.create<luciole::BatchRenameService>(workspace.page);
      auto& service = job->service<luciole::BatchRenameService>();
      TEST_CHECK(service.prepare({entry(fixture.at("volume/a")), entry(fixture.at("volume/b"))}));
      until([&] { return !service.busy(); });
      TEST_CHECK(!prepare(workspace.hub, fixture.at("volume")));
      TEST_CHECK(service.apply());
    } else {
      const bool move = kind == History::Move || kind == History::CrossMove || kind == History::CrossMoveRedo;
      const bool replace = kind == History::Replace || kind == History::ReplaceRedo;
      const auto* const source = kind == History::Move ? "volume/input" : "input";
      fixture.write(source);
      fs::create_directory(fixture.root / "volume/destination");
      if (replace) {
        fixture.write("volume/destination/input", "original");
      }
      job = workspace.hub.create<luciole::TransferService>(workspace.page);
      auto& service = job->service<luciole::TransferService>();
      TEST_CHECK(service.start(
          {{fixture.at(source)},
           fixture.at("volume/destination"),
           move ? luciole::TransferAction::Move : luciole::TransferAction::Copy}
      ));
      TEST_CHECK(!prepare(workspace.hub, fixture.at("volume")));
      if (replace) {
        until([&] { return service.conflict().has_value(); });
        TEST_CHECK(!prepare(workspace.hub, fixture.at("volume")));
        TEST_CHECK(service.resolve(service.conflict()->id, luciole::TransferConflictChoice::Replace));
      }
    }
    until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo());
    if (kind == History::CopyRedo || kind == History::ReplaceRedo || kind == History::CrossMoveRedo) {
      TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
      until([&] { return !workspace.hub.busy(); });
      TEST_CHECK(workspace.hub.canRedo());
      TEST_CHECK(!fs::exists(fixture.root / "volume/destination/input"));
    }
    const auto descriptors = fixture.held();
    TEST_CHECK(descriptors >= 2);
    const bool redo = workspace.hub.canRedo();
    const auto unrelated = realMounts ? std::make_unique<Fixture>() : nullptr;
    const auto otherRoot = unrelated ? unrelated->at("volume") : luciole::Location::fromUri("smb://fixture/other/");
    TEST_CHECK(!prepare(workspace.hub, otherRoot));
    TEST_CHECK(workspace.hub.canRedo() == redo && workspace.hub.canUndo() != redo);
    TEST_CHECK(fixture.held() == descriptors);
    if (unrelated) {
      TEST_CHECK(unrelated->remove().error.empty());
    }
    // Real mount mode independently asks the kernel: the completed history alone prevents unmounting.
    TEST_CHECK(fixture.remove().busy);
    unsigned attempts = 0, releases = 0;
    bool finished = false;
    removeDriveAfterPreparation(
        [&](auto done) {
          ++releases;
          workspace.hub.prepareDriveRemoval(fixture.at("volume"), std::move(done));
        },
        [&] { return workspace.hub.removalReady(fixture.at("volume")); },
        [&](const auto& done) {
          ++attempts;
          done(fixture.remove());
        },
        [&](const luciole::DriveRemoveResult& result) {
          TEST_CHECK(result.error.empty());
          finished = true;
        }
    );
    until([&] { return finished; });
    TEST_CHECK(attempts == 1 && releases == 1 && fixture.held() == 0);
    TEST_CHECK(!workspace.hub.canUndo() && !workspace.hub.canRedo());
    // Keep the finished job alive throughout, as an open report/dialog does.
    TEST_CHECK(job && !job->running());
    if (!realMounts) {
      if (kind == History::CopyRedo || kind == History::ReplaceRedo || kind == History::CrossMoveRedo) {
        TEST_CHECK(!fs::exists(fixture.root / "volume/destination/input"));
      } else if (
          kind == History::Copy || kind == History::Replace || kind == History::Move || kind == History::CrossMove
      ) {
        TEST_CHECK(fs::exists(fixture.root / "volume/destination/input"));
      }
      for (const auto& child : fs::recursive_directory_iterator(fixture.root / "volume")) {
        TEST_CHECK(!child.path().filename().native().starts_with(".luciole"));
      }
    }
    std::println("history {}: {} descriptors released; normal unmount passed", static_cast<int>(kind), descriptors);
  }
  void unrelatedHistory() {
    Fixture fixture;
    Workspace workspace;
    fixture.write("volume/source");
    fs::create_directory(fixture.root / "outside");
    const auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    TEST_CHECK(job->service<luciole::TransferService>().start(
        {{fixture.at("volume/source")}, fixture.at("outside"), luciole::TransferAction::Copy}
    ));
    until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo());
    // A provider mount unrelated to any native descriptor never clears native history.
    TEST_CHECK(!prepare(workspace.hub, luciole::Location::fromUri("smb://fixture/share/")));
    TEST_CHECK(workspace.hub.canUndo());
    if (realMounts) {
      // Copy Undo pins its destination, not the source drive. The source can depart while Undo stays usable.
      TEST_CHECK(fixture.held() == 0);
      TEST_CHECK(!prepare(workspace.hub, fixture.at("volume")));
      TEST_CHECK(workspace.hub.canUndo());
      TEST_CHECK(fixture.remove().error.empty());
    }
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, false));
    until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canRedo() && !fs::exists(fixture.root / "outside/source"));
    TEST_CHECK(!prepare(workspace.hub, luciole::Location::fromUri("smb://fixture/share/")));
    TEST_CHECK(workspace.hub.canRedo());
    TEST_CHECK(workspace.hub.replayHistory(workspace.page, true));
    until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo() && fs::exists(fixture.root / "outside/source"));
  }
  void bindMountHistory() {
    if (!realMounts) {
      return;
    }
    Fixture fixture;
    Workspace workspace;
    fs::create_directory(fixture.root / "outside");
    fs::create_directory(fixture.root / "volume/bind");
    fs::create_directory(fixture.root / "outside/target");
    TEST_CHECK(
        ::mount((fixture.root / "outside").c_str(), (fixture.root / "volume/bind").c_str(), nullptr, MS_BIND, nullptr)
        == 0
    );
    TEST_CHECK(fs::equivalent(fixture.root / "outside", fixture.root / "volume/bind"));
    fixture.write("input");
    // A differently spelled destination still captures the bind mount through its actual descriptors.
    fs::create_directory_symlink(fixture.root / "volume/bind", fixture.root / "alias");
    const auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    TEST_CHECK(job->service<luciole::TransferService>().start(
        {{fixture.at("input")}, fixture.at("alias/target"), luciole::TransferAction::Copy}
    ));
    until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo());
    // Equal device/inode is insufficient: the underlying mount does not own these bind-mount descriptors.
    TEST_CHECK(!prepare(workspace.hub, fixture.at("outside")));
    TEST_CHECK(workspace.hub.canUndo());
    TEST_CHECK(::umount((fixture.root / "volume/bind").c_str()) != 0 && errno == EBUSY);
    TEST_CHECK(!prepare(workspace.hub, fixture.at("volume/bind")));
    until([&] { return workspace.hub.removalReady(fixture.at("volume/bind")); });
    TEST_CHECK(!workspace.hub.canUndo());
    TEST_CHECK(::umount((fixture.root / "volume/bind").c_str()) == 0);
    TEST_CHECK(fs::exists(fixture.root / "outside/target/input"));
    TEST_CHECK(fixture.remove().error.empty());
    std::println("bind mount and symlink alias: exact mount attribution passed");
  }

  void obsoletePreparation() {
    Fixture fixture;
    Workspace workspace;
    fixture.write("input");
    bool finished = false;
    workspace.hub.prepareDriveRemoval(fixture.at("volume"), [&](std::optional<luciole::FileError> error) {
      TEST_CHECK(error && error->kind == luciole::FileErrorKind::Changed);
      finished = true;
    });
    const auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    TEST_CHECK(job->service<luciole::TransferService>().start(
        {{fixture.at("input")}, fixture.at("volume"), luciole::TransferAction::Copy}
    ));
    until([&] { return finished && !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndo());
  }
  void blockerPolicy() {
    for (int mode = 0; mode != 6; ++mode) {
      unsigned attempts = 0, preparations = 0;
      bool finished = false;
      removeDriveAfterPreparation(
          [&](const auto& done) {
            ++preparations;
            done(
                mode == 3 ? std::optional(luciole::FileError{luciole::FileErrorKind::Changed, "Changed"}) : std::nullopt
            );
          },
          [] { return true; },
          [&](const auto& done) {
            ++attempts;
            luciole::DriveRemoveResult result;
            if (mode != 0) {
              result = {.error = "Busy", .busy = true, .blockers = {}};
              if (mode >= 2) {
                result.blockers.push_back({.pid = getpid(), .name = "Luciole"});
              }
              if (mode == 2 || (mode == 5 && attempts > 1)) {
                result.blockers.push_back({.pid = getpid() + 1, .name = "Other"});
              }
            }
            done(std::move(result));
          },
          [&](const luciole::DriveRemoveResult& result) {
            TEST_CHECK(result.error.empty() == (mode == 0));
            finished = true;
          }
      );
      until([&] { return finished; });
      TEST_CHECK(attempts == (mode == 3 ? 0U : mode == 4 ? 8U : mode == 5 ? 2U : 1U));
      TEST_CHECK(preparations == 1);
    }
  }
} // namespace

int main(int argc, char** argv) {
  realMounts = argc == 2 && std::string_view(argv[1]) == "--mount-history";
  TEST_CHECK(argc == 1 || realMounts);
  for (const auto kind :
       {History::Copy, History::CopyRedo, History::Replace, History::ReplaceRedo, History::Move, History::CrossMove,
        History::CrossMoveRedo, History::Create, History::Rename, History::BatchRename}) {
    retainedHistory(kind);
  }
  unrelatedHistory();
  bindMountHistory();
  obsoletePreparation();
  blockerPolicy();
  until([] { return !luciole::CopyReceipt::cleanupPending(); });
}
