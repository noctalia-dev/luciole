#include "fs/file_query.h"
#include "fs/folder_totals.h"
#include "fs/folder_totals_walk.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::FolderTotalsPhase;
  void pumpUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void drain() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    pumpUntil([&] { return std::chrono::steady_clock::now() >= end; });
  }
  luciole::DirectoryEntry entry(const fs::path& path) {
    luciole::DirectoryEntry result{
        .place = luciole::Location::fromPath(path.native()), .name = {}, .size = {}, .modifiedSeconds = {}
    };
    const auto facts = luciole::readFileDetails(result.location(), {});
    result.type = facts.type;
    result.symlink = facts.type == luciole::FileType::Symlink;
    result.setIdentity(facts.identity);
    result.setEtag(facts.etag);
    result.contentRevision = facts.contentRevision;
    return result;
  }
  auto walk(std::vector<luciole::DirectoryEntry> roots, luciole::FolderTotalsLimits limits = {}) {
    return luciole::walkFolderTotals(std::move(roots), {}, [](const auto&) {}, limits);
  }
  void basic(const fs::path& root) {
    fs::create_directory(root / "empty");
    const auto empty = walk({entry(root / "empty")});
    TEST_CHECK(!empty.partial() && empty.bytes == 0 && empty.files == 0 && empty.folders == 0);
    const auto tree = root / "tree";
    fs::create_directories(tree / "nested");
    std::ofstream(tree / "café-<literal>-文.txt") << "abc";
    fs::create_hard_link(tree / "café-<literal>-文.txt", tree / "hard-link");
    std::ofstream(tree / ".hidden") << "12345";
    const int fd = ::open((tree / "nested" / "sparse").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    TEST_CHECK(fd >= 0 && ::ftruncate(fd, 1048576) == 0 && ::close(fd) == 0);
    fs::create_directory(root / "outside");
    std::ofstream(root / "outside" / "excluded") << std::string(20000, 'x');
    fs::create_symlink("../outside", tree / "external");
    fs::create_symlink("missing", tree / "broken");
    fs::create_symlink(".", tree / "cycle");
    TEST_CHECK(::mkfifo((tree / "pipe").c_str(), 0600) == 0);
    const auto source = entry(tree);
    const auto result = walk({source, entry(tree / "nested"), entry(tree / "nested" / "sparse"), source});
    TEST_CHECK(result.phase == FolderTotalsPhase::Complete && !result.partial());
    TEST_CHECK(result.bytes == 1048576 + 3 + 3 + 5 + 10 + 7 + 1);
    TEST_CHECK(result.files == 4 && result.folders == 1 && result.links == 3 && result.other == 1);
    const auto limited = walk({source}, {.items = 3, .depth = 128});
    TEST_CHECK(limited.limited && limited.partial());
    TEST_CHECK(limited.files + limited.folders + limited.links + limited.other == 2);
    const auto shallow = walk({source}, {.items = 100, .depth = 0});
    TEST_CHECK(shallow.limited && shallow.folders == 0 && shallow.files == 0);
    const auto broken = walk({entry(tree / "broken")});
    TEST_CHECK(!broken.partial() && broken.links == 1 && broken.bytes == 7);
    auto stale = source;
    stale.setIdentity(luciole::FileIdentity{"wrong", "wrong"});
    const auto changed = walk({stale});
    TEST_CHECK(changed.changed == 1 && changed.bytes == 0 && changed.partial());
    auto removed = entry(tree / ".hidden");
    fs::remove(tree / ".hidden");
    const auto missing = walk({removed});
    TEST_CHECK(missing.unavailable == 1 && missing.partial());
    auto remote = source;
    remote.place = luciole::Location::fromUri("sftp://example.invalid/folder");
    auto trash = source;
    trash.place = luciole::Location::trash();
    const auto unsupported = walk({remote, trash});
    TEST_CHECK(unsupported.unsupported == 2 && unsupported.partial() && unsupported.bytes == 0);
  }
  void progressAndRaces(const fs::path& root) {
    const auto tree = root / "many";
    fs::create_directory(tree);
    for (int i = 0; i < 1000; ++i) {
      std::ofstream(tree / std::to_string(i)) << 'x';
    }
    const auto source = entry(tree);
    luciole::CancelToken cancel;
    std::uint64_t previous = 0;
    const auto stopped = luciole::walkFolderTotals({source}, cancel, [&](const auto& progress) {
      TEST_CHECK(progress.files >= previous);
      previous = progress.files;
      if (progress.files >= 64) {
        cancel.cancel();
      }
    });
    TEST_CHECK(stopped.phase == FolderTotalsPhase::Cancelled && stopped.partial());
    TEST_CHECK(stopped.files >= 64 && stopped.files < 1000 && stopped.bytes == stopped.files);
    bool replaced = false;
    const auto changed = luciole::walkFolderTotals({source}, {}, [&](const auto& progress) {
      if (!replaced && progress.files >= 64) {
        replaced = true;
        fs::rename(tree, root / "held-tree");
        fs::create_directory_symlink(root / "outside", tree);
      }
    });
    TEST_CHECK(replaced && changed.changed > 0 && changed.partial());
    TEST_CHECK(changed.files == 1000 && changed.bytes == 1000); // Held folder, not the replacement link's target.
    fs::remove(tree);
    fs::rename(root / "held-tree", tree);
  }
  void unreadable(const fs::path& root) {
    const auto path = root / "unreadable";
    fs::create_directory(path);
    std::ofstream(path / "secret") << "hidden";
    TEST_CHECK(::chmod(path.c_str(), 0000) == 0);
    if (::geteuid() != 0) {
      const auto result = walk({entry(path)});
      TEST_CHECK(result.unavailable == 1 && result.folders == 0 && result.bytes == 0 && result.partial());
    } else {
      TEST_CHECK(::chmod(root.c_str(), 0755) == 0);
      const pid_t pid = ::fork();
      TEST_CHECK(pid >= 0);
      if (pid == 0) {
        TEST_CHECK(::setgid(65534) == 0 && ::setuid(65534) == 0);
        const auto result = walk({entry(path)});
        TEST_CHECK(result.unavailable == 1 && result.partial());
        ::_exit(0);
      }
      int status = 0;
      TEST_CHECK(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    TEST_CHECK(::chmod(path.c_str(), 0700) == 0);
  }
  void lifecycle(const fs::path& root) {
    luciole::FolderTotalsService service;
    int changes = 0;
    service.setChangedCallback([&] { ++changes; });
    TEST_CHECK(!service.busy() && service.snapshot().phase == FolderTotalsPhase::Idle);
    drain();
    TEST_CHECK(changes == 0);
    const auto few = root / "few";
    fs::create_directory(few);
    std::ofstream(few / "one") << 'x';
    std::ofstream(few / "two") << 'x';
    std::ofstream(few / "three") << 'x';
    const std::vector targets{entry(root / "many")};
    TEST_CHECK(service.start(targets));
    // A new start supersedes the running walk; the superseded result never publishes.
    const std::vector superseded{entry(few)};
    TEST_CHECK(service.start(superseded));
    pumpUntil([&] { return !service.busy(); });
    drain();
    TEST_CHECK(service.snapshot().files == 3 && !service.snapshot().partial());
    TEST_CHECK(changes > 0);
    const int finished = changes;
    drain();
    TEST_CHECK(changes == finished); // No idle progress timer.
    TEST_CHECK(service.start(targets));
    service.cancel();
    TEST_CHECK(service.snapshot().cancelRequested);
    pumpUntil([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().phase == FolderTotalsPhase::Cancelled && service.snapshot().partial());
    TEST_CHECK(service.start(targets));
    service.clear();
    const int before = changes;
    pumpUntil([&] { return !service.busy(); });
    TEST_CHECK(changes == before && service.snapshot().phase == FolderTotalsPhase::Idle);
    const std::vector<luciole::DirectoryEntry> many(4100, targets.front());
    TEST_CHECK(service.start(many));
    pumpUntil([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().limited && service.snapshot().files == 1000);
    {
      luciole::FolderTotalsService retiring;
      retiring.setChangedCallback([] { TEST_CHECK(false); });
      TEST_CHECK(retiring.start(targets));
    }
    drain();
    auto owner = std::make_unique<luciole::FolderTotalsService>();
    owner->setChangedCallback([&] { owner.reset(); });
    TEST_CHECK(owner->start(targets));
    pumpUntil([&] { return !owner; });
    drain();
  }

  void serviceNetworkRefusal(const fs::path& root) {
    const auto tree = root / "refused";
    fs::create_directory(tree);
    std::ofstream(tree / "one") << 'x';
    luciole::FolderTotalsService service;
    int changes = 0;
    service.setChangedCallback([&] { ++changes; });
    service.setNetworkFilesystemCheck([](const std::string&) { return true; });
    const std::vector targets{entry(tree)};
    TEST_CHECK(service.start(targets));
    pumpUntil([&] { return !service.busy(); });
    drain();
    TEST_CHECK(service.snapshot().phase == FolderTotalsPhase::Unsupported);
    TEST_CHECK(service.snapshot().unsupported == 1 && service.snapshot().partial());
    TEST_CHECK(service.snapshot().bytes == 0 && service.snapshot().files == 0);
    TEST_CHECK(changes > 0);
    luciole::FolderTotalsService plain;
    TEST_CHECK(plain.start(targets));
    pumpUntil([&] { return !plain.busy(); });
    TEST_CHECK(plain.snapshot().phase == FolderTotalsPhase::Complete && plain.snapshot().files == 1);
  }

  // Same-device scope keeps same-filesystem totals identical to an unscoped walk; refusing a nested filesystem
  // without traversing it needs a real mount, verified manually since a fixture cannot create one unprivileged.
  void sameDeviceContainment(const fs::path& root) {
    const auto tree = root / "local-only";
    fs::create_directories(tree / "nested");
    std::ofstream(tree / "one") << "abc";
    std::ofstream(tree / "nested" / "two") << "de";
    luciole::FolderTotalsService service;
    const std::vector targets{entry(tree)};
    TEST_CHECK(service.start(targets));
    pumpUntil([&] { return !service.busy(); });
    drain();
    const auto plain = service.snapshot();
    TEST_CHECK(plain.phase == FolderTotalsPhase::Complete && !plain.partial());
    TEST_CHECK(plain.bytes == 5 && plain.files == 2 && plain.folders == 1);
    service.setSameDeviceOnly(true);
    TEST_CHECK(service.start(targets));
    pumpUntil([&] { return !service.busy(); });
    drain();
    TEST_CHECK(service.snapshot().phase == FolderTotalsPhase::Complete && !service.snapshot().partial());
    TEST_CHECK(
        service.snapshot().bytes == plain.bytes
        && service.snapshot().files == plain.files
        && service.snapshot().folders == plain.folders
    );
  }
} // namespace

int main() {
  const luciole::GString temporary(g_dir_make_tmp("luciole-folder-totals-XXXXXX", nullptr));
  TEST_CHECK(temporary);
  const fs::path root(temporary.get());
  basic(root);
  progressAndRaces(root);
  unreadable(root);
  lifecycle(root);
  serviceNetworkRefusal(root);
  sameDeviceContainment(root);
  fs::remove_all(root);
  std::println("folder totals checks passed");
}
