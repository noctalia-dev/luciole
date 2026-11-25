#include "event_loop/glib_poll_source.h"
#include "fs/directory_session.h"
#include "fs/drive_monitor.h"
#include "fs/gio_interop.h"
#include "fs/location_mount.h"
#include "fs/system_mounts.h"
#include "operations/archive_service.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/permission_service.h"
#include "operations/transfer_service.h"
#include "platform/terminal_launcher.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <source_location>
#include <sys/stat.h>

namespace {
  using namespace std::chrono_literals;
  using luciole::MountConfiguration;
  using luciole::MountedFilesystem;
  using luciole::SystemMountState;

  void testMountState() {
    const MountConfiguration config{
        .path = "/media/test/NFS share", .source = "nas:/export", .type = "nfs", .options = "", .automount = true
    };
    std::vector<MountedFilesystem> mounts{{.path = config.path, .source = "systemd-1", .type = "autofs"}};
    TEST_CHECK(luciole::mountState(config, mounts) == SystemMountState::Waiting);
    mounts.push_back({.path = config.path, .source = config.source, .type = "nfs4"});
    TEST_CHECK(luciole::mountState(config, mounts) == SystemMountState::Mounted);
    mounts.pop_back();
    TEST_CHECK(luciole::mountState(config, mounts) == SystemMountState::Waiting);
    mounts.clear();
    TEST_CHECK(luciole::mountState(config, mounts) == SystemMountState::Unavailable);
    mounts.push_back({.path = config.path, .source = "other:/export", .type = "nfs4"});
    TEST_CHECK(luciole::mountState(config, mounts) == SystemMountState::Unavailable);
    mounts.front() = {.path = config.path, .source = "rootfs", .type = "ext4"};
    TEST_CHECK(luciole::mountState(config, mounts) == SystemMountState::Unavailable);

    // Trailing slashes in fstab source/path still match the kernel's normalized mountinfo entries.
    const MountConfiguration trailing{
        .path = "/media/test/NFS share/",
        .source = "nas:/export/",
        .type = "nfs",
        .options = "x-gvfs-show",
        .automount = true
    };
    const std::vector<MountedFilesystem> normalized{
        {.path = "/media/test/NFS share", .source = "nas:/export", .type = "nfs4"}
    };
    TEST_CHECK(luciole::mountState(trailing, normalized) == SystemMountState::Mounted);
    TEST_CHECK(luciole::sameMountSource("nas:/", "nas://"));
    TEST_CHECK(luciole::sameMountSource("//nas/share/", "//nas/share"));
    TEST_CHECK(!luciole::sameMountSource("nas:/", "nas:/export"));
    TEST_CHECK(luciole::bindMount("/Desktop", "rw,relatime"));
    TEST_CHECK(luciole::bindMount("/@home/kasey/Desktop", "rw,subvol=/@home"));
    TEST_CHECK(!luciole::bindMount("/@games", "rw,subvolid=257,subvol=/@games"));
    TEST_CHECK(!luciole::bindMount("/", "rw,relatime"));
    const std::vector<MountConfiguration> trailingList{trailing};
    const std::vector<MountConfiguration> native{
        {.path = "/media/test/NFS share", .source = "nas:/export", .type = "nfs4", .options = "rw", .automount = true}
    };
    const auto merged = luciole::mergeMountConfiguration(trailingList, native);
    TEST_CHECK(merged.size() == 1);
    TEST_CHECK(merged.front().path == "/media/test/NFS share");
    TEST_CHECK(merged.front().options.contains("x-gvfs-show"));
    TEST_CHECK(luciole::automountFor(luciole::Location::fromPath("/media/test/NFS share"), trailingList).has_value());

    // SMB shares match under either kernel name, as NFS does.
    const MountConfiguration smb{
        .path = "/media/test/SMB", .source = "//nas/share", .type = "cifs", .options = "", .automount = true
    };
    const std::vector<MountedFilesystem> smb3{{.path = smb.path, .source = "//nas/share/", .type = "smb3"}};
    TEST_CHECK(luciole::mountState(smb, smb3) == SystemMountState::Mounted);
    TEST_CHECK(luciole::networkMountType("smb3") && !luciole::networkMountType("ext4"));
  }

  void testTargetsAndVisibility() {
    const std::vector<MountConfiguration> config{
        {.path = "/media/test/a/Share", .source = "nas:/a", .type = "nfs", .options = "", .automount = true},
        {.path = "/media/test/b/Share", .source = "nas:/b", .type = "nfs4", .options = "", .automount = true},
        {.path = "/srv/Space #name",
         .source = "nas:/c",
         .type = "nfs",
         .options = "x-gvfs-show,x-gvfs-name=Music%20Archive",
         .automount = true},
        {.path = "/media/test/Hidden", .source = "nas:/d", .type = "nfs", .options = "x-gvfs-hide", .automount = true},
        {.path = "/srv/Hidden", .source = "nas:/e", .type = "nfs", .options = "", .automount = true},
        {.path = "/media/test/a/Share/nested", .source = "nas:/nested", .type = "nfs", .options = "", .automount = true}
    };
    const auto rows = luciole::systemMounts(config, {}, "/home/test", "test");
    TEST_CHECK(rows.size() == config.size());
    TEST_CHECK(rows[0].visible && rows[1].visible && rows[2].visible);
    TEST_CHECK(!rows[3].visible && !rows[4].visible);
    TEST_CHECK(rows[0].label == rows[1].label);
    TEST_CHECK(rows[0].configuration.path != rows[1].configuration.path);
    TEST_CHECK(rows[2].label == "Music Archive");
    const auto target = luciole::Location::fromPath(config[2].path);
    TEST_CHECK(target.nativePath() == config[2].path);
    TEST_CHECK(luciole::automountFor(target, config)->path == config[2].path);
    TEST_CHECK(!luciole::automountFor(luciole::Location::fromPath("/media/test/a/Share-other"), config));
    const auto nested = luciole::automountFor(luciole::Location::fromPath("/media/test/a/Share/nested/folder"), config);
    TEST_CHECK(nested && nested->source == "nas:/nested");
  }

  void pumpUntil(
      const std::function<bool()>& done, std::chrono::seconds timeout = 5s,
      std::source_location caller = std::source_location::current()
  ) {
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::println(stderr, "Timed out at {}:{}", caller.file_name(), caller.line());
        TEST_CHECK(false);
      }
      std::vector<pollfd> fds;
      const auto start = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 10) >= 0);
      source.dispatch(fds, start);
    }
  }

  void testInactiveAutomountDoesNotBrowseUnderlyingDirectory() {
    const luciole::GString directory(g_dir_make_tmp("luciole-automount-test-XXXXXX", nullptr));
    TEST_CHECK(directory != nullptr);
    const std::filesystem::path path(directory.get());
    std::ofstream(path / "underlying-file") << "This is not the NAS";
    luciole::publishNativeMounts(
        {{.path = path.native(),
          .source = "unreachable.invalid:/export",
          .type = "nfs",
          .options = "",
          .automount = true}}
    );
    luciole::DirectorySession session;
    session.open(luciole::Location::fromPath(path.native()));
    pumpUntil([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Error);
    TEST_CHECK(session.snapshot().error->kind == luciole::FileErrorKind::NotMounted);
    TEST_CHECK(session.snapshot().entries.empty());
    session.refresh();
    session.cancel();
    const auto generation = session.snapshot().generation;
    // Complete a subsequent real listing before checking that the cancelled request stayed obsolete.
    session.open(luciole::Location::fromPath(path.parent_path().native()));
    pumpUntil([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().generation > generation);
    TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);

    // Browsing through a symlinked directory alias or missing subpath beneath it still hits the automount guard.
    const auto aliasDir = path.parent_path() / (path.filename().native() + "-alias");
    std::filesystem::create_directory_symlink(path, aliasDir);
    session.open(luciole::Location::fromPath(aliasDir.native()));
    pumpUntil([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Error);
    TEST_CHECK(session.snapshot().error->kind == luciole::FileErrorKind::NotMounted);
    TEST_CHECK(session.snapshot().entries.empty());

    const auto config = luciole::configuredSystemMounts();
    TEST_CHECK(luciole::resolveAutomountFor(luciole::Location::fromPath(aliasDir.native()), config, true).has_value());
    TEST_CHECK(
        !luciole::resolveAutomountFor(luciole::Location::fromPath(aliasDir.native()), config, false).has_value()
    );
    TEST_CHECK(
        luciole::resolveAutomountFor(
            luciole::Location::fromPath((aliasDir / "missing" / "sub").native()), config, false
        )
            .has_value()
    );

    const auto subdirLink = path.parent_path() / (path.filename().native() + "-sublink");
    std::filesystem::create_directory_symlink(path / "subdir", subdirLink);
    const std::vector<MountedFilesystem> waiting{{.path = path.native(), .source = "systemd-1", .type = "autofs"}};
    const std::vector<MountedFilesystem> mounted{
        {.path = path.native(), .source = "unreachable.invalid:/export", .type = "nfs4"}
    };
    TEST_CHECK(!luciole::symlinkEntersUnmountedAutomount(aliasDir.native(), config, waiting));
    TEST_CHECK(luciole::symlinkEntersUnmountedAutomount(subdirLink.native(), config, waiting));
    TEST_CHECK(!luciole::symlinkEntersUnmountedAutomount(subdirLink.native(), config, mounted));

    luciole::publishNativeMounts({});
    std::filesystem::remove(subdirLink);
    std::filesystem::remove(aliasDir);
    std::filesystem::remove_all(path);
  }

  void testParentListingDoesNotReadAutomountDirectoryFile() {
    const luciole::GString directory(g_dir_make_tmp("luciole-automount-parent-XXXXXX", nullptr));
    TEST_CHECK(directory != nullptr);
    const std::filesystem::path parent(directory.get());
    const auto mount = parent / "automount";
    const auto regular = parent / "regular";
    std::filesystem::create_directory(mount);
    std::filesystem::create_directory(regular);
    std::ofstream(mount / ".directory") << "[Desktop Entry]\nIcon=folder-cloud\n";
    std::ofstream(regular / ".directory") << "[Desktop Entry]\nIcon=folder-cloud\n";
    std::filesystem::create_directory_symlink(mount, parent / "automount-link");
    std::filesystem::create_directory_symlink(regular, parent / "regular-link");
    luciole::publishNativeMounts(
        {{.path = mount.native(),
          .source = "unreachable.invalid:/export",
          .type = "nfs",
          .options = "",
          .automount = true}}
    );
    luciole::DirectorySession session;
    session.open(luciole::Location::fromPath(parent.native()));
    const auto firstIcon = [&](std::string_view name) -> std::string {
      for (const auto& entry : session.snapshot().entries) {
        if (entry.name == name && entry.icon && !entry.icon->names.empty()) {
          return entry.icon->names.front();
        }
      }
      return {};
    };
    pumpUntil([&] {
      return session.snapshot().status == luciole::DirectoryStatus::Ready
          && firstIcon("regular") == "folder-cloud"
          && firstIcon("regular-link") == "folder-cloud";
    });
    // Both the folder icon pass and the link pass have finished; neither opened automount/.directory.
    TEST_CHECK(firstIcon("automount") != "folder-cloud");
    TEST_CHECK(firstIcon("automount-link") != "folder-cloud");
    luciole::publishNativeMounts({});
    std::filesystem::remove_all(parent);
  }

  void testFstabMountReportsHelperError() {
    const luciole::GString directory(g_dir_make_tmp("luciole-fstab-mount-XXXXXX", nullptr));
    TEST_CHECK(directory != nullptr);
    const std::filesystem::path root(directory.get());
    const auto helper = root / "mount";
    std::ofstream(helper) << "#!/bin/sh\nprintf 'mount.nfs: access denied by server\\n' >&2\nexit 32\n";
    std::filesystem::permissions(helper, std::filesystem::perms::owner_all);
    const char* inherited = g_getenv("PATH");
    const std::string previous = inherited ? inherited : "";
    g_setenv("PATH", (root.native() + ":" + previous).c_str(), TRUE);
    luciole::LocationMount request;
    std::optional<std::expected<luciole::Location, luciole::FileError>> outcome;
    request.startFstabMount(luciole::Location::fromPath(root.native()), [&](auto result) {
      outcome = std::move(result);
    });
    pumpUntil([&] { return outcome.has_value(); });
    // mount's trailing newline does not reach the one-line error.
    TEST_CHECK(!*outcome && outcome->error().message == "mount.nfs: access denied by server");
    g_setenv("PATH", previous.c_str(), TRUE);
    std::filesystem::remove_all(root);
  }

  void testInactiveAutomountRefusesFileJobsAndTerminal() {
    const luciole::GString directory(g_dir_make_tmp("luciole-automount-jobs-XXXXXX", nullptr));
    TEST_CHECK(directory != nullptr);
    const std::filesystem::path root(directory.get());
    const auto mount = root / "share";
    const auto local = root / "local.txt";
    std::filesystem::create_directory(mount);
    std::ofstream(local) << "source";
    TEST_CHECK(!luciole::openSystemMount(luciole::Location::fromPath(local.native()), {}).valid());
    TEST_CHECK(luciole::openSystemMounts(std::vector{luciole::Location::fromPath(local.native())}, {}).empty());
    luciole::publishNativeMounts(
        {{.path = mount.native(),
          .source = "unreachable.invalid:/export",
          .type = "nfs",
          .options = "",
          .automount = true}}
    );
    const auto target = luciole::Location::fromPath(mount.native());
    luciole::TransferService transfer;
    TEST_CHECK(transfer.start({{luciole::Location::fromPath(local.native())}, target, luciole::TransferAction::Copy}));
    pumpUntil([&] { return !transfer.busy(); });
    TEST_CHECK(
        transfer.results().size() == 1
        && transfer.results().front().outcome == luciole::TransferOutcome::Failed
        && transfer.results().front().error->kind == luciole::FileErrorKind::NotMounted
    );
    TEST_CHECK(!std::filesystem::exists(mount / "local.txt"));

    luciole::CreateService creator;
    TEST_CHECK(creator.prepare(target, luciole::CreateKind::Folder));
    pumpUntil([&] { return !creator.busy(); });
    TEST_CHECK(
        creator.snapshot().status == luciole::CreateStatus::Error
        && creator.snapshot().error->kind == luciole::FileErrorKind::NotMounted
    );

    luciole::TerminalLauncher terminal;
    terminal.setCommand("/usr/bin/true");
    TEST_CHECK(terminal.available());
    std::optional<std::string> terminalError;
    terminal.open(target, [&](std::string message) { terminalError = std::move(message); });
    pumpUntil([&] { return terminalError.has_value(); });
    TEST_CHECK(*terminalError == "The system automount at " + mount.native() + " is unavailable.");

    // Permanent deletion refuses a parent folder containing a configured automount without touching child files.
    const auto underlying = mount / "underlying.txt";
    std::ofstream(underlying) << "keep";
    const auto config = luciole::configuredSystemMounts();
    TEST_CHECK(luciole::automountInside(luciole::Location::fromPath(root.native()), config).has_value());
    const auto rootLink = root.parent_path() / (root.filename().native() + "-link");
    std::filesystem::create_directory_symlink(root, rootLink);
    TEST_CHECK(!luciole::automountInside(luciole::Location::fromPath(rootLink.native()), config).has_value());
    {
      luciole::DirectorySession parentSession;
      parentSession.open(luciole::Location::fromPath(root.parent_path().native()));
      pumpUntil([&] { return parentSession.snapshot().status == luciole::DirectoryStatus::Ready; });
      const auto rootEntry =
          std::ranges::find(parentSession.snapshot().entries, root.filename().native(), [](const auto& e) {
            return e.name;
          });
      TEST_CHECK(rootEntry != parentSession.snapshot().entries.end());
      luciole::EmptyTrashService prepared;
      TEST_CHECK(prepared.prepareDeletion({*rootEntry}));
      pumpUntil([&] { return !prepared.busy(); });
      TEST_CHECK(
          prepared.snapshot().status == luciole::EmptyTrashStatus::Ready
          && prepared.snapshot().size == 0
          && prepared.snapshot().sizeIncomplete
      );
      luciole::EmptyTrashService deleter;
      TEST_CHECK(deleter.deleteSelection({*rootEntry}));
      pumpUntil([&] { return deleter.snapshot().status == luciole::EmptyTrashStatus::Finished; });
      TEST_CHECK(
          deleter.snapshot().results.size() == 1
          && deleter.snapshot().results.front().outcome == luciole::EmptyTrashOutcome::Failed
          && deleter.snapshot().results.front().error->kind == luciole::FileErrorKind::NotMounted
      );
      TEST_CHECK(std::filesystem::exists(underlying));
    }
    std::filesystem::remove(rootLink);

    // Bulk permission inspection keeps per-item results when one entry sits on a stopped automount.
    {
      luciole::DirectorySession rootSession;
      rootSession.open(luciole::Location::fromPath(root.native()));
      pumpUntil([&] { return rootSession.snapshot().status == luciole::DirectoryStatus::Ready; });
      std::vector<luciole::DirectoryEntry> pair;
      for (const auto& entry : rootSession.snapshot().entries) {
        if (entry.name == "local.txt" || entry.name == "share") {
          pair.push_back(entry);
        }
      }
      TEST_CHECK(pair.size() == 2);
      luciole::PermissionService permissions;
      TEST_CHECK(permissions.prepare(pair));
      pumpUntil([&] { return !permissions.busy(); });
      TEST_CHECK(permissions.snapshot().phase == luciole::PermissionPhase::Ready);
      TEST_CHECK(permissions.snapshot().results && permissions.snapshot().results->size() == 2);
      const auto byName = [&](std::string_view name) {
        return std::ranges::find_if(*permissions.snapshot().results, [&](const auto& row) {
          return row.location.basename() == name;
        });
      };
      TEST_CHECK(byName("local.txt")->outcome == luciole::PermissionOutcome::Ready);
      TEST_CHECK(byName("share")->outcome == luciole::PermissionOutcome::Failed);
      TEST_CHECK(byName("share")->error && byName("share")->error->kind == luciole::FileErrorKind::NotMounted);
    }

    // Retrying a partial transfer preserves completed outcomes and retryability when a pending source's mount stops.
    {
      const auto dest = root / "dest";
      std::filesystem::create_directory(dest);
      // A FIFO fails transfer even when tests run as root in CI containers.
      const auto retryFile = mount / "retry.fifo";
      TEST_CHECK(::mkfifo(retryFile.c_str(), 0600) == 0);
      luciole::publishNativeMounts({});
      luciole::TransferService partial;
      TEST_CHECK(partial.start(
          {{luciole::Location::fromPath(local.native()), luciole::Location::fromPath(retryFile.native())},
           luciole::Location::fromPath(dest.native()),
           luciole::TransferAction::Copy}
      ));
      pumpUntil([&] { return !partial.busy(); });
      TEST_CHECK(partial.results().size() == 2);
      TEST_CHECK(partial.results()[0].outcome == luciole::TransferOutcome::Copied);
      TEST_CHECK(partial.results()[1].outcome == luciole::TransferOutcome::Failed);
      TEST_CHECK(partial.retryable() > 0);

      luciole::publishNativeMounts(
          {{.path = mount.native(),
            .source = "unreachable.invalid:/export",
            .type = "nfs",
            .options = "",
            .automount = true}}
      );
      TEST_CHECK(partial.retryFailed());
      pumpUntil([&] { return !partial.busy(); });
      TEST_CHECK(partial.results().size() == 2);
      TEST_CHECK(partial.results()[0].outcome == luciole::TransferOutcome::Copied);
      TEST_CHECK(
          partial.results()[1].outcome == luciole::TransferOutcome::Failed
          && partial.results()[1].error->kind == luciole::FileErrorKind::NotMounted
      );
      TEST_CHECK(partial.retryable() > 0);
    }

    // Archive compression preserves a local symlink to a stopped automount, and refuses a parent containing one.
    {
      const auto shareLink = root / "share-link";
      std::filesystem::create_directory_symlink(mount, shareLink);
      const auto linkTar = root / "link.tar";
      luciole::ArchiveService linkArchive;
      TEST_CHECK(linkArchive.compress(
          {luciole::Location::fromPath(shareLink.native())}, luciole::Location::fromPath(linkTar.native()),
          luciole::ArchiveFormat::Tar
      ));
      pumpUntil([&] { return !linkArchive.busy(); });
      TEST_CHECK(!linkArchive.error() && std::filesystem::exists(linkTar));

      const auto parentTar = root.parent_path() / (root.filename().native() + "-parent.tar");
      luciole::ArchiveService parentArchive;
      TEST_CHECK(parentArchive.compress(
          {luciole::Location::fromPath(root.native())}, luciole::Location::fromPath(parentTar.native()),
          luciole::ArchiveFormat::Tar
      ));
      pumpUntil([&] { return !parentArchive.busy(); });
      TEST_CHECK(
          parentArchive.error()
          && parentArchive.error()->kind == luciole::FileErrorKind::NotMounted
          && !std::filesystem::exists(parentTar)
      );
      std::filesystem::remove(linkTar);
      std::filesystem::remove(shareLink);
    }

    // Recursive transfers refuse a parent source or destination child sitting on a stopped automount.
    {
      const auto outDir = root.parent_path() / (root.filename().native() + "-copy-out");
      std::filesystem::create_directory(outDir);
      luciole::TransferService copyParent;
      TEST_CHECK(copyParent.start(
          {{luciole::Location::fromPath(root.native())},
           luciole::Location::fromPath(outDir.native()),
           luciole::TransferAction::Copy}
      ));
      pumpUntil([&] { return !copyParent.busy(); });
      TEST_CHECK(
          copyParent.results().size() == 1
          && copyParent.results().front().outcome == luciole::TransferOutcome::Failed
          && copyParent.results().front().error->kind == luciole::FileErrorKind::NotMounted
          && !std::filesystem::exists(outDir / root.filename())
      );

      const auto incomingShare = outDir / "share";
      std::filesystem::create_directory(incomingShare);
      std::ofstream(incomingShare / "incoming.txt") << "new";
      luciole::TransferService copyOntoMount;
      TEST_CHECK(copyOntoMount.start(
          {{luciole::Location::fromPath(incomingShare.native())},
           luciole::Location::fromPath(root.native()),
           luciole::TransferAction::Copy}
      ));
      pumpUntil([&] { return !copyOntoMount.busy(); });
      TEST_CHECK(
          copyOntoMount.results().size() == 1
          && copyOntoMount.results().front().outcome == luciole::TransferOutcome::Failed
          && copyOntoMount.results().front().error->kind == luciole::FileErrorKind::NotMounted
          && !std::filesystem::exists(mount / "incoming.txt")
      );
      std::filesystem::remove_all(outDir);
    }

    // Recursive permission edits skip a stopped automount subtree and leave its underlying files untouched.
    {
      TEST_CHECK(::chmod(local.c_str(), 0600) == 0);
      TEST_CHECK(::chmod(underlying.c_str(), 0600) == 0);
      luciole::DirectorySession parentSession;
      parentSession.open(luciole::Location::fromPath(root.parent_path().native()));
      pumpUntil([&] { return parentSession.snapshot().status == luciole::DirectoryStatus::Ready; });
      const auto rootEntry =
          std::ranges::find(parentSession.snapshot().entries, root.filename().native(), [](const auto& e) {
            return e.name;
          });
      TEST_CHECK(rootEntry != parentSession.snapshot().entries.end());
      luciole::PermissionService recursive;
      TEST_CHECK(recursive.prepare(*rootEntry));
      pumpUntil([&] { return !recursive.busy(); });
      TEST_CHECK(recursive.applyRecursive(
          {.files = {.mode = 0644, .owner = {}, .group = {}, .mask = 0777}, .directoryMode = {}, .directoryMask = 0}
      ));
      pumpUntil([&] { return !recursive.busy(); });
      struct stat localStat{};
      struct stat underlyingStat{};
      TEST_CHECK(::lstat(local.c_str(), &localStat) == 0 && (localStat.st_mode & 0777) == 0644);
      TEST_CHECK(::lstat(underlying.c_str(), &underlyingStat) == 0 && (underlyingStat.st_mode & 0777) == 0600);
      TEST_CHECK(recursive.snapshot().results != nullptr);
      const auto mountRow =
          std::ranges::find_if(*recursive.snapshot().results, [&](const auto& row) { return row.location == target; });
      TEST_CHECK(
          mountRow != recursive.snapshot().results->end() && mountRow->outcome == luciole::PermissionOutcome::Skipped
      );
    }

    // Retrying a partially completed folder merge keeps Partial on the parent when the mount stops.
    {
      const auto mergeSrc = mount / "tree";
      const auto mergeDst = root / "merge-dest";
      std::filesystem::create_directories(mergeSrc);
      std::filesystem::create_directories(mergeDst / "tree");
      std::ofstream(mergeSrc / "a-done.txt") << "copied";
      TEST_CHECK(::mkfifo((mergeSrc / "z-fail.fifo").c_str(), 0600) == 0);
      luciole::publishNativeMounts({});
      luciole::TransferService merge;
      TEST_CHECK(merge.start(
          {{luciole::Location::fromPath(mergeSrc.native())},
           luciole::Location::fromPath(mergeDst.native()),
           luciole::TransferAction::Copy,
           true}
      ));
      std::uint64_t handled = 0;
      pumpUntil([&] {
        if (const auto conflict = merge.conflict(); conflict && conflict->id != handled) {
          handled = conflict->id;
          TEST_CHECK(merge.resolve(conflict->id, luciole::TransferConflictChoice::Merge));
        }
        return !merge.busy();
      });
      TEST_CHECK(
          merge.results().size() == 1
          && merge.results().front().outcome == luciole::TransferOutcome::Partial
          && merge.retryable() == 1
          && std::filesystem::exists(mergeDst / "tree" / "a-done.txt")
      );
      luciole::publishNativeMounts(
          {{.path = mount.native(),
            .source = "unreachable.invalid:/export",
            .type = "nfs",
            .options = "",
            .automount = true}}
      );
      TEST_CHECK(merge.retryFailed());
      pumpUntil([&] { return !merge.busy(); });
      TEST_CHECK(
          merge.results().size() == 1
          && merge.results().front().outcome == luciole::TransferOutcome::Partial
          && merge.results().front().error
          && merge.results().front().error->kind == luciole::FileErrorKind::NotMounted
          && merge.retryable() == 1
      );
      const auto& children = merge.results().front().children;
      const auto childByName = [&](std::string_view name) {
        return std::ranges::find_if(children, [&](const auto& row) { return row.source.basename() == name; });
      };
      TEST_CHECK(
          childByName("a-done.txt") != children.end()
          && childByName("a-done.txt")->outcome == luciole::TransferOutcome::Copied
      );
      TEST_CHECK(
          childByName("z-fail.fifo") != children.end()
          && childByName("z-fail.fifo")->outcome == luciole::TransferOutcome::Failed
          && childByName("z-fail.fifo")->error
          && childByName("z-fail.fifo")->error->kind == luciole::FileErrorKind::NotMounted
      );
    }

    luciole::publishNativeMounts({});
    std::filesystem::remove_all(root);
  }

  void integration(std::string_view mode, const std::string& path) {
    const bool manual = mode.starts_with("--manual");
    const auto target = luciole::Location::fromPath(path);
    const auto key = "system-mount:" + target.uri();
    luciole::DriveMonitor drives;
    drives.start();
    pumpUntil([&] { return drives.find(key) != nullptr; }, 15s);
    TEST_CHECK(drives.find(key)->activation == (manual ? std::nullopt : std::optional(target)));
    if (mode == "--wait-inactive") {
      std::println("ready for native unit stop");
      std::fflush(stdout);
      TEST_CHECK(std::getchar() == 'g');
      pumpUntil(
          [&] {
            const auto config = luciole::automountFor(target, luciole::configuredSystemMounts());
            return config
                && luciole::mountState(*config, luciole::readMountedFilesystems()) == SystemMountState::Unavailable;
          },
          15s
      );
    }
    if (mode == "--terminal-stopped") {
      luciole::TerminalLauncher terminal;
      terminal.setCommand("/usr/bin/true");
      TEST_CHECK(terminal.available());
      std::optional<std::string> error;
      terminal.open(target, [&](std::string message) { error = std::move(message); });
      pumpUntil([&] { return error.has_value(); }, 15s);
      TEST_CHECK(*error == "The system automount at " + path + " is unavailable.");
      return;
    }
    if (mode == "--inspect-idle") {
      TEST_CHECK(!drives.find(key)->root);
      for (int i = 0; i < 5; ++i) {
        drives.refreshUsage();
      }
      TEST_CHECK(
          luciole::mountState(
              *luciole::automountFor(target, luciole::configuredSystemMounts()), luciole::readMountedFilesystems()
          )
          == SystemMountState::Waiting
      );
      std::println("idle automount discovered without accessing {}", path);
      return;
    }
    {
      luciole::DirectorySession session;
      bool activated = false;
      luciole::LocationMount request;
      drives.mount(key, request, [&](std::expected<luciole::Location, luciole::FileError> location) {
        if (mode == "--manual-error") {
          TEST_CHECK(!location && location.error().message == "Stopped waiting for the mount. It may still complete.");
          activated = true;
          return;
        }
        TEST_CHECK(location == target);
        session.open(*location);
        activated = true;
      });
      if (manual) {
        pumpUntil([&] { return activated; }, 15s);
      } else {
        TEST_CHECK(activated);
      }
      if (mode == "--manual-error") {
        std::println("ordinary NFS mount failure returned at the mount deadline");
        return;
      }
      pumpUntil([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; }, 15s);
      if (mode == "--browse-error" || mode == "--wait-inactive") {
        TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Error);
        TEST_CHECK(session.snapshot().entries.empty());
        std::println("automount failure reported: {}", session.snapshot().error->message);
        return;
      }
      TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);
      TEST_CHECK(std::ranges::any_of(session.snapshot().entries, [](const auto& entry) {
        return entry.name == "marker";
      }));
      pumpUntil([&] { return drives.find(key) && drives.find(key)->root == target; }, 15s);
      if (!manual) {
        TEST_CHECK(!drives.find(key)->usage);
      }
    }
    if (manual) {
      pumpUntil([&] { return drives.find(key)->removal == luciole::DriveRemoval::Unmount; }, 15s);
      bool removed = false;
      drives.remove(key, [&](const luciole::DriveRemoveResult& result) {
        TEST_CHECK(result.error.empty());
        removed = true;
      });
      pumpUntil([&] { return removed; }, 15s);
      std::println("ordinary user-mountable NFS mounted and unmounted: {}", path);
      return;
    }
    pumpUntil(
        [&] {
          drives.refreshUsage();
          return drives.find(key) && !drives.find(key)->root;
        },
        15s
    );
    TEST_CHECK(drives.find(key)->activation == target);
    std::println("opened NAS contents and expired idle with the same row: {}", path);
  }
} // namespace

int main(int argc, char* argv[]) {
  if (argc == 3) {
    const std::string_view mode(argv[1]);
    TEST_CHECK(
        mode == "--browse"
        || mode == "--terminal-stopped"
        || mode == "--inspect-idle"
        || mode == "--browse-error"
        || mode == "--wait-inactive"
        || mode == "--manual"
        || mode == "--manual-error"
    );
    integration(mode, argv[2]);
    return 0;
  }
  TEST_CHECK(argc == 1);
  testMountState();
  testTargetsAndVisibility();
  testInactiveAutomountDoesNotBrowseUnderlyingDirectory();
  testParentListingDoesNotReadAutomountDirectoryFile();
  testFstabMountReportsHelperError();
  testInactiveAutomountRefusesFileJobsAndTerminal();
  std::println("system mount tests passed");
}
