#include "event_loop/glib_poll_source.h"
#include "fs/directory_session.h"
#include "fs/drive_monitor.h"
#include "fs/gio_interop.h"
#include "fs/network_info.h"
#include "fs/system_mounts.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <source_location>

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
    const MountConfiguration inactive{
        .path = path.native(), .source = "unreachable.invalid:/export", .type = "nfs", .options = "", .automount = true
    };
    luciole::publishNativeMounts({inactive});
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
    luciole::publishNativeMounts({});
    const auto target = luciole::Location::fromPath(path.native());
    auto request = luciole::filesystemRequest("list", target);
    request["attributes"] = G_FILE_ATTRIBUTE_STANDARD_NAME;
    // Dispatch a request issued after removal to a worker that still remembers the inactive unit.
    luciole::publishNativeMounts({inactive});
    std::vector<std::string> names;
    (void)luciole::networkFilesystemRequest(
        request,
        [&](const auto& batch) {
          for (const auto& value : batch.at("entries")) {
            names.emplace_back(g_file_info_get_name(luciole::decodeInfo(value).get()));
          }
          return luciole::NetworkMessage{};
        },
        -1
    );
    TEST_CHECK((names == std::vector<std::string>{"underlying-file"}));
    luciole::publishNativeMounts({});
    std::filesystem::remove_all(path);
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
      drives.mount(key, [&](std::optional<luciole::Location> location, const std::string& error) {
        if (mode == "--manual-error") {
          TEST_CHECK(!location && error == "Mount request timed out.");
          activated = true;
          return;
        }
        TEST_CHECK(location == target && error.empty());
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
  std::println("system mount tests passed");
}
