#include "event_loop/glib_poll_source.h"
#include "operations/copy_receipt.h"
#include "operations/creation_stage.h"
#include "operations/history_revisions.h"
#include "operations/move_receipt.h"
#include "operations/transfer_service.h"
#include "operations/transfer_staging.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <print>
#include <sched.h>
#include <stdexcept>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::TransferAction;
  using luciole::TransferConflictChoice;
  using luciole::TransferOutcome;
  std::string fixtureParent = "/tmp";
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {(std::istreambuf_iterator<char>(input)), {}};
  }
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }
  void drainHistoryCleanup() {
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (luciole::CopyReceipt::cleanupPending()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto first = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
      source.dispatch(fds, first);
    }
  }
  struct Fixture {
    fs::path root;
    explicit Fixture(const std::string& parent = fixtureParent) {
      std::string name = parent + "/luciole-transfer-XXXXXX";
      TEST_CHECK(::mkdtemp(name.data()) != nullptr);
      root = name;
      fs::create_directory(root / "source");
      fs::create_directory(root / "destination");
    }
    ~Fixture() {
      drainHistoryCleanup();
      std::error_code error;
      for (const auto& item :
           fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error)) {
        if (item.is_directory() && !item.is_symlink()) {
          fs::permissions(item.path(), fs::perms::owner_all, fs::perm_options::add, error);
        }
      }
      fs::remove_all(root, error);
    }
    fs::path source(const std::string& name) const { return root / "source" / name; }
    fs::path destination(const std::string& name) const { return root / "destination" / name; }
    void clean() const {
      drainHistoryCleanup();
      for (const auto& item : fs::recursive_directory_iterator(root)) {
        TEST_CHECK(!item.path().filename().native().starts_with(".luciole-transfer-"));
      }
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        m_source.dispatch(fds, first);
      }
    }
    void
    run(luciole::TransferService& service, std::vector<luciole::Location> sources, const fs::path& destination,
        TransferAction action, std::function<TransferConflictChoice(const luciole::TransferConflict&)> choose = {}) {
      TEST_CHECK(service.start({std::move(sources), location(destination), action}));
      std::uint64_t handled = 0;
      until([&] {
        if (const auto conflict = service.conflict(); conflict && conflict->id != handled) {
          handled = conflict->id;
          TEST_CHECK(!service.resolve(conflict->id + 100000, TransferConflictChoice::Replace));
          const auto decision = choose ? choose(*conflict) : TransferConflictChoice::Cancel;
          TEST_CHECK(service.resolve(conflict->id, decision));
        }
        return !service.busy();
      });
    }
    void replay(luciole::TransferService& service, const std::shared_ptr<const luciole::MoveReceipt>& receipt) {
      TEST_CHECK(receipt && service.replay(receipt));
      until([&] { return !service.busy(); });
    }
    void replayCopy(luciole::TransferService& service, const std::shared_ptr<const luciole::CopyReceipt>& receipt) {
      TEST_CHECK(receipt && service.replayCopy(receipt));
      until([&] { return !service.busy(); });
    }
    void cleanup(luciole::TransferService& service) {
      service.takeCopyReceipt().reset();
      until([] { return !luciole::CopyReceipt::cleanupPending(); });
    }

  private:
    GlibPollSource m_source;
  };
  void testCopyHistory(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Link}) {
      Fixture f;
      luciole::TransferService service;
      const auto raw = std::string("raw-") + static_cast<char>(0xff);
      write(f.source("文.txt"), "original");
      fs::create_directories(f.source("folder/sub"));
      write(f.source("folder/sub/.inside"), "nested");
      fs::create_symlink("missing", f.source("broken"));
      write(f.source(raw), "raw");
      pump.run(
          service,
          {location(f.source("文.txt")), location(f.source("folder")), location(f.source("broken")),
           location(f.source(raw))},
          f.destination(""), action
      );
      TEST_CHECK(service.copyReceipt() && !service.receipt());
      const auto original = fs::status(f.destination("文.txt")).permissions();
      write(f.source("文.txt"), "source changed");
      auto stale = service.copyReceipt();
      std::shared_ptr<const luciole::CopyReceipt> staleHidden;
      for (int cycle = 0; cycle < 3; ++cycle) {
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(service.copyReceipt() && !service.historyRetired() && service.completed() == 4);
        TEST_CHECK(!fs::exists(f.destination("文.txt")) && !fs::exists(f.destination("folder")));
        TEST_CHECK(read(f.source("文.txt")) == "source changed" && read(f.source("folder/sub/.inside")) == "nested");
        luciole::TransferService other;
        pump.replayCopy(other, staleHidden ? staleHidden : stale);
        TEST_CHECK(other.historyRetired() && !other.copyReceipt());
        if (!staleHidden) {
          staleHidden = service.copyReceipt();
        }
        TEST_CHECK(
            service.results()[0].destination
            && fs::is_directory(fs::path(*service.results()[0].destination->nativePath()).parent_path())
        );
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(service.copyReceipt() && !service.historyRetired());
        if (action == TransferAction::Copy) {
          TEST_CHECK(
              read(f.destination("文.txt")) == "original" && read(f.destination("folder/sub/.inside")) == "nested"
          );
          TEST_CHECK(fs::read_symlink(f.destination("broken")) == "missing" && read(f.destination(raw)) == "raw");
        } else {
          TEST_CHECK(fs::read_symlink(f.destination("文.txt")) == f.source("文.txt"));
          TEST_CHECK(fs::read_symlink(f.destination("folder")) == f.source("folder"));
          TEST_CHECK(fs::read_symlink(f.destination("broken")) == f.source("broken"));
        }
        TEST_CHECK(fs::status(f.destination("文.txt")).permissions() == original);
      }
      TEST_CHECK(!service.retryFailed() && !service.replayCopy({}));
      pump.replayCopy(service, service.copyReceipt());
      stale.reset();
      staleHidden.reset();
      pump.cleanup(service);
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
      for (const auto& item : fs::directory_iterator(f.destination(""))) {
        TEST_CHECK(!item.path().filename().native().starts_with(".luciole-create-"));
      }
    }
  }
  void testCopyHistoryGuards(Pump& pump) {
    for (int scenario = 0; scenario < 7; ++scenario) {
      Fixture f;
      luciole::TransferService service;
      write(f.source("a"), "A");
      fs::create_directories(f.source("tree/sub"));
      write(f.source("tree/sub/inside"), "original");
      pump.run(service, {location(f.source("a")), location(f.source("tree"))}, f.destination(""), TransferAction::Copy);
      auto receipt = service.copyReceipt();
      TEST_CHECK(receipt);
      fs::path retained;
      if (scenario >= 5) {
        pump.replayCopy(service, receipt);
        receipt = service.copyReceipt();
        retained = *service.results()[1].destination->nativePath();
        if (scenario == 5) {
          write(retained / "sub/inside", "modified retained data");
        } else {
          fs::create_symlink("missing", f.destination("tree"));
        }
      } else if (scenario <= 1) {
        const auto path = scenario == 0 ? f.destination("a") : f.destination("tree/sub/inside");
        struct stat stamp{};
        TEST_CHECK(::stat(path.c_str(), &stamp) == 0);
        write(path, scenario == 0 ? "B" : "modified");
        const struct timespec times[]{stamp.st_atim, stamp.st_mtim};
        TEST_CHECK(::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
      } else if (scenario == 2) {
        write(f.destination("tree/sub/new"), "new data");
      } else if (scenario == 3) {
        fs::rename(f.destination("tree"), f.destination("held"));
        fs::create_directory(f.destination("tree"));
      } else {
        fs::rename(f.root / "destination", f.root / "held-parent");
        fs::create_directory(f.root / "destination");
      }
      pump.replayCopy(service, receipt);
      TEST_CHECK(!service.copyReceipt() && service.results().size() == 2);
      TEST_CHECK(service.historyRetired() == (scenario != 6));
      TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
        return result.outcome == TransferOutcome::Failed;
      }));
      if (scenario == 6) {
        TEST_CHECK(!fs::exists(f.destination("a")) && fs::is_symlink(f.destination("tree")));
        TEST_CHECK(fs::remove(f.destination("tree")));
        pump.replayCopy(service, receipt);
        TEST_CHECK(service.copyReceipt() && read(f.destination("a")) == "A");
      } else if (scenario >= 5) {
        TEST_CHECK(!fs::exists(f.destination("a")) && read(retained / "sub/inside") == "modified retained data");
      } else {
        const auto actual = scenario == 4 ? f.root / "held-parent/a" : f.destination("a");
        TEST_CHECK(read(actual) == (scenario == 0 ? "B" : "A"));
      }
      receipt.reset();
      pump.cleanup(service);
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == (scenario != 5));
      if (scenario == 5) {
        TEST_CHECK(
            read(retained / "sub/inside") == "modified retained data"
            && std::ranges::any_of(errors, [&](const auto& error) { return error.message.contains(retained.native()); })
        );
      }
    }
  }
  void testCopyHistoryRetention(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    write(f.source("a.txt"), "incoming");
    write(f.destination("a.txt"), "existing");
    pump.run(
        service, {location(f.source("a.txt")), location(f.source("a.txt"))}, f.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(service.copyReceipt() && read(f.destination("a (1).txt")) == "incoming");
    TEST_CHECK(service.results()[1].outcome == TransferOutcome::Skipped);
    pump.replayCopy(service, service.copyReceipt());
    const fs::path retained = *service.results()[0].destination->nativePath();
    TEST_CHECK(read(retained) == "incoming" && read(f.destination("a.txt")) == "existing");
    pump.replayCopy(service, service.copyReceipt());
    TEST_CHECK(read(f.destination("a (1).txt")) == "incoming" && read(f.destination("a.txt")) == "existing");
    pump.replayCopy(service, service.copyReceipt());
    pump.cleanup(service);
    TEST_CHECK(!fs::exists(retained.parent_path()) && read(f.destination("a.txt")) == "existing");
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());

    fs::path deep = f.source("deep");
    for (int i = 0; i < 66; ++i) {
      deep /= "child";
    }
    fs::create_directories(deep);
    pump.run(service, {location(f.source("deep"))}, f.destination(""), TransferAction::Copy);
    TEST_CHECK(service.results()[0].outcome == TransferOutcome::Copied && !service.copyReceipt());
    pump.cleanup(service);
  }
  void testCopyHistoryCancellation(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    std::vector<luciole::Location> sources;
    for (int i = 0; i < 128; ++i) {
      write(f.source(std::to_string(i)), std::to_string(i));
      sources.push_back(location(f.source(std::to_string(i))));
    }
    pump.run(service, sources, f.destination(""), TransferAction::Copy);
    auto receipt = service.copyReceipt();
    TEST_CHECK(receipt);
    luciole::CancelToken token;
    token.cancel();
    std::atomic<std::size_t> completed{0};
    auto result = receipt->reverse(token, completed);
    TEST_CHECK(!result.retireHistory && !result.receipt && completed.load() == 128);
    TEST_CHECK(std::ranges::all_of(result.results, [](const auto& item) {
      return item.outcome == TransferOutcome::Cancelled;
    }));
    token = luciole::CancelToken{};
    completed.store(0);
    std::jthread interrupt([&](const std::stop_token& stop) {
      while (!stop.stop_requested() && completed.load() == 0) {
        std::this_thread::yield();
      }
      token.cancel();
    });
    result = receipt->reverse(token, completed);
    interrupt.request_stop();
    interrupt.join();
    TEST_CHECK(completed.load() == 128);
    for (std::size_t i = 0; i < result.results.size(); ++i) {
      const auto& item = result.results[i];
      if (item.outcome == TransferOutcome::Moved) {
        TEST_CHECK(item.destination && read(*item.destination->nativePath()) == std::to_string(i));
        TEST_CHECK(!fs::exists(f.destination(std::to_string(i))));
      } else {
        TEST_CHECK(
            item.outcome == TransferOutcome::Cancelled && read(f.destination(std::to_string(i))) == std::to_string(i)
        );
      }
      TEST_CHECK(read(f.source(std::to_string(i))) == std::to_string(i));
    }
    result.receipt.reset();
    receipt.reset();
    pump.cleanup(service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() == !result.retireHistory);
  }

  fs::path retainedItem(const Fixture& fixture, const struct stat& expected) {
    for (const auto& item : fs::recursive_directory_iterator(fixture.destination(""))) {
      if (item.path().filename().native().starts_with(".luciole-create-")) {
        struct stat current{};
        const auto path = item.path() / "item";
        if (::lstat(path.c_str(), &current) == 0
            && current.st_dev == expected.st_dev
            && current.st_ino == expected.st_ino) {
          return path;
        }
      }
    }
    throw std::runtime_error("Retained destination not found");
  }
  void testRenameRevisions() {
    struct stat before{};
    before.st_dev = 1;
    before.st_ino = 2;
    before.st_mode = S_IFDIR | 0750;
    before.st_nlink = 2;
    before.st_mtim = {123, 456};
    before.st_ctim = {123, 789};
    auto after = before;
    ++after.st_ctim.tv_sec;
    TEST_CHECK(luciole::sameRenamedRevision(before, after));
    ++after.st_mtim.tv_nsec;
    TEST_CHECK(!luciole::sameRenamedRevision(before, after));
    TEST_CHECK(luciole::sameRenamedRevision(before, after, true));
    for (int mutation = 0; mutation < 7; ++mutation) {
      auto changed = after;
      switch (mutation) {
      case 0:
        ++changed.st_ino;
        break;
      case 1:
        ++changed.st_dev;
        break;
      case 2:
        changed.st_mode ^= S_IWUSR;
        break;
      case 3:
        ++changed.st_uid;
        break;
      case 4:
        ++changed.st_gid;
        break;
      case 5:
        ++changed.st_size;
        break;
      case 6:
        ++changed.st_nlink;
        break;
      }
      TEST_CHECK(!luciole::sameRenamedRevision(before, changed, true));
    }
    luciole::HistoryRevisions history;
    history.capture(before);
    history.renamed(before, after, true);
    history.validate(after, after);
    auto changed = after;
    ++changed.st_mtim.tv_nsec;
    bool rejected = false;
    try {
      history.validate(after, changed);
    } catch (const luciole::FileErrorException&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
    for (const auto type : {S_IFREG, S_IFLNK}) {
      before.st_mode = type | 0640;
      after.st_mode = before.st_mode;
      TEST_CHECK(!luciole::sameRenamedRevision(before, after, true));
    }
  }
  void testReplacementSourceGuard(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Link}) {
      for (bool alias : {false, true}) {
        Fixture f;
        fs::create_directory(f.destination("item"));
        write(f.destination("item/item"), "source bytes");
        write(f.destination("item/keep"), "unrelated bytes");
        if (alias) {
          fs::create_directory_symlink(f.destination("item"), f.root / "alias");
        }
        luciole::TransferService service;
        pump.run(
            service, {location(alias ? f.root / "alias/item" : f.destination("item/item"))}, f.destination(""), action,
            [](const auto&) { return TransferConflictChoice::Replace; }
        );
        TEST_CHECK(service.results()[0].outcome == TransferOutcome::Failed);
        TEST_CHECK(service.results()[0].error->kind == luciole::FileErrorKind::WouldRecurse);
        TEST_CHECK(read(f.destination("item/item")) == "source bytes");
        TEST_CHECK(read(f.destination("item/keep")) == "unrelated bytes");
        f.clean();
      }
    }
  }
  void testReplacementTimestampGuards(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      for (const bool changeRetained : {false, true}) {
        Fixture f;
        fs::create_directories(f.source("item/sub"));
        fs::create_directories(f.destination("item/sub"));
        write(f.source("item/sub/file"), "incoming");
        write(f.destination("item/sub/file"), "previous");
        const auto old = luciole::transfer::info(AT_FDCWD, f.destination("item").native());
        luciole::TransferService service;
        pump.run(service, {location(f.source("item"))}, f.destination(""), action, [](const auto&) {
          return TransferConflictChoice::Replace;
        });
        TEST_CHECK(service.copyReceipt() || service.receipt());
        const auto retained = retainedItem(f, old);
        const auto changed = changeRetained ? retained : f.destination("item");
        struct stat current{};
        TEST_CHECK(::lstat(changed.c_str(), &current) == 0);
        const timespec times[]{{0, UTIME_OMIT}, {current.st_mtim.tv_sec - 10, current.st_mtim.tv_nsec}};
        TEST_CHECK(::utimensat(AT_FDCWD, changed.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
        if (action == TransferAction::Copy) {
          pump.replayCopy(service, service.copyReceipt());
        } else {
          pump.replay(service, service.receipt());
        }
        TEST_CHECK(service.historyRetired() && !service.copyReceipt() && !service.receipt());
        TEST_CHECK(service.results()[0].outcome == TransferOutcome::Failed);
        TEST_CHECK(read(f.destination("item/sub/file")) == "incoming");
        TEST_CHECK(read(retained / "sub/file") == "previous");
        service.takeCopyReceipt().reset();
        service.takeReceipt().reset();
        pump.until([] { return !luciole::CopyReceipt::cleanupPending() && !luciole::MoveReceipt::cleanupPending(); });
        (void)luciole::CopyReceipt::takeCleanupErrors();
        if (changeRetained) {
          TEST_CHECK(read(retained / "sub/file") == "previous");
        }
      }
    }
  }
  void testReplacementHistory(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Link}) {
      for (const bool finishUndone : {false, true}) {
        for (int variant = 0; variant < 5; ++variant) {
          Fixture f;
          luciole::TransferService service;
          const std::string name = "item 文";
          const bool incomingFolder = variant == 1 || variant == 3;
          const bool previousFolder = variant == 1 || variant == 2;
          write(f.root / "protected", "outside data");
          if (incomingFolder) {
            fs::create_directories(f.source(name) / "sub");
            write(f.source(name) / "sub/.new", "incoming");
          } else if (variant == 4) {
            fs::create_symlink("incoming-missing", f.source(name));
          } else {
            write(f.source(name), "incoming");
          }
          if (previousFolder) {
            fs::create_directories(f.destination(name) / "sub");
            write(f.destination(name) / "sub/old", "previous");
            fs::create_symlink(f.root / "protected", f.destination(name) / "sub/link");
            TEST_CHECK(::chmod(f.destination(name).c_str(), 0750) == 0);
          } else if (variant == 4) {
            fs::create_symlink("previous-missing", f.destination(name));
          } else {
            write(f.destination(name), "previous");
            TEST_CHECK(::chmod(f.destination(name).c_str(), 0640) == 0);
          }
          struct stat old{};
          TEST_CHECK(::lstat(f.destination(name).c_str(), &old) == 0);
          pump.run(service, {location(f.source(name))}, f.destination(""), action, [](const auto&) {
            return TransferConflictChoice::Replace;
          });
          TEST_CHECK(service.copyReceipt() && !service.results()[0].error);
          if (action == TransferAction::Copy && variant == 0) {
            TEST_CHECK(fs::remove(f.source(name)));
          }
          const auto verifyIncoming = [&] {
            if (action == TransferAction::Link) {
              TEST_CHECK(fs::read_symlink(f.destination(name)) == f.source(name));
            } else if (incomingFolder) {
              TEST_CHECK(read(f.destination(name) / "sub/.new") == "incoming");
            } else if (variant == 4) {
              TEST_CHECK(fs::read_symlink(f.destination(name)) == "incoming-missing");
            } else {
              TEST_CHECK(read(f.destination(name)) == "incoming");
            }
          };
          const auto verifyPrevious = [&] {
            struct stat restored{};
            TEST_CHECK(::lstat(f.destination(name).c_str(), &restored) == 0);
            TEST_CHECK(
                restored.st_ino == old.st_ino && restored.st_dev == old.st_dev && restored.st_mode == old.st_mode
            );
            // XFS updates a directory's mtime during an exchange. File mtimes must remain exact.
            if (!previousFolder) {
              TEST_CHECK(
                  restored.st_mtim.tv_sec == old.st_mtim.tv_sec && restored.st_mtim.tv_nsec == old.st_mtim.tv_nsec
              );
            }
            if (previousFolder) {
              TEST_CHECK(read(f.destination(name) / "sub/old") == "previous");
            } else if (variant == 4) {
              TEST_CHECK(fs::read_symlink(f.destination(name)) == "previous-missing");
            } else {
              TEST_CHECK(read(f.destination(name)) == "previous");
            }
          };
          for (int cycle = 0; cycle < 3; ++cycle) {
            pump.replayCopy(service, service.copyReceipt());
            TEST_CHECK(service.copyReceipt() && !service.historyRetired());
            verifyPrevious();
            TEST_CHECK(service.results()[0].children.size() == 2);
            pump.replayCopy(service, service.copyReceipt());
            TEST_CHECK(service.copyReceipt() && !service.historyRetired());
            verifyIncoming();
          }
          if (finishUndone) {
            pump.replayCopy(service, service.copyReceipt());
            TEST_CHECK(service.copyReceipt() && !service.historyRetired());
            verifyPrevious();
          }
          pump.cleanup(service);
          TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
          if (finishUndone) {
            verifyPrevious();
          } else {
            verifyIncoming();
          }
          TEST_CHECK(read(f.root / "protected") == "outside data");
          for (const auto& item : fs::directory_iterator(f.destination(""))) {
            TEST_CHECK(!item.path().filename().native().starts_with(".luciole-create-"));
          }
          f.clean();
        }
      }
    }
  }
  void testCopyMergeHistory(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    fs::create_directories(f.source("tree/sub/fresh"));
    fs::create_directories(f.destination("tree/sub"));
    write(f.source("tree/sub/file"), "incoming");
    write(f.source("tree/sub/fresh/data"), "fresh");
    write(f.source("tree/top"), "top");
    write(f.destination("tree/sub/file"), "previous");
    write(f.destination("tree/keep"), "keep");
    TEST_CHECK(::chmod(f.destination("tree").c_str(), 0750) == 0);
    struct stat parent{};
    TEST_CHECK(::stat(f.destination("tree").c_str(), &parent) == 0);
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Copy, [](const auto& conflict) {
      return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
    });
    TEST_CHECK(service.copyReceipt() && !service.results()[0].error);
    write(f.destination("tree/keep"), "edited destination-only data");
    write(f.destination("tree/sub/added"), "user data");
    for (int cycle = 0; cycle < 3; ++cycle) {
      pump.replayCopy(service, service.copyReceipt());
      TEST_CHECK(service.copyReceipt() && read(f.destination("tree/sub/file")) == "previous");
      TEST_CHECK(!fs::exists(f.destination("tree/sub/fresh")) && !fs::exists(f.destination("tree/top")));
      TEST_CHECK(
          read(f.destination("tree/keep")) == "edited destination-only data"
          && read(f.destination("tree/sub/added")) == "user data"
      );
      pump.replayCopy(service, service.copyReceipt());
      TEST_CHECK(service.copyReceipt() && read(f.destination("tree/sub/file")) == "incoming");
      TEST_CHECK(read(f.destination("tree/sub/fresh/data")) == "fresh" && read(f.destination("tree/top")) == "top");
      struct stat current{};
      TEST_CHECK(
          ::stat(f.destination("tree").c_str(), &current) == 0
          && current.st_ino == parent.st_ino
          && current.st_mode == parent.st_mode
      );
    }
    pump.cleanup(service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    TEST_CHECK(read(f.source("tree/sub/file")) == "incoming");
    f.clean();

    fs::create_directory(f.source("empty"));
    fs::create_directory(f.destination("empty"));
    write(f.destination("empty/keep"), "kept");
    pump.run(service, {location(f.source("empty"))}, f.destination(""), TransferAction::Copy, [](const auto&) {
      return TransferConflictChoice::Merge;
    });
    TEST_CHECK(!service.copyReceipt() && read(f.destination("empty/keep")) == "kept");
    write(f.source("new"), "new");
    pump.run(
        service, {location(f.source("empty")), location(f.source("new"))}, f.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Merge; }
    );
    TEST_CHECK(service.copyReceipt());
    pump.replayCopy(service, service.copyReceipt());
    TEST_CHECK(!fs::exists(f.destination("new")) && read(f.destination("empty/keep")) == "kept");
    pump.cleanup(service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
  }
  void testReplacementGuards(Pump& pump) {
    for (int scenario = 0; scenario < 6; ++scenario) {
      Fixture f;
      luciole::TransferService service;
      write(f.source("a"), "new-a");
      fs::create_directories(f.source("b/sub"));
      write(f.source("b/sub/value"), "new-b");
      write(f.destination("a"), "old-a");
      fs::create_directories(f.destination("b/sub"));
      write(f.destination("b/sub/value"), "old-b");
      struct stat oldB{};
      TEST_CHECK(::lstat(f.destination("b").c_str(), &oldB) == 0);
      pump.run(
          service, {location(f.source("a")), location(f.source("b"))}, f.destination(""), TransferAction::Copy,
          [](const auto&) { return TransferConflictChoice::Replace; }
      );
      auto receipt = service.copyReceipt();
      TEST_CHECK(receipt);
      const auto retained = retainedItem(f, oldB);
      if (scenario == 0) {
        write(f.destination("b/sub/value"), "edited-new");
      } else if (scenario == 1) {
        write(retained / "sub/value", "edited-old");
      } else if (scenario == 2) {
        fs::rename(f.destination("b"), f.root / "held-b");
        fs::create_directory(f.destination("b"));
      } else if (scenario == 3) {
        fs::rename(f.destination(""), f.root / "held-parent");
        fs::create_directory(f.destination(""));
      } else if (scenario == 4) {
        pump.replayCopy(service, receipt);
        receipt = service.copyReceipt();
        write(f.destination("b/sub/value"), "edited-restored-old");
      } else {
        fs::rename(retained.parent_path(), f.destination("changed-stage"));
      }
      pump.replayCopy(service, receipt);
      TEST_CHECK(!service.copyReceipt() && service.historyRetired());
      TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
        return result.outcome == TransferOutcome::Failed;
      }));
      TEST_CHECK(
          read((scenario == 3 ? f.root / "held-parent/a" : f.destination("a"))) == (scenario == 4 ? "old-a" : "new-a")
      );
      TEST_CHECK(read(f.source("a")) == "new-a" && read(f.source("b/sub/value")) == "new-b");
      receipt.reset();
      pump.cleanup(service);
      TEST_CHECK(!luciole::CopyReceipt::takeCleanupErrors().empty());
      if (scenario == 1) {
        TEST_CHECK(read(retained / "sub/value") == "edited-old");
      }
      if (scenario == 4) {
        TEST_CHECK(read(f.destination("b/sub/value")) == "edited-restored-old");
      }
    }
  }
  void testReplacementAlias(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    write(f.source("item"), "new");
    write(f.destination("item"), "old");
    fs::create_hard_link(f.destination("item"), f.root / "alias");
    pump.run(service, {location(f.source("item"))}, f.destination(""), TransferAction::Copy, [](const auto&) {
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(service.copyReceipt() && service.results()[0].outcome == TransferOutcome::Copied);
    TEST_CHECK(read(f.destination("item")) == "new" && read(f.root / "alias") == "old");
    pump.replayCopy(service, service.copyReceipt());
    TEST_CHECK(service.copyReceipt() && fs::equivalent(f.destination("item"), f.root / "alias"));
    pump.replayCopy(service, service.copyReceipt());
    TEST_CHECK(service.copyReceipt() && read(f.destination("item")) == "new");
    pump.cleanup(service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() && read(f.root / "alias") == "old");
  }

  void testIncompleteReplacement(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    write(f.source("item"), "new");
    write(f.destination("item"), "old");
    struct stat old{};
    TEST_CHECK(::stat(f.destination("item").c_str(), &old) == 0);
    pump.run(
        service, {location(f.source("item")), location(f.source("missing"))}, f.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(!service.copyReceipt() && service.results()[0].outcome == TransferOutcome::Copied);
    TEST_CHECK(service.results()[1].outcome == TransferOutcome::Failed);
    const auto retained = retainedItem(f, old);
    TEST_CHECK(service.report().contains(location(retained).uri()));
    pump.cleanup(service);
    TEST_CHECK(!luciole::CopyReceipt::takeCleanupErrors().empty());
    TEST_CHECK(read(retained) == "old" && read(f.destination("item")) == "new");
  }

  void testReplacementCancellation(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    constexpr std::size_t count = 128;
    std::vector<luciole::Location> sources;
    for (std::size_t i = 0; i < count; ++i) {
      const auto name = std::to_string(i);
      write(f.source(name), "new-" + name);
      write(f.destination(name), "old-" + name);
      sources.push_back(location(f.source(name)));
    }
    pump.run(service, sources, f.destination(""), TransferAction::Copy, [](const auto&) {
      return TransferConflictChoice::Replace;
    });
    auto receipt = service.copyReceipt();
    TEST_CHECK(receipt);
    luciole::CancelToken token;
    token.cancel();
    std::atomic<std::size_t> completed{0};
    auto result = receipt->reverse(token, completed);
    TEST_CHECK(!result.retireHistory && completed.load() == count);
    TEST_CHECK(std::ranges::all_of(result.results, [](const auto& item) {
      return item.outcome == TransferOutcome::Cancelled;
    }));
    token = luciole::CancelToken{};
    completed.store(0);
    std::jthread interrupt([&](const std::stop_token& stop) {
      while (!stop.stop_requested() && completed.load() == 0) {
        std::this_thread::yield();
      }
      token.cancel();
    });
    result = receipt->reverse(token, completed);
    interrupt.request_stop();
    interrupt.join();
    TEST_CHECK(completed.load() == count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& item = result.results[i];
      const auto name = std::to_string(i);
      const bool moved = item.outcome == TransferOutcome::Moved;
      TEST_CHECK(moved || item.outcome == TransferOutcome::Cancelled);
      TEST_CHECK(read(f.destination(name)) == (moved ? "old-" : "new-") + name);
      if (moved) {
        TEST_CHECK(item.children.size() == 2 && item.children[0].destination);
        TEST_CHECK(read(*item.children[0].destination->nativePath()) == "new-" + name);
      } else {
        TEST_CHECK(item.destination && read(*item.destination->nativePath()) == "old-" + name);
      }
      TEST_CHECK(read(f.source(name)) == "new-" + name);
    }
    result.receipt.reset();
    receipt.reset();
    pump.cleanup(service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() == !result.retireHistory);
  }

  void testFailedRetentionHandoff() {
    Fixture f;
    write(f.source("item"), "incoming");
    fs::create_directories(f.destination("item/sub"));
    write(f.destination("item/sub/old"), "previous");
    write(f.destination("item/sub/keep"), "unrelated old data");
    auto sourceParent = luciole::checkedFd(::open(f.source("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    auto targetParent = luciole::checkedFd(::open(f.destination("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto source = location(f.source("item"));
    auto target = std::make_shared<const luciole::transfer::Destination>(
        luciole::transfer::Destination{location(f.destination("")), luciole::statFd(targetParent.get()), {}}
    );
    luciole::transfer::Node node(
        {0,
         source,
         {},
         "item",
         luciole::transfer::info(sourceParent.get(), "item"),
         f.source("").native(),
         luciole::statFd(sourceParent.get())},
        target, luciole::TransferResult(source)
    );
    std::atomic<std::uint64_t> bytes{0};
    luciole::transfer::Work work({}, bytes, [](const auto&, const auto&, const auto&, const auto&, bool) {
      return TransferConflictChoice::Replace;
    });
    std::size_t remaining = 16384;
    work.prepareCopy = [&](const auto&, const auto& destination, int parent, int stage, bool replacing) {
      auto proof = luciole::CopyEffect::prepare(destination, parent, stage, replacing, remaining, {});
      // An external writer changes a nested previous item after capture; its directory stamp is unchanged.
      write(f.destination("item/sub/old"), "edited previous data");
      return proof;
    };
    work.recordCopy = [](const auto& proof, int from) { (void)proof->published(from); };
    luciole::transfer::execute(node, TransferAction::Copy, work, false);
    TEST_CHECK(node.result.outcome == TransferOutcome::Partial && !node.result.retryable && node.result.error);
    TEST_CHECK(read(f.destination("item")) == "incoming" && read(f.source("item")) == "incoming");
    bool found = false;
    for (const auto& item : fs::directory_iterator(f.destination(""))) {
      if (item.path().filename().native().starts_with(".luciole-transfer-")) {
        TEST_CHECK(read(item.path() / "item/sub/old") == "edited previous data");
        TEST_CHECK(read(item.path() / "item/sub/keep") == "unrelated old data");
        TEST_CHECK(node.result.error->message.contains(item.path().filename().native()));
        found = true;
      }
    }
    TEST_CHECK(found);
  }

  void testAliasRemovalDuringPublication() {
    Fixture f;
    write(f.source("item"), "incoming");
    write(f.destination("item"), "previous");
    fs::create_hard_link(f.destination("item"), f.root / "alias");
    auto from = luciole::checkedFd(::open(f.source("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    auto to = luciole::checkedFd(::open(f.destination("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    luciole::transfer::Source captured{
        0,
        location(f.source("item")),
        {},
        "item",
        luciole::transfer::info(from.get(), "item"),
        f.source("").native(),
        luciole::statFd(from.get())
    };
    auto destination = std::make_shared<const luciole::transfer::Destination>(
        luciole::transfer::Destination{location(f.destination("")), luciole::statFd(to.get()), {}}
    );
    luciole::transfer::Node node(std::move(captured), destination, luciole::TransferResult(location(f.source("item"))));
    std::atomic<std::uint64_t> bytes{0};
    luciole::transfer::Work work({}, bytes, [](const auto&, const auto&, const auto&, const auto&, bool) {
      return TransferConflictChoice::Replace;
    });
    work.history = std::make_shared<luciole::HistoryRevisions>();
    work.prepareCopy = [&](const auto&, const auto& target, int parent, int stage, bool replacing) {
      std::size_t remaining = 32;
      auto proof = luciole::CopyEffect::prepare(target, parent, stage, replacing, remaining, {}, work.history);
      // The rename must not adopt an external unlink as its own cleanup.
      TEST_CHECK(fs::remove(f.root / "alias"));
      return proof;
    };
    bool recorded = false;
    work.recordCopy = [&](const auto&, int) { recorded = true; };
    luciole::transfer::execute(node, TransferAction::Copy, work, false);
    TEST_CHECK(node.result.outcome == TransferOutcome::Partial && node.result.error && !recorded);
    TEST_CHECK(read(f.source("item")) == "incoming" && read(f.destination("item")) == "incoming");
    bool retained = false;
    for (const auto& entry : fs::directory_iterator(f.destination(""))) {
      if (entry.path().filename().native().starts_with(".luciole-transfer-")) {
        TEST_CHECK(read(entry.path() / "item") == "previous");
        TEST_CHECK(node.result.error->message.contains(entry.path().filename().native()));
        retained = true;
      }
    }
    TEST_CHECK(retained);
  }

  void testRepeatedDestinationRecovery(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    fs::create_directories(f.source("one"));
    fs::create_directories(f.source("two"));
    write(f.source("one/item"), "first");
    write(f.source("two/item"), "second");
    write(f.destination("item"), "original");
    pump.run(
        service, {location(f.source("one/item")), location(f.source("two/item"))}, f.destination(""),
        TransferAction::Copy, [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(service.copyReceipt() && service.copyReceipt()->size() == 1 && read(f.destination("item")) == "second");
    for (int cycle = 0; cycle < 3; ++cycle) {
      pump.replayCopy(service, service.copyReceipt());
      TEST_CHECK(service.copyReceipt() && read(f.destination("item")) == "original");
      pump.replayCopy(service, service.copyReceipt());
      TEST_CHECK(service.copyReceipt() && read(f.destination("item")) == "second");
    }
    pump.cleanup(service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    TEST_CHECK(read(f.source("one/item")) == "first" && read(f.source("two/item")) == "second");
  }

  void testMoveReplacementRecovery(Pump& pump) {
    for (const bool finishUndone : {false, true}) {
      for (int kind = 0; kind < 4; ++kind) {
        Fixture f;
        luciole::TransferService service;
        const std::string name = "item 文";
        if (kind == 1 || kind == 2) {
          fs::create_directories(f.source(name) / "sub");
          write(f.source(name) / "sub/new", "incoming");
        } else if (kind == 3) {
          fs::create_symlink("missing-new", f.source(name));
        } else {
          write(f.source(name), "incoming");
        }
        if (kind == 1 || kind == 0) {
          fs::create_directories(f.destination(name) / "sub");
          write(f.destination(name) / "sub/old", "previous");
          fs::create_symlink("missing-old-child", f.destination(name) / "sub/link");
        } else if (kind == 3) {
          fs::create_symlink("missing-old", f.destination(name));
        } else {
          write(f.destination(name), "previous");
        }
        struct stat source{}, previous{};
        TEST_CHECK(::lstat(f.source(name).c_str(), &source) == 0);
        TEST_CHECK(::lstat(f.destination(name).c_str(), &previous) == 0);
        pump.run(service, {location(f.source(name))}, f.destination(""), TransferAction::Move, [](const auto&) {
          return TransferConflictChoice::Replace;
        });
        TEST_CHECK(
            service.receipt() && service.results()[0].outcome == TransferOutcome::Moved && !service.results()[0].error
        );
        TEST_CHECK(fs::symlink_status(f.source(name)).type() == fs::file_type::not_found);
        const auto checkIncoming = [&](const fs::path& path) {
          struct stat now{};
          TEST_CHECK(::lstat(path.c_str(), &now) == 0 && now.st_ino == source.st_ino && now.st_dev == source.st_dev);
          if (kind == 1 || kind == 2) {
            TEST_CHECK(read(path / "sub/new") == "incoming");
          } else if (kind == 3) {
            TEST_CHECK(fs::read_symlink(path) == "missing-new");
          } else {
            TEST_CHECK(read(path) == "incoming");
          }
        };
        const auto checkOld = [&] {
          struct stat now{};
          TEST_CHECK(
              ::lstat(f.destination(name).c_str(), &now) == 0
              && now.st_ino == previous.st_ino
              && now.st_mode == previous.st_mode
          );
          if (kind == 1 || kind == 0) {
            TEST_CHECK(read(f.destination(name) / "sub/old") == "previous");
          } else if (kind == 3) {
            TEST_CHECK(fs::read_symlink(f.destination(name)) == "missing-old");
          } else {
            TEST_CHECK(read(f.destination(name)) == "previous");
          }
        };
        for (int cycle = 0; cycle < 3; ++cycle) {
          checkIncoming(f.destination(name));
          pump.replay(service, service.receipt());
          TEST_CHECK(service.receipt() && !service.historyRetired());
          checkIncoming(f.source(name));
          checkOld();
          pump.replay(service, service.receipt());
          TEST_CHECK(service.receipt() && !service.historyRetired());
        }
        if (finishUndone) {
          pump.replay(service, service.receipt());
          checkOld();
          checkIncoming(f.source(name));
        }
        service.takeReceipt().reset();
        pump.until([] { return !luciole::CopyReceipt::cleanupPending(); });
        TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
        if (finishUndone) {
          checkOld();
          checkIncoming(f.source(name));
        } else {
          checkIncoming(f.destination(name));
        }
        for (const auto& entry : fs::recursive_directory_iterator(f.destination(""))) {
          TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
        }
        f.clean();
      }
    }
  }
  void testMoveMergeRecovery(Pump& pump) {
    for (const bool finishUndone : {false, true}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("tree/sub/new-dir"));
      fs::create_directories(f.source("tree/whole/deep"));
      fs::create_directories(f.source("tree/empty"));
      fs::create_directories(f.destination("tree/sub"));
      fs::create_directories(f.destination("tree/empty"));
      write(f.source("tree/sub/conflict"), "incoming");
      write(f.source("tree/sub/new-dir/new"), "new");
      write(f.source("tree/whole/deep/value"), "whole");
      write(f.source("loose"), "loose");
      write(f.destination("tree/sub/conflict"), "old");
      write(f.destination("tree/keep"), "keep");
      TEST_CHECK(::chmod(f.source("tree").c_str(), 0750) == 0);
      struct stat root{}, sub{}, empty{}, destination{};
      TEST_CHECK(::lstat(f.source("tree").c_str(), &root) == 0);
      TEST_CHECK(::lstat(f.source("tree/sub").c_str(), &sub) == 0);
      TEST_CHECK(::lstat(f.source("tree/empty").c_str(), &empty) == 0);
      TEST_CHECK(::lstat(f.destination("tree").c_str(), &destination) == 0);
      pump.run(
          service, {location(f.source("tree")), location(f.source("loose"))}, f.destination(""), TransferAction::Move,
          [](const auto& conflict) {
            return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
          }
      );
      if (!service.receipt()) {
        std::cerr << service.report() << '\n';
      }
      TEST_CHECK(service.receipt() && !fs::exists(f.source("tree")) && !fs::exists(f.source("loose")));
      write(f.destination("tree/later"), "later");
      write(f.destination("tree/keep"), "edited keep");
      for (int cycle = 0; cycle < 3; ++cycle) {
        pump.replay(service, service.receipt());
        if (!service.receipt()) {
          std::cerr << service.report() << '\n';
        }
        TEST_CHECK(service.receipt() && !service.historyRetired());
        struct stat now{};
        TEST_CHECK(
            ::lstat(f.source("tree").c_str(), &now) == 0 && now.st_ino == root.st_ino && now.st_mode == root.st_mode
        );
        TEST_CHECK(::lstat(f.source("tree/sub").c_str(), &now) == 0 && now.st_ino == sub.st_ino);
        TEST_CHECK(::lstat(f.source("tree/empty").c_str(), &now) == 0 && now.st_ino == empty.st_ino);
        TEST_CHECK(
            read(f.source("tree/sub/conflict")) == "incoming" && read(f.destination("tree/sub/conflict")) == "old"
        );
        TEST_CHECK(
            read(f.source("tree/sub/new-dir/new")) == "new" && read(f.source("tree/whole/deep/value")) == "whole"
        );
        TEST_CHECK(read(f.source("loose")) == "loose");
        TEST_CHECK(read(f.destination("tree/later")) == "later" && read(f.destination("tree/keep")) == "edited keep");
        pump.replay(service, service.receipt());
        if (!service.receipt()) {
          std::cerr << service.report() << '\n';
        }
        TEST_CHECK(service.receipt() && !service.historyRetired() && !fs::exists(f.source("tree")));
        TEST_CHECK(read(f.destination("tree/sub/conflict")) == "incoming");
      }
      if (finishUndone) {
        pump.replay(service, service.receipt());
        TEST_CHECK(service.receipt());
      }
      service.takeReceipt().reset();
      pump.until([] { return !luciole::CopyReceipt::cleanupPending(); });
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
      struct stat now{};
      TEST_CHECK(::lstat(f.destination("tree").c_str(), &now) == 0 && now.st_ino == destination.st_ino);
      TEST_CHECK(read(f.destination("tree/later")) == "later" && read(f.destination("tree/keep")) == "edited keep");
      for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
        TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
      }
      f.clean();
    }
  }

  void testMoveRecoveryGuards(Pump& pump) {
    for (int variant = 0; variant < 6; ++variant) {
      Fixture f;
      {
        luciole::TransferService service;
        fs::create_directories(f.source("tree/sub"));
        fs::create_directories(f.destination("tree/sub"));
        write(f.source("tree/sub/item"), "new");
        write(f.destination("tree/sub/item"), "old");
        write(f.destination("tree/keep"), "keep");
        struct stat old{};
        TEST_CHECK(::lstat(f.destination("tree/sub/item").c_str(), &old) == 0);
        pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Move, [](const auto& c) {
          return c.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
        });
        TEST_CHECK(service.receipt());
        const auto original = service.receipt();
        const auto previous = retainedItem(f, old);
        if (variant == 0) {
          fs::create_directory(f.source("tree"));
          write(f.source("tree/foreign"), "foreign");
        }
        if (variant == 1) {
          write(previous, "edited old");
        }
        if (variant == 2) {
          write(f.destination("tree/sub/item"), "edited new");
        }
        if (variant == 3) {
          fs::rename(f.root / "source", f.root / "held-source");
          fs::create_directory(f.root / "source");
        }
        if (variant >= 4) {
          pump.replay(service, service.receipt());
          TEST_CHECK(service.receipt());
          if (variant == 4) {
            write(f.source("tree/unrecorded"), "new child");
          } else {
            pump.replay(service, original);
            TEST_CHECK(service.historyRetired() && !service.receipt());
            TEST_CHECK(read(f.source("tree/sub/item")) == "new" && read(f.destination("tree/sub/item")) == "old");
          }
        }
        if (variant < 5) {
          pump.replay(service, service.receipt());
          TEST_CHECK(!service.receipt());
          if (variant == 0) {
            TEST_CHECK(!service.historyRetired() && fs::exists(f.source("tree/foreign")));
            fs::remove_all(f.source("tree"));
            pump.replay(service, original);
            TEST_CHECK(service.receipt() && read(f.source("tree/sub/item")) == "new");
          } else {
            TEST_CHECK(service.historyRetired());
          }
        }
        if (variant == 1) {
          TEST_CHECK(read(previous) == "edited old" && read(f.destination("tree/sub/item")) == "new");
        }
        if (variant == 2) {
          TEST_CHECK(read(previous) == "old" && read(f.destination("tree/sub/item")) == "edited new");
        }
        if (variant == 3) {
          TEST_CHECK(fs::is_empty(f.root / "source") && read(f.destination("tree/sub/item")) == "new");
        }
        if (variant == 4) {
          TEST_CHECK(read(f.source("tree/unrecorded")) == "new child" && read(f.source("tree/sub/item")) == "new");
        }
        TEST_CHECK(read(f.destination("tree/keep")) == "keep");
      }
      drainHistoryCleanup();
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      if (variant > 0 && variant < 4) {
        TEST_CHECK(!errors.empty());
      } else {
        TEST_CHECK(errors.empty());
      }
      f.clean();
    }
    // An empty merge still removes a source container and is one reversible move.
    Fixture f;
    luciole::TransferService service;
    fs::create_directory(f.source("empty"));
    fs::create_directory(f.destination("empty"));
    struct stat original{};
    TEST_CHECK(::lstat(f.source("empty").c_str(), &original) == 0);
    pump.run(service, {location(f.source("empty"))}, f.destination(""), TransferAction::Move, [](const auto&) {
      return TransferConflictChoice::Merge;
    });
    TEST_CHECK(service.receipt() && service.receipt()->size() == 1);
    pump.replay(service, service.receipt());
    struct stat restored{};
    TEST_CHECK(
        service.receipt() && ::lstat(f.source("empty").c_str(), &restored) == 0 && restored.st_ino == original.st_ino
    );
    pump.replay(service, service.receipt());
    TEST_CHECK(service.receipt() && !fs::exists(f.source("empty")));
  }
  void testMoveRetentionHandoffFailure() {
    Fixture f;
    write(f.source("item"), "incoming");
    fs::create_directories(f.destination("item/sub"));
    write(f.destination("item/sub/old"), "previous");
    auto from = luciole::checkedFd(::open(f.source("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    auto to = luciole::checkedFd(::open(f.destination("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto source = luciole::transfer::info(from.get(), "item");
    luciole::transfer::Source captured{
        0, location(f.source("item")), std::move(from), "item", source, f.source("").native(), {}
    };
    captured.parentMetadata = luciole::statFd(captured.parent.get());
    auto destination = std::make_shared<const luciole::transfer::Destination>(
        luciole::transfer::Destination{location(f.destination("")), luciole::statFd(to.get()), {}}
    );
    luciole::transfer::Node node(std::move(captured), destination, luciole::TransferResult(location(f.source("item"))));
    std::atomic<std::uint64_t> bytes{0};
    luciole::transfer::Work work({}, bytes, [](const auto&, const auto&, const auto&, const auto&, bool) {
      return TransferConflictChoice::Replace;
    });
    work.prepareMove = [&](const auto& item, const auto& target, int parent) {
      std::size_t remaining = 32;
      auto proof =
          luciole::MoveEffect::prepareReplacement(item.location, target, item.parent.get(), parent, remaining, {});
      write(f.destination("item/sub/old"), "changed previous");
      return proof;
    };
    work.recordPreparedMove = [](const auto& proof) { (void)proof->publishedReplacement(); };
    luciole::transfer::execute(node, TransferAction::Move, work, false);
    TEST_CHECK(node.result.outcome == TransferOutcome::Partial && node.result.error && !node.result.retryable);
    TEST_CHECK(read(f.destination("item")) == "incoming" && read(f.source("item/sub/old")) == "changed previous");
    TEST_CHECK(node.result.error->message.contains("original source"));
    f.clean();
  }

  void testMoveReplacementCancellation(Pump& pump) {
    Fixture f;
    {
      luciole::TransferService service;
      std::vector<luciole::Location> sources;
      for (int n = 0; n < 128; ++n) {
        const auto name = std::to_string(n);
        write(f.source(name), "incoming:" + name);
        write(f.destination(name), "previous:" + name);
        sources.push_back(location(f.source(name)));
      }
      pump.run(service, sources, f.destination(""), TransferAction::Move, [](const auto&) {
        return TransferConflictChoice::Replace;
      });
      auto receipt = service.receipt();
      TEST_CHECK(receipt && service.replay(receipt));
      service.cancel();
      pump.until([&] { return !service.busy(); });
      if (!service.historyRetired()) {
        if (!service.receipt()) {
          pump.replay(service, receipt);
        } else {
          receipt = service.receipt();
          pump.replay(service, receipt);
        }
      }
      if (service.receipt()) {
        // Return to the forward state before testing a cancellation after an actual effect.
        if (fs::exists(f.source("0"))) {
          pump.replay(service, service.receipt());
        }
        receipt = service.receipt();
        TEST_CHECK(service.replay(receipt));
        pump.until([&] { return service.completed() > 0 || !service.busy(); });
        service.cancel();
        pump.until([&] { return !service.busy(); });
      }
      const auto verifyVersion = [&](const std::string& name) {
        if (fs::exists(f.source(name))) {
          TEST_CHECK(read(f.source(name)) == "incoming:" + name && read(f.destination(name)) == "previous:" + name);
        } else {
          TEST_CHECK(read(f.destination(name)) == "incoming:" + name);
          bool previous = false;
          for (const auto& child : fs::directory_iterator(f.destination(""))) {
            if (child.path().filename().native().starts_with(".luciole-create-")
                && read(child.path() / "item") == "previous:" + name) {
              previous = true;
              break;
            }
          }
          TEST_CHECK(previous);
        }
      };
      for (int n = 0; n < 128; ++n) {
        verifyVersion(std::to_string(n));
      }
      if (service.historyRetired()) {
        TEST_CHECK(!service.receipt() && service.report().contains(".luciole-create-"));
      }
    }
    drainHistoryCleanup();
    const auto errors = luciole::CopyReceipt::takeCleanupErrors();
    for (const auto& error : errors) {
      TEST_CHECK(error.message.contains("inspection"));
    }
  }

  void testMoveRecoveryAliasesAndChains(Pump& pump) {
    Fixture f;
    {
      luciole::TransferService service;
      write(f.source("item"), "incoming");
      write(f.destination("item"), "aliased previous");
      fs::create_hard_link(f.destination("item"), f.root / "alias");
      pump.run(service, {location(f.source("item"))}, f.destination(""), TransferAction::Move, [](const auto&) {
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(service.receipt() && service.results()[0].outcome == TransferOutcome::Moved);
      TEST_CHECK(read(f.destination("item")) == "incoming" && read(f.root / "alias") == "aliased previous");
      fs::create_directories(f.source("one"));
      fs::create_directories(f.source("two"));
      write(f.source("one/item"), "first");
      write(f.source("two/item"), "second");
      struct stat original{}, first{};
      TEST_CHECK(::lstat(f.destination("item").c_str(), &original) == 0);
      TEST_CHECK(::lstat(f.source("one/item").c_str(), &first) == 0);
      pump.run(
          service, {location(f.source("one/item")), location(f.source("two/item"))}, f.destination(""),
          TransferAction::Move, [](const auto&) { return TransferConflictChoice::Replace; }
      );
      TEST_CHECK(service.receipt() && read(f.destination("item")) == "second");
      for (int cycle = 0; cycle < 3; ++cycle) {
        pump.replay(service, service.receipt());
        TEST_CHECK(service.receipt() && read(f.destination("item")) == "incoming");
        TEST_CHECK(read(f.source("one/item")) == "first" && read(f.source("two/item")) == "second");
        pump.replay(service, service.receipt());
        TEST_CHECK(service.receipt() && read(f.destination("item")) == "second");
        TEST_CHECK(!fs::exists(f.source("one/item")) && !fs::exists(f.source("two/item")));
      }
    }
    drainHistoryCleanup();
    const auto errors = luciole::CopyReceipt::takeCleanupErrors();
    TEST_CHECK(errors.empty());
    f.clean();
  }

  bool hasTransferHistory(const luciole::TransferService& service, TransferAction action) {
    return action == TransferAction::Move ? service.receipt() != nullptr : service.copyReceipt() != nullptr;
  }
  void reverseTransfer(Pump& pump, luciole::TransferService& service, TransferAction action) {
    if (action == TransferAction::Move) {
      pump.replay(service, service.receipt());
    } else {
      pump.replayCopy(service, service.copyReceipt());
    }
    if (!hasTransferHistory(service, action)) {
      std::println(stderr, "{}", service.report());
    }
  }
  void dropTransferHistory(Pump& pump, luciole::TransferService& service) {
    service.takeReceipt().reset();
    service.takeCopyReceipt().reset();
    pump.until([] { return !luciole::CopyReceipt::cleanupPending(); });
  }
  void testAliasedPreviousTrees(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move, TransferAction::Link}) {
      for (int kind = 0; kind < 3; ++kind) {
        const bool nested = kind == 1;
        for (const bool finishUndone : {false, true}) {
          Fixture f;
          luciole::TransferService service;
          if (kind == 2) {
            write(f.root / "protected", "old shared data");
            fs::create_symlink(f.root / "protected", f.root / "alias");
          } else {
            write(f.root / "alias", "old shared data");
          }
          for (const auto* name : {"a", "b"}) {
            write(f.source(name), std::string("incoming ") + name);
            if (nested) {
              fs::create_directories(f.destination(std::string(name) + "/sub"));
              fs::create_hard_link(f.root / "alias", f.destination(std::string(name) + "/sub/x"));
              fs::create_hard_link(f.root / "alias", f.destination(std::string(name) + "/sub/y"));
            } else {
              fs::create_hard_link(f.root / "alias", f.destination(name));
            }
          }
          pump.run(
              service, {location(f.source("a")), location(f.source("b"))}, f.destination(""), action,
              [](const auto&) { return TransferConflictChoice::Replace; }
          );
          if (!hasTransferHistory(service, action)) {
            std::println(stderr, "{}", service.report());
          }
          TEST_CHECK(hasTransferHistory(service, action));
          for (int cycle = 0; cycle < 3; ++cycle) {
            reverseTransfer(pump, service, action);
            TEST_CHECK(hasTransferHistory(service, action));
            for (const auto* name : {"a", "b"}) {
              const auto restored = nested ? f.destination(std::string(name) + "/sub/x") : f.destination(name);
              TEST_CHECK(fs::equivalent(f.root / "alias", restored));
              if (kind == 2) {
                struct stat outside{}, current{};
                TEST_CHECK(
                    ::lstat((f.root / "alias").c_str(), &outside) == 0
                    && ::lstat(restored.c_str(), &current) == 0
                    && outside.st_ino == current.st_ino
                );
                TEST_CHECK(fs::read_symlink(restored) == f.root / "protected");
              }
              if (nested) {
                TEST_CHECK(fs::equivalent(restored, f.destination(std::string(name) + "/sub/y")));
              }
              if (action == TransferAction::Move) {
                TEST_CHECK(read(f.source(name)) == std::string("incoming ") + name);
              }
            }
            reverseTransfer(pump, service, action);
            TEST_CHECK(hasTransferHistory(service, action));
            TEST_CHECK(read(f.destination("a")) == "incoming a" && read(f.destination("b")) == "incoming b");
          }
          if (finishUndone) {
            reverseTransfer(pump, service, action);
            TEST_CHECK(hasTransferHistory(service, action));
          }
          dropTransferHistory(pump, service);
          TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
          TEST_CHECK(read(f.root / "alias") == "old shared data");
          struct stat after{};
          TEST_CHECK(::lstat((f.root / "alias").c_str(), &after) == 0);
          TEST_CHECK(after.st_nlink == (finishUndone ? (nested ? 5U : 3U) : 1U));
          for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
            TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
          }
        }
      }
    }
  }
  void testAliasMutationGuards(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move, TransferAction::Link}) {
      for (int change = 0; change < 4; ++change) {
        Fixture f;
        {
          luciole::TransferService service;
          write(f.source("a"), "new a");
          write(f.source("b"), "new b");
          write(f.destination("a"), "old");
          TEST_CHECK(::chmod(f.destination("a").c_str(), 0644) == 0);
          write(f.destination("b"), "old b");
          fs::create_hard_link(f.destination("a"), f.root / "alias");
          pump.run(
              service, {location(f.source("a")), location(f.source("b"))}, f.destination(""), action,
              [](const auto&) { return TransferConflictChoice::Replace; }
          );
          TEST_CHECK(hasTransferHistory(service, action));
          struct stat original{};
          TEST_CHECK(::lstat((f.root / "alias").c_str(), &original) == 0);
          if (change == 0) {
            write(f.root / "alias", "new");
            const struct timespec times[]{original.st_atim, original.st_mtim};
            TEST_CHECK(::utimensat(AT_FDCWD, (f.root / "alias").c_str(), times, 0) == 0);
          } else if (change == 1) {
            fs::create_hard_link(f.root / "alias", f.root / "extra");
          } else if (change == 2) {
            TEST_CHECK(fs::remove(f.root / "alias"));
          } else {
            TEST_CHECK(::chmod((f.root / "alias").c_str(), 0600) == 0);
          }
          reverseTransfer(pump, service, action);
          TEST_CHECK(!hasTransferHistory(service, action) && service.historyRetired());
          TEST_CHECK(read(f.destination("a")) == "new a" && read(f.destination("b")) == "new b");
          TEST_CHECK(service.report().contains(".luciole-create-"));
          if (action == TransferAction::Move) {
            TEST_CHECK(!fs::exists(f.source("a")) && !fs::exists(f.source("b")));
          }
        }
        drainHistoryCleanup();
        TEST_CHECK(!luciole::CopyReceipt::takeCleanupErrors().empty());
      }
    }
  }
  void testRepeatedTransferVersions(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move, TransferAction::Link}) {
      for (const bool original : {false, true}) {
        for (const bool finishUndone : {false, true}) {
          Fixture f;
          luciole::TransferService service;
          fs::create_directories(f.source("one/item/sub"));
          fs::create_directories(f.source("two"));
          fs::create_directories(f.source("three"));
          write(f.source("one/item/sub/file"), "first folder");
          write(f.source("two/item"), "second file");
          fs::create_symlink("absent-third", f.source("three/item"));
          if (original) {
            write(f.destination("item"), "original");
            fs::create_hard_link(f.destination("item"), f.root / "alias");
          }
          struct stat first{}, second{}, third{};
          TEST_CHECK(::lstat(f.source("one/item").c_str(), &first) == 0);
          TEST_CHECK(::lstat(f.source("two/item").c_str(), &second) == 0);
          TEST_CHECK(::lstat(f.source("three/item").c_str(), &third) == 0);
          pump.run(
              service,
              {location(f.source("one/item")), location(f.source("two/item")), location(f.source("three/item"))},
              f.destination(""), action, [](const auto&) { return TransferConflictChoice::Replace; }
          );
          if (!hasTransferHistory(service, action)) {
            std::println(stderr, "{}", service.report());
          }
          TEST_CHECK(hasTransferHistory(service, action));
          const auto checkForward = [&] {
            TEST_CHECK(
                fs::read_symlink(f.destination("item"))
                == (action == TransferAction::Link ? f.source("three/item") : fs::path("absent-third"))
            );
            if (action == TransferAction::Move) {
              TEST_CHECK(
                  !fs::exists(f.source("one/item"))
                  && !fs::exists(f.source("two/item"))
                  && !fs::is_symlink(f.source("three/item"))
              );
            }
          };
          const auto checkUndone = [&] {
            if (original) {
              TEST_CHECK(
                  fs::equivalent(f.destination("item"), f.root / "alias") && read(f.destination("item")) == "original"
              );
            } else {
              TEST_CHECK(fs::symlink_status(f.destination("item")).type() == fs::file_type::not_found);
            }
            TEST_CHECK(
                read(f.source("one/item/sub/file")) == "first folder" && read(f.source("two/item")) == "second file"
            );
            TEST_CHECK(fs::read_symlink(f.source("three/item")) == "absent-third");
            struct stat now{};
            TEST_CHECK(::lstat(f.source("one/item").c_str(), &now) == 0 && now.st_ino == first.st_ino);
            TEST_CHECK(::lstat(f.source("two/item").c_str(), &now) == 0 && now.st_ino == second.st_ino);
            TEST_CHECK(::lstat(f.source("three/item").c_str(), &now) == 0 && now.st_ino == third.st_ino);
          };
          for (int cycle = 0; cycle < 3; ++cycle) {
            checkForward();
            reverseTransfer(pump, service, action);
            TEST_CHECK(hasTransferHistory(service, action));
            checkUndone();
            reverseTransfer(pump, service, action);
            TEST_CHECK(hasTransferHistory(service, action));
          }
          if (finishUndone) {
            reverseTransfer(pump, service, action);
            TEST_CHECK(hasTransferHistory(service, action));
          }
          dropTransferHistory(pump, service);
          TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
          finishUndone ? checkUndone() : checkForward();
          for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
            TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
          }
        }
      }
    }
  }
  void testRepeatedMergeLeaves(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("one/tree/sub"));
      fs::create_directories(f.source("two/tree/sub"));
      fs::create_directories(f.destination("tree/sub"));
      write(f.source("one/tree/sub/item"), "first");
      write(f.source("two/tree/sub/item"), "second");
      write(f.destination("tree/sub/item"), "original");
      write(f.destination("tree/keep"), "keep");
      fs::create_hard_link(f.destination("tree/sub/item"), f.root / "alias");
      pump.run(
          service, {location(f.source("one/tree")), location(f.source("two/tree"))}, f.destination(""), action,
          [](const auto& c) { return c.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace; }
      );
      if (!hasTransferHistory(service, action)) {
        std::println(stderr, "{}", service.report());
      }
      TEST_CHECK(hasTransferHistory(service, action));
      for (int cycle = 0; cycle < 3; ++cycle) {
        reverseTransfer(pump, service, action);
        TEST_CHECK(hasTransferHistory(service, action));
        TEST_CHECK(read(f.source("one/tree/sub/item")) == "first" && read(f.source("two/tree/sub/item")) == "second");
        TEST_CHECK(fs::equivalent(f.destination("tree/sub/item"), f.root / "alias"));
        reverseTransfer(pump, service, action);
        TEST_CHECK(hasTransferHistory(service, action) && read(f.destination("tree/sub/item")) == "second");
        TEST_CHECK(read(f.destination("tree/keep")) == "keep");
      }
      dropTransferHistory(pump, service);
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() && read(f.root / "alias") == "original");
    }
  }
  void testIncomingAliasChains(Pump& pump) {
    for (const bool sameAsOld : {false, true}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("one"));
      fs::create_directories(f.source("two"));
      write(f.source("one/item"), "shared");
      fs::create_hard_link(f.source("one/item"), f.source("two/item"));
      if (sameAsOld) {
        fs::create_hard_link(f.source("one/item"), f.destination("item"));
      } else {
        write(f.destination("item"), "original");
      }
      pump.run(
          service, {location(f.source("one/item")), location(f.source("two/item"))}, f.destination(""),
          TransferAction::Move, [](const auto&) { return TransferConflictChoice::Replace; }
      );
      if (!service.receipt()) {
        std::println(stderr, "{}", service.report());
      }
      TEST_CHECK(service.receipt());
      for (int cycle = 0; cycle < 3; ++cycle) {
        pump.replay(service, service.receipt());
        TEST_CHECK(service.receipt() && fs::equivalent(f.source("one/item"), f.source("two/item")));
        TEST_CHECK(read(f.destination("item")) == (sameAsOld ? "shared" : "original"));
        if (sameAsOld) {
          TEST_CHECK(fs::equivalent(f.source("one/item"), f.destination("item")));
        }
        pump.replay(service, service.receipt());
        TEST_CHECK(service.receipt() && read(f.destination("item")) == "shared");
      }
      dropTransferHistory(pump, service);
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    }
  }

  void testRepeatedHistoryGuards(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move, TransferAction::Link}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("one"));
      fs::create_directories(f.source("two"));
      write(f.source("one/item"), "first");
      write(f.source("two/item"), "last");
      write(f.destination("item"), "original");
      fs::create_hard_link(f.destination("item"), f.root / "alias");
      pump.run(
          service, {location(f.source("one/item")), location(f.source("two/item"))}, f.destination(""), action,
          [](const auto&) { return TransferConflictChoice::Replace; }
      );
      TEST_CHECK(hasTransferHistory(service, action));
      if (action == TransferAction::Move) {
        const auto receipt = service.receipt();
        write(f.source("one/item"), "foreign");
        pump.replay(service, receipt);
        TEST_CHECK(!service.receipt() && !service.historyRetired());
        TEST_CHECK(
            read(f.source("one/item")) == "foreign"
            && read(f.destination("item")) == "last"
            && !fs::exists(f.source("two/item"))
        );
        TEST_CHECK(fs::remove(f.source("one/item")));
        pump.replay(service, receipt);
      } else {
        reverseTransfer(pump, service, action);
      }
      TEST_CHECK(hasTransferHistory(service, action) && read(f.destination("item")) == "original");
      if (action == TransferAction::Move) {
        write(f.source("one/item"), "changed first");
        reverseTransfer(pump, service, action);
        TEST_CHECK(!service.receipt() && service.historyRetired());
        TEST_CHECK(
            read(f.source("one/item")) == "changed first"
            && read(f.source("two/item")) == "last"
            && read(f.destination("item")) == "original"
        );
      } else {
        // Source edits after Undo do not alter retained-inode Copy/Link Redo.
        write(f.source("one/item"), "changed first");
        reverseTransfer(pump, service, action);
        TEST_CHECK(hasTransferHistory(service, action) && read(f.destination("item")) == "last");
      }
      dropTransferHistory(pump, service);
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    }
  }
  void testRepeatedMoveCancellation(Pump& pump) {
    Fixture f;
    {
      luciole::TransferService service;
      std::vector<luciole::Location> sources;
      for (int n = 0; n < 128; ++n) {
        const auto parent = std::to_string(n);
        fs::create_directory(f.source(parent));
        write(f.source(parent + "/item"), "version-" + parent);
        sources.push_back(location(f.source(parent + "/item")));
      }
      write(f.destination("item"), "original");
      pump.run(service, sources, f.destination(""), TransferAction::Move, [](const auto&) {
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(service.receipt() && service.replay(service.receipt()));
      pump.until([&] { return service.completed() > 0 || !service.busy(); });
      service.cancel();
      pump.until([&] { return !service.busy(); });
      std::map<std::string, int> versions;
      for (const auto& path : fs::recursive_directory_iterator(f.root)) {
        if (path.is_regular_file()) {
          ++versions[read(path.path())];
        }
      }
      TEST_CHECK(versions.size() == 129 && versions["original"] == 1);
      for (int n = 0; n < 128; ++n) {
        TEST_CHECK(versions["version-" + std::to_string(n)] == 1);
      }
      if (service.historyRetired()) {
        TEST_CHECK(!service.receipt() && service.report().contains(".luciole-create-"));
      } else {
        TEST_CHECK(service.receipt() && read(f.destination("item")) == "original");
      }
    }
    drainHistoryCleanup();
    for (const auto& error : luciole::CopyReceipt::takeCleanupErrors()) {
      TEST_CHECK(error.message.contains("inspection"));
    }
  }
  std::vector<luciole::Location> overlappingCopies(Fixture& f) {
    fs::create_directories(f.source("one/tree/sub/empty"));
    fs::create_directories(f.source("one/tree/sub/becomes-file"));
    fs::create_directories(f.source("two/tree/sub/becomes-folder"));
    fs::create_directories(f.source("two/tree/sub/fresh"));
    fs::create_directories(f.source("three/tree/sub/fresh"));
    TEST_CHECK(::chmod(f.source("one/tree/sub").c_str(), 0755) == 0);
    write(f.source("one/tree/sub/item"), "first");
    write(f.source("one/tree/sub/keep"), "untouched");
    write(f.source("one/tree/sub/.hidden"), "hidden");
    write(f.source("one/tree/sub/becomes-folder"), "old file");
    write(f.source("one/tree/sub/becomes-file/old"), "old folder");
    fs::create_symlink("missing-first", f.source("one/tree/sub/link"));
    write(f.source("two/tree/sub/item"), "second");
    write(f.source("two/tree/sub/becomes-folder/new"), "new folder");
    write(f.source("two/tree/sub/becomes-file"), "new file");
    write(f.source("two/tree/sub/fresh/data"), "fresh data");
    fs::create_symlink("missing-second", f.source("two/tree/sub/link"));
    write(f.source("three/tree/sub/item"), "third");
    write(f.source("three/tree/sub/fresh/added"), "added data");
    write(f.source("three/tree/sub/" + std::string("raw-") + static_cast<char>(0xff)), "raw name");
    return {location(f.source("one/tree")), location(f.source("two/tree")), location(f.source("three/tree"))};
  }
  void testOverlappingCopies(Pump& pump) {
    for (int original = 0; original < 3; ++original) {
      for (const bool finishUndone : {false, true}) {
        Fixture f;
        luciole::TransferService service;
        const auto sources = overlappingCopies(f);
        if (original == 2) {
          fs::create_directories(f.destination("tree/old"));
          write(f.destination("tree/old/item"), "original");
        } else if (original == 1) {
          write(f.destination("tree"), "original");
        }
        if (original) {
          fs::create_hard_link(
              original == 2 ? f.destination("tree/old/item") : f.destination("tree"), f.root / "alias"
          );
        }
        struct stat before{};
        if (original) {
          TEST_CHECK(::lstat(f.destination("tree").c_str(), &before) == 0);
        }
        pump.run(service, sources, f.destination(""), TransferAction::Copy, [&](const auto& c) {
          return c.canMerge && c.source != sources.front() ? TransferConflictChoice::Merge
                                                           : TransferConflictChoice::Replace;
        });
        TEST_CHECK(service.copyReceipt() && service.copyReceipt()->size() == 1);
        TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& r) { return !r.error; }));
        const auto tree = f.destination("tree");
        const auto inode = luciole::transfer::info(AT_FDCWD, tree.native());
        for (int cycle = 0; cycle < 3; ++cycle) {
          TEST_CHECK(read(tree / "sub/item") == "third" && read(tree / "sub/keep") == "untouched");
          TEST_CHECK(
              read(tree / "sub/becomes-file") == "new file" && read(tree / "sub/becomes-folder/new") == "new folder"
          );
          TEST_CHECK(read(tree / "sub/.hidden") == "hidden" && fs::is_empty(tree / "sub/empty"));
          TEST_CHECK(read(tree / "sub/fresh/data") == "fresh data" && read(tree / "sub/fresh/added") == "added data");
          TEST_CHECK(fs::read_symlink(tree / "sub/link") == "missing-second");
          TEST_CHECK(read(tree / "sub" / (std::string("raw-") + static_cast<char>(0xff))) == "raw name");
          for (const auto& entry : fs::recursive_directory_iterator(tree)) {
            TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
          }
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(service.copyReceipt());
          if (original) {
            TEST_CHECK(luciole::sameIdentity(before, luciole::transfer::info(AT_FDCWD, tree.native())));
            TEST_CHECK(fs::equivalent(original == 2 ? tree / "old/item" : tree, f.root / "alias"));
          } else {
            TEST_CHECK(!fs::exists(tree));
          }
          write(f.destination("later-sibling"), "outside edit");
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(
              service.copyReceipt() && luciole::sameIdentity(inode, luciole::transfer::info(AT_FDCWD, tree.native()))
          );
          TEST_CHECK(read(f.destination("later-sibling")) == "outside edit");
        }
        if (finishUndone) {
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(service.copyReceipt());
        }
        dropTransferHistory(pump, service);
        TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
        TEST_CHECK(read(f.source("one/tree/sub/item")) == "first" && read(f.source("two/tree/sub/item")) == "second");
        TEST_CHECK(read(f.source("three/tree/sub/item")) == "third");
        for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
          TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
        }
      }
    }
  }
  void testOverlappingCopyGuards(Pump& pump) {
    for (int change = 0; change < 6; ++change) {
      Fixture f;
      {
        luciole::TransferService service;
        const auto sources = overlappingCopies(f);
        pump.run(service, sources, f.destination(""), TransferAction::Copy, [](const auto& c) {
          return c.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
        });
        TEST_CHECK(service.copyReceipt());
        const auto tree = f.destination("tree");
        if (change == 0) {
          const auto stamp = luciole::transfer::info(AT_FDCWD, (tree / "sub/keep").native());
          write(tree / "sub/keep", "corrupted");
          const struct timespec times[]{stamp.st_atim, stamp.st_mtim};
          TEST_CHECK(::utimensat(AT_FDCWD, (tree / "sub/keep").c_str(), times, 0) == 0);
        } else if (change == 1) {
          write(tree / "sub/extra", "user addition");
        } else if (change == 2) {
          fs::rename(tree / "sub", tree / "held");
          fs::create_directory_symlink(tree / "held", tree / "sub");
        } else if (change == 3) {
          TEST_CHECK(::chmod((tree / "sub").c_str(), 0700) == 0);
        } else {
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(service.copyReceipt());
          if (change == 4) {
            write(tree, "occupied");
            auto retry = service.copyReceipt();
            pump.replayCopy(service, retry);
            TEST_CHECK(!service.historyRetired() && read(tree) == "occupied");
            TEST_CHECK(
                service.results().front().error
                && service.results().front().error->kind == luciole::FileErrorKind::Exists
            );
            TEST_CHECK(fs::remove(tree));
            pump.replayCopy(service, retry);
            retry.reset();
            TEST_CHECK(service.copyReceipt() && read(tree / "sub/item") == "third");
            dropTransferHistory(pump, service);
            TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
            continue;
          }
          const auto retained = *service.results().front().destination->nativePath();
          write(fs::path(retained) / "sub/item", "retained edit");
        }
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(!service.copyReceipt() && service.historyRetired());
        if (change < 4) {
          TEST_CHECK(read(tree / "sub/item") == "third");
        } else {
          TEST_CHECK(!fs::exists(tree));
        }
        TEST_CHECK(read(f.source("one/tree/sub/item")) == "first");
      }
      drainHistoryCleanup();
      (void)luciole::CopyReceipt::takeCleanupErrors();
    }
  }
  void testChangedOverlapDuringTransfer(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      for (const bool addEntry : {false, true}) {
        Fixture f;
        {
          luciole::TransferService service;
          const auto sources = overlappingCopies(f);
          bool changed = false;
          pump.run(service, sources, f.destination(""), action, [&](const auto& c) {
            if (!c.canMerge && !changed) {
              write(f.destination(addEntry ? "tree/sub/extra" : "tree/sub/keep"), "outside edit");
              changed = true;
            }
            return c.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
          });
          TEST_CHECK(changed && !hasTransferHistory(service, action));
          TEST_CHECK(read(f.destination(addEntry ? "tree/sub/extra" : "tree/sub/keep")) == "outside edit");
          TEST_CHECK(read(f.destination("tree/sub/item")) == "third");
          TEST_CHECK(service.report().contains(".luciole-create-"));
          if (action == TransferAction::Copy) {
            TEST_CHECK(read(f.source("one/tree/sub/item")) == "first");
          }
        }
        drainHistoryCleanup();
        TEST_CHECK(!luciole::CopyReceipt::takeCleanupErrors().empty());
      }
    }
  }

  void testCopyReplacedAncestor(Pump& pump) {
    for (int original = 0; original < 3; ++original) {
      for (const bool finishUndone : {false, true}) {
        Fixture f;
        luciole::TransferService service;
        auto sources = overlappingCopies(f);
        if (original == 1) {
          write(f.destination("tree"), "original");
        }
        if (original == 2) {
          fs::create_directories(f.destination("tree/old"));
          write(f.destination("tree/old/item"), "original");
        }
        if (original) {
          fs::create_hard_link(
              original == 2 ? f.destination("tree/old/item") : f.destination("tree"), f.root / "alias"
          );
        }
        pump.run(service, sources, f.destination(""), TransferAction::Copy, [&](const auto& c) {
          return c.source == sources.front() || c.source == sources.back() || !c.canMerge
              ? TransferConflictChoice::Replace
              : TransferConflictChoice::Merge;
        });
        if (!service.copyReceipt()) {
          std::println(stderr, "{}", service.report());
        }
        TEST_CHECK(service.copyReceipt() && service.copyReceipt()->size() == 1);
        const auto final = luciole::transfer::info(AT_FDCWD, f.destination("tree").native());
        for (int cycle = 0; cycle < 3; ++cycle) {
          TEST_CHECK(read(f.destination("tree/sub/item")) == "third");
          TEST_CHECK(!fs::exists(f.destination("tree/sub/keep")) && !fs::exists(f.destination("tree/sub/fresh/data")));
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(service.copyReceipt());
          if (original) {
            TEST_CHECK(
                fs::equivalent(original == 2 ? f.destination("tree/old/item") : f.destination("tree"), f.root / "alias")
            );
          } else {
            TEST_CHECK(!fs::exists(f.destination("tree")));
          }
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(
              service.copyReceipt()
              && luciole::sameIdentity(final, luciole::transfer::info(AT_FDCWD, f.destination("tree").native()))
          );
        }
        if (finishUndone) {
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(service.copyReceipt());
        }
        dropTransferHistory(pump, service);
        TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
        TEST_CHECK(read(f.source("one/tree/sub/item")) == "first" && read(f.source("two/tree/sub/item")) == "second");
        for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
          TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
        }
      }
    }
  }
  void testCopyIntoExistingAncestor(Pump& pump) {
    for (const bool file : {false, true}) {
      for (const bool checkpoint : {false, true}) {
        for (const bool finishUndone : {false, true}) {
          Fixture f;
          luciole::TransferService service;
          auto sources = overlappingCopies(f);
          fs::create_directories(f.destination("tree/sub"));
          write(f.destination("tree/sub/item"), "original item");
          write(f.destination("tree/keep-original"), "original keep");
          fs::create_symlink("original-missing", f.destination("tree/original-link"));
          fs::create_hard_link(f.destination("tree/sub/item"), f.root / "alias");
          const auto before = luciole::transfer::info(AT_FDCWD, f.destination("tree").native());
          const auto sub = luciole::transfer::info(AT_FDCWD, f.destination("tree/sub").native());
          if (file) {
            fs::remove_all(f.source("three/tree"));
            write(f.source("three/tree"), "final file");
          }
          const auto parentReplacement = sources.back();
          if (checkpoint) {
            fs::create_directories(f.source("four/tree"));
            write(f.source("four/tree/final"), "final checkpoint");
            sources.push_back(location(f.source("four/tree")));
          }
          pump.run(service, sources, f.destination(""), TransferAction::Copy, [&](const auto& c) {
            if (c.source == parentReplacement) {
              write(f.destination("tree/keep-original"), "outside edit");
              write(f.destination("tree/user-added"), "user addition");
            }
            return c.source == parentReplacement || c.source == sources.back() || !c.canMerge
                ? TransferConflictChoice::Replace
                : TransferConflictChoice::Merge;
          });
          if (!service.copyReceipt()) {
            std::println(stderr, "{}", service.report());
          }
          TEST_CHECK(service.copyReceipt() && service.copyReceipt()->size() == 1);
          const auto after = luciole::transfer::info(AT_FDCWD, f.destination("tree").native());
          for (int cycle = 0; cycle < 3; ++cycle) {
            TEST_CHECK(
                checkpoint ? read(f.destination("tree/final")) == "final checkpoint"
                    : file ? read(f.destination("tree")) == "final file"
                           : read(f.destination("tree/sub/item")) == "third"
            );
            pump.replayCopy(service, service.copyReceipt());
            TEST_CHECK(
                service.copyReceipt()
                && luciole::sameIdentity(before, luciole::transfer::info(AT_FDCWD, f.destination("tree").native()))
            );
            TEST_CHECK(
                luciole::sameIdentity(sub, luciole::transfer::info(AT_FDCWD, f.destination("tree/sub").native()))
            );
            TEST_CHECK(fs::equivalent(f.destination("tree/sub/item"), f.root / "alias"));
            TEST_CHECK(read(f.destination("tree/sub/item")) == "original item");
            TEST_CHECK(
                read(f.destination("tree/keep-original")) == "outside edit"
                && read(f.destination("tree/user-added")) == "user addition"
            );
            TEST_CHECK(fs::read_symlink(f.destination("tree/original-link")) == "original-missing");
            TEST_CHECK(!fs::exists(f.destination("tree/sub/keep")) && !fs::exists(f.destination("tree/sub/fresh")));
            pump.replayCopy(service, service.copyReceipt());
            TEST_CHECK(
                service.copyReceipt()
                && luciole::sameIdentity(after, luciole::transfer::info(AT_FDCWD, f.destination("tree").native()))
            );
          }
          if (finishUndone) {
            pump.replayCopy(service, service.copyReceipt());
            TEST_CHECK(service.copyReceipt());
          }
          dropTransferHistory(pump, service);
          TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
          TEST_CHECK(read(f.source("one/tree/sub/item")) == "first" && read(f.source("two/tree/sub/item")) == "second");
          for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
            TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
          }
        }
      }
    }
  }

  void testNestedCopyAncestor(Pump& pump) {
    for (const bool newOuter : {false, true}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("one/outer/tree/sub"));
      fs::create_directories(f.source("two/outer"));
      fs::create_directories(f.source("three/outer"));
      write(f.source("one/outer/tree/sub/item"), "first copy");
      write(f.source("two/outer/tree"), "replacement file");
      write(f.source("three/outer/final"), "final copy");
      if (!newOuter) {
        fs::create_directories(f.destination("outer/tree/sub"));
        write(f.destination("outer/tree/sub/item"), "original");
        write(f.destination("outer/keep"), "kept");
      }
      const auto first = location(f.source("one/outer"));
      const auto last = location(f.source("three/outer"));
      pump.run(
          service, {first, location(f.source("two/outer")), last}, f.destination(""), TransferAction::Copy,
          [&](const auto& c) {
            return c.source == last || !c.canMerge ? TransferConflictChoice::Replace : TransferConflictChoice::Merge;
          }
      );
      if (!service.copyReceipt()) {
        std::println(stderr, "{}", service.report());
      }
      TEST_CHECK(service.copyReceipt() && read(f.destination("outer/final")) == "final copy");
      for (int n = 0; n < 3; ++n) {
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(service.copyReceipt());
        if (newOuter) {
          TEST_CHECK(!fs::exists(f.destination("outer")));
        } else {
          TEST_CHECK(
              read(f.destination("outer/tree/sub/item")) == "original" && read(f.destination("outer/keep")) == "kept"
          );
        }
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(service.copyReceipt() && read(f.destination("outer/final")) == "final copy");
      }
      dropTransferHistory(pump, service);
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
      TEST_CHECK(
          read(f.source("one/outer/tree/sub/item")) == "first copy"
          && read(f.source("two/outer/tree")) == "replacement file"
      );
    }
  }
  void testCopyAncestorGuards(Pump& pump) {
    for (int change = 0; change < 4; ++change) {
      Fixture f;
      {
        luciole::TransferService service;
        fs::create_directories(f.destination("tree/sub"));
        fs::create_directories(f.source("one/tree/sub"));
        fs::create_directories(f.source("two/tree"));
        write(f.destination("tree/sub/item"), "original");
        write(f.source("one/tree/sub/item"), "first copy");
        write(f.source("two/tree/final"), "final copy");
        const auto last = location(f.source("two/tree"));
        pump.run(
            service, {location(f.source("one/tree")), last}, f.destination(""), TransferAction::Copy,
            [&](const auto& c) {
              return c.source == last || !c.canMerge ? TransferConflictChoice::Replace : TransferConflictChoice::Merge;
            }
        );
        TEST_CHECK(service.copyReceipt());
        if (change == 0) {
          write(f.destination("tree/user-added"), "user addition");
        } else if (change == 1) {
          write(f.destination("tree/final"), "edited");
        } else if (change == 2) {
          const auto previous = service.copyReceipt();
          pump.replayCopy(service, previous);
          TEST_CHECK(service.copyReceipt() && read(f.destination("tree/sub/item")) == "original");
          write(f.destination("tree/sub/item"), "edited original");
        } else {
          pump.replayCopy(service, service.copyReceipt());
          TEST_CHECK(service.copyReceipt());
          write(f.destination("tree/later"), "later original child");
        }
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(!service.copyReceipt() && service.historyRetired());
        TEST_CHECK(read(f.source("one/tree/sub/item")) == "first copy");
        if (change == 0) {
          TEST_CHECK(read(f.destination("tree/user-added")) == "user addition");
        }
        if (change == 2) {
          TEST_CHECK(read(f.destination("tree/sub/item")) == "edited original");
        }
        if (change == 3) {
          TEST_CHECK(read(f.destination("tree/later")) == "later original child");
        }
      }
      drainHistoryCleanup();
      (void)luciole::CopyReceipt::takeCleanupErrors();
    }
  }

  void testPrivateParentReconstructionCancellation() {
    Fixture f;
    fs::create_directories(f.source("one/tree"));
    fs::create_directories(f.source("two/tree"));
    fs::create_directories(f.destination("tree"));
    for (int n = 0; n < 128; ++n) {
      write(f.source("one/tree/" + std::to_string(n)), "copy" + std::to_string(n));
      write(f.destination("tree/" + std::to_string(n)), "old" + std::to_string(n));
    }
    write(f.source("two/tree/final"), "final result");
    const auto old = luciole::transfer::info(AT_FDCWD, f.destination("tree").native());
    auto destination = luciole::checkedFd(::open(f.destination("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    auto binding = std::make_shared<const luciole::transfer::Destination>(
        luciole::transfer::Destination{location(f.destination("")), luciole::statFd(destination.get()), {}}
    );
    std::atomic<std::uint64_t> bytes{0};
    const auto first = location(f.source("one/tree"));
    luciole::transfer::Work work({}, bytes, [&](const auto& source, const auto&, const auto&, const auto&, bool) {
      return source.location == first ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
    });
    work.history = std::make_shared<luciole::HistoryRevisions>();
    std::vector<std::shared_ptr<const luciole::CopyEffect>> effects;
    std::size_t remaining = 16384;
    work.prepareCopy = [&](const auto&, const auto& target, int parent, int stage, bool replacing) {
      return luciole::CopyEffect::prepare(target, parent, stage, replacing, remaining, {}, work.history);
    };
    work.recordCopy = [&](const auto& proof, int stage) {
      effects.push_back(proof->published(stage, destination.get()));
    };
    for (const auto& source : {first, location(f.source("two/tree"))}) {
      const auto parent = *source.parent()->nativePath();
      auto fd = luciole::checkedFd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      luciole::transfer::Source input{
          0,
          source,
          {},
          *source.basename(),
          luciole::transfer::info(fd.get(), *source.basename()),
          parent,
          luciole::statFd(fd.get())
      };
      luciole::transfer::Node node(std::move(input), binding, luciole::TransferResult(source));
      luciole::transfer::execute(node, TransferAction::Copy, work, false);
      TEST_CHECK(node.result.outcome == TransferOutcome::Copied && !node.result.error);
    }
    const auto retained = retainedItem(f, old);
    luciole::CancelToken cancelled;
    cancelled.cancel();
    bool preflightRefused = false;
    try {
      (void)luciole::CopyReceipt::capture(effects, TransferAction::Copy, cancelled);
    } catch (const luciole::FileErrorException& error) {
      TEST_CHECK(error.error().kind == luciole::FileErrorKind::Cancelled);
      preflightRefused = true;
    }
    TEST_CHECK(preflightRefused && read(retained / "0") == "copy0");
    luciole::CancelToken token;
    std::atomic<bool> interrupted{false};
    std::jthread cancel([&](const std::stop_token& stop) {
      while (!stop.stop_requested()) {
        if (read(retained / "0") == "old0") {
          interrupted.store(true);
          token.cancel();
          return;
        }
        std::this_thread::yield();
      }
    });
    bool refused = false;
    std::shared_ptr<const luciole::CopyReceipt> receipt;
    try {
      receipt = luciole::CopyReceipt::capture(effects, TransferAction::Copy, token);
    } catch (const luciole::FileErrorException&) {
      refused = true;
    }
    cancel.request_stop();
    cancel.join();
    // Cancellation can arrive after capture completes. Neither scheduling order may lose data.
    TEST_CHECK(refused ? interrupted.load() && token.cancelled() : receipt && receipt->size() == 1);
    TEST_CHECK(read(f.destination("tree/final")) == "final result");
    for (int n = 0; n < 128; ++n) {
      unsigned count = 0;
      const auto value = "old" + std::to_string(n);
      for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
        if (entry.is_regular_file() && read(entry.path()) == value) {
          ++count;
        }
      }
      TEST_CHECK(count == 1 && read(f.source("one/tree/" + std::to_string(n))) == "copy" + std::to_string(n));
    }
    receipt.reset();
    effects.clear();
    drainHistoryCleanup();
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() == !refused);
  }

  void testMoveAncestorGenerations(Pump& pump) {
    for (const bool nested : {false, true}) {
      Fixture f;
      luciole::TransferService service;
      std::vector<luciole::Location> sources;
      if (nested) {
        fs::create_directories(f.destination("outer/tree/sub"));
        write(f.destination("outer/tree/sub/item"), "original");
        fs::create_directories(f.source("one/outer/tree/sub"));
        write(f.source("one/outer/tree/sub/item"), "one");
        fs::create_directories(f.source("two/outer"));
        write(f.source("two/outer/tree"), "two");
        fs::create_directories(f.source("three/outer"));
        write(f.source("three/outer/final"), "three");
        for (const char* name : {"one", "two", "three"}) {
          sources.push_back(location(f.source(std::string(name) + "/outer")));
        }
      } else {
        fs::create_directories(f.destination("tree/sub"));
        write(f.destination("tree/sub/item"), "original");
        for (const char* name : {"one", "two", "three", "four"}) {
          fs::create_directories(f.source(std::string(name) + "/tree/sub"));
          write(f.source(std::string(name) + "/tree/sub/item"), name);
          sources.push_back(location(f.source(std::string(name) + "/tree")));
        }
      }
      const auto originalFile = f.destination(nested ? "outer/tree/sub/item" : "tree/sub/item");
      fs::create_hard_link(originalFile, f.root / "alias");
      std::map<fs::path, struct stat> initial;
      for (const auto& entry : fs::recursive_directory_iterator(f.source(""))) {
        initial.emplace(entry.path(), luciole::transfer::info(AT_FDCWD, entry.path().native()));
      }
      pump.run(service, sources, f.destination(""), TransferAction::Move, [&](const auto& c) {
        return !c.canMerge || (nested ? c.source == sources.back() : c.source == sources[1] || c.source == sources[3])
            ? TransferConflictChoice::Replace
            : TransferConflictChoice::Merge;
      });
      if (!service.receipt()) {
        std::println(stderr, "Move generations nested={}: {}", nested, service.report());
      }
      TEST_CHECK(service.receipt());
      for (int cycle = 0; cycle < 3; ++cycle) {
        TEST_CHECK(read(f.destination(nested ? "outer/final" : "tree/sub/item")) == (nested ? "three" : "four"));
        reverseTransfer(pump, service, TransferAction::Move);
        if (!service.receipt()) {
          std::println(stderr, "Move generations Undo nested={}: {}", nested, service.report());
        }
        TEST_CHECK(service.receipt() && fs::equivalent(originalFile, f.root / "alias"));
        for (const auto& [path, stamp] : initial) {
          TEST_CHECK(luciole::sameIdentity(stamp, luciole::transfer::info(AT_FDCWD, path.native())));
        }
        reverseTransfer(pump, service, TransferAction::Move);
        if (!service.receipt()) {
          std::println(stderr, "Move generations Redo nested={}: {}", nested, service.report());
        }
        TEST_CHECK(service.receipt());
      }
      reverseTransfer(pump, service, TransferAction::Move);
      TEST_CHECK(service.receipt());
      dropTransferHistory(pump, service);
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    }
  }
  void testMoveAncestorGuards(Pump& pump) {
    for (int change = 0; change < 5; ++change) {
      Fixture f;
      {
        luciole::TransferService service;
        const auto sources = overlappingCopies(f);
        fs::create_directories(f.destination("tree/sub"));
        write(f.destination("tree/sub/item"), "original");
        fs::create_hard_link(f.destination("tree/sub/item"), f.root / "alias");
        pump.run(service, sources, f.destination(""), TransferAction::Move, [&](const auto& c) {
          return c.source == sources.back() || !c.canMerge ? TransferConflictChoice::Replace
                                                           : TransferConflictChoice::Merge;
        });
        TEST_CHECK(service.receipt());
        if (change == 0) {
          write(f.destination("tree/sub/item"), "edited final");
        } else if (change == 1) {
          fs::create_directory(f.source("two/tree"));
        } else if (change == 2) {
          write(f.root / "alias", "edited retained original");
        } else {
          reverseTransfer(pump, service, TransferAction::Move);
          TEST_CHECK(service.receipt());
          write(change == 3 ? f.destination("tree/later") : f.source("three/tree/sub/item"), "later edit");
        }
        const auto receipt = service.receipt();
        reverseTransfer(pump, service, TransferAction::Move);
        if (change == 1) {
          TEST_CHECK(!service.receipt() && !service.historyRetired());
          TEST_CHECK(!fs::exists(f.source("one/tree")) && !fs::exists(f.source("three/tree")));
          fs::remove(f.source("two/tree"));
          pump.replay(service, receipt);
          TEST_CHECK(service.receipt() && fs::equivalent(f.destination("tree/sub/item"), f.root / "alias"));
        } else {
          TEST_CHECK(!service.receipt() && service.historyRetired());
          if (change < 3) {
            TEST_CHECK(!fs::exists(f.source("one/tree")) && !fs::exists(f.source("two/tree")));
          } else {
            TEST_CHECK(read(f.source("one/tree/sub/item")) == "first");
          }
        }
      }
      drainHistoryCleanup();
      (void)luciole::CopyReceipt::takeCleanupErrors();
    }
  }
  void testMoveAncestorCancellation(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    const auto sources = overlappingCopies(f);
    fs::create_directories(f.destination("tree/sub"));
    write(f.destination("tree/sub/item"), "original");
    pump.run(service, sources, f.destination(""), TransferAction::Move, [&](const auto& c) {
      return c.source == sources.back() || !c.canMerge ? TransferConflictChoice::Replace
                                                       : TransferConflictChoice::Merge;
    });
    auto receipt = service.receipt();
    TEST_CHECK(receipt);
    luciole::CancelToken preflight;
    preflight.cancel();
    std::atomic<std::size_t> completed{0};
    auto cancelled = receipt->reverse(preflight, completed);
    TEST_CHECK(!cancelled.receipt && !cancelled.retireHistory && read(f.destination("tree/sub/item")) == "third");
    completed.store(0);
    luciole::CancelToken token;
    std::jthread interrupt([&](const std::stop_token& stop) {
      while (!stop.stop_requested() && completed.load() == 0) {
        std::this_thread::yield();
      }
      token.cancel();
    });
    auto result = receipt->reverse(token, completed);
    interrupt.request_stop();
    interrupt.join();
    for (const std::string value : {"original", "first", "second", "third"}) {
      unsigned count = 0;
      for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
        if (!entry.is_symlink() && entry.is_regular_file() && read(entry.path()) == value) {
          ++count;
        }
      }
      TEST_CHECK(count == 1);
    }
    const bool retired = result.retireHistory;
    result.receipt.reset();
    receipt.reset();
    dropTransferHistory(pump, service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() == !retired);
  }

  void testMoveAncestorReplacement(Pump& pump) {
    for (int original = 0; original < 4; ++original) {
      for (const bool finishUndone : {false, true}) {
        Fixture f;
        luciole::TransferService service;
        const auto sources = overlappingCopies(f);
        if (original == 1) {
          write(f.destination("tree"), "original");
        } else if (original >= 2) {
          fs::create_directories(f.destination("tree/sub"));
          write(f.destination("tree/sub/item"), "original");
          write(f.destination("tree/untouched"), "kept");
        }
        if (original) {
          fs::create_hard_link(
              original == 1 ? f.destination("tree") : f.destination("tree/sub/item"), f.root / "alias"
          );
        }
        std::map<fs::path, struct stat> initial;
        for (const auto& entry : fs::recursive_directory_iterator(f.source(""))) {
          initial.emplace(entry.path(), luciole::transfer::info(AT_FDCWD, entry.path().native()));
        }
        std::optional<struct stat> previous;
        if (original) {
          previous = luciole::transfer::info(AT_FDCWD, f.destination("tree").native());
        }
        pump.run(service, sources, f.destination(""), TransferAction::Move, [&](const auto& conflict) {
          return conflict.source == sources.back()
                  || !conflict.canMerge
                  || (original != 3 && conflict.source == sources.front())
              ? TransferConflictChoice::Replace
              : TransferConflictChoice::Merge;
        });
        if (!service.receipt()) {
          std::println(stderr, "Move ancestor case {}: {}", original, service.report());
        }
        TEST_CHECK(service.receipt());
        const auto final = luciole::transfer::info(AT_FDCWD, f.destination("tree").native());
        for (int cycle = 0; cycle < 3; ++cycle) {
          TEST_CHECK(read(f.destination("tree/sub/item")) == "third");
          for (const auto& source : sources) {
            TEST_CHECK(!fs::exists(*source.nativePath()));
          }
          reverseTransfer(pump, service, TransferAction::Move);
          if (!service.receipt()) {
            std::println(stderr, "Move ancestor Undo {}: {}", original, service.report());
          }
          TEST_CHECK(service.receipt());
          for (const auto& [path, stamp] : initial) {
            TEST_CHECK(luciole::sameIdentity(stamp, luciole::transfer::info(AT_FDCWD, path.native())));
          }
          TEST_CHECK(read(f.source("one/tree/sub/item")) == "first");
          TEST_CHECK(read(f.source("two/tree/sub/item")) == "second");
          TEST_CHECK(read(f.source("three/tree/sub/item")) == "third");
          if (previous) {
            TEST_CHECK(
                luciole::sameIdentity(*previous, luciole::transfer::info(AT_FDCWD, f.destination("tree").native()))
            );
            TEST_CHECK(
                fs::equivalent(original == 1 ? f.destination("tree") : f.destination("tree/sub/item"), f.root / "alias")
            );
            if (original >= 2) {
              TEST_CHECK(read(f.destination("tree/untouched")) == "kept");
            }
          } else {
            TEST_CHECK(!fs::exists(f.destination("tree")));
          }
          reverseTransfer(pump, service, TransferAction::Move);
          if (!service.receipt()) {
            std::println(stderr, "Move ancestor Redo {}: {}", original, service.report());
          }
          TEST_CHECK(
              service.receipt()
              && luciole::sameIdentity(final, luciole::transfer::info(AT_FDCWD, f.destination("tree").native()))
          );
        }
        if (finishUndone) {
          reverseTransfer(pump, service, TransferAction::Move);
          TEST_CHECK(service.receipt());
        }
        dropTransferHistory(pump, service);
        TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
        for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
          TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
        }
      }
    }
  }

  void testOverlappingMoves(Pump& pump) {
    for (int original = 0; original < 3; ++original) {
      for (const bool finishUndone : {false, true}) {
        Fixture f;
        luciole::TransferService service;
        const auto sources = overlappingCopies(f);
        std::map<fs::path, struct stat> originals;
        std::map<fs::path, std::string> contents;
        for (const auto& entry : fs::recursive_directory_iterator(f.source(""))) {
          originals.emplace(entry.path(), luciole::transfer::info(AT_FDCWD, entry.path().native()));
          if (entry.is_symlink()) {
            contents.emplace(entry.path(), fs::read_symlink(entry.path()).native());
          } else if (entry.is_regular_file()) {
            contents.emplace(entry.path(), read(entry.path()));
          }
        }
        if (original == 2) {
          fs::create_directories(f.destination("tree/old"));
          write(f.destination("tree/old/item"), "original");
        } else if (original == 1) {
          write(f.destination("tree"), "original");
        }
        if (original) {
          fs::create_hard_link(
              original == 2 ? f.destination("tree/old/item") : f.destination("tree"), f.root / "alias"
          );
        }
        const auto root = f.destination("tree");
        pump.run(service, sources, f.destination(""), TransferAction::Move, [&](const auto& c) {
          return c.canMerge && c.source != sources.front() ? TransferConflictChoice::Merge
                                                           : TransferConflictChoice::Replace;
        });
        if (!service.receipt()) {
          std::println(stderr, "{}", service.report());
        }
        TEST_CHECK(service.receipt());
        const auto rootInode = luciole::transfer::info(AT_FDCWD, root.native());
        for (int cycle = 0; cycle < 3; ++cycle) {
          TEST_CHECK(read(root / "sub/item") == "third" && read(root / "sub/keep") == "untouched");
          TEST_CHECK(
              read(root / "sub/becomes-file") == "new file" && read(root / "sub/becomes-folder/new") == "new folder"
          );
          TEST_CHECK(read(root / "sub/fresh/data") == "fresh data" && read(root / "sub/fresh/added") == "added data");
          TEST_CHECK(fs::read_symlink(root / "sub/link") == "missing-second");
          for (const auto& source : sources) {
            TEST_CHECK(!fs::exists(*source.nativePath()));
          }
          for (const auto& entry : fs::recursive_directory_iterator(root)) {
            TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
          }
          reverseTransfer(pump, service, TransferAction::Move);
          TEST_CHECK(service.receipt());
          for (const auto& [path, value] : originals) {
            const auto now = luciole::transfer::info(AT_FDCWD, path.native());
            TEST_CHECK(luciole::sameIdentity(value, now) && value.st_mode == now.st_mode);
            if (S_ISREG(value.st_mode)) {
              TEST_CHECK(read(path) == contents.at(path));
            }
            if (S_ISLNK(value.st_mode)) {
              TEST_CHECK(fs::read_symlink(path).native() == contents.at(path));
            }
          }
          if (original) {
            TEST_CHECK(fs::equivalent(original == 2 ? root / "old/item" : root, f.root / "alias"));
          } else {
            TEST_CHECK(!fs::exists(root));
          }
          write(f.destination("later-sibling"), "user data");
          reverseTransfer(pump, service, TransferAction::Move);
          TEST_CHECK(
              service.receipt() && luciole::sameIdentity(rootInode, luciole::transfer::info(AT_FDCWD, root.native()))
          );
          TEST_CHECK(read(f.destination("later-sibling")) == "user data");
        }
        if (finishUndone) {
          reverseTransfer(pump, service, TransferAction::Move);
          TEST_CHECK(service.receipt());
        }
        dropTransferHistory(pump, service);
        TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
        for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
          TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-create-"));
        }
      }
    }
  }
  void testOverlappingMoveGuards(Pump& pump) {
    for (int change = 0; change < 6; ++change) {
      Fixture f;
      {
        luciole::TransferService service;
        const auto sources = overlappingCopies(f);
        pump.run(service, sources, f.destination(""), TransferAction::Move, [](const auto& c) {
          return c.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
        });
        TEST_CHECK(service.receipt());
        const auto root = f.destination("tree");
        if (change == 0) {
          write(root / "sub/keep", "user edit");
        } else if (change == 1) {
          write(root / "sub/extra", "user addition");
        } else if (change == 2) {
          TEST_CHECK(::chmod((root / "sub").c_str(), 0700) == 0);
        } else if (change == 3) {
          fs::create_directory(f.source("one/tree"));
          auto retry = service.receipt();
          pump.replay(service, retry);
          TEST_CHECK(!service.historyRetired() && read(root / "sub/item") == "third");
          TEST_CHECK(!fs::exists(f.source("two/tree")) && !fs::exists(f.source("three/tree")));
          TEST_CHECK(fs::remove(f.source("one/tree")));
          pump.replay(service, retry);
          retry.reset();
          TEST_CHECK(service.receipt() && read(f.source("one/tree/sub/item")) == "first");
          reverseTransfer(pump, service, TransferAction::Move);
          TEST_CHECK(service.receipt());
          dropTransferHistory(pump, service);
          TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
          continue;
        } else {
          reverseTransfer(pump, service, TransferAction::Move);
          TEST_CHECK(service.receipt());
          write(f.source(change == 4 ? "one/tree/sub/keep" : "two/tree/sub/fresh/extra"), "source edit");
        }
        pump.replay(service, service.receipt());
        TEST_CHECK(!service.receipt() && service.historyRetired());
        TEST_CHECK(change < 4 ? read(root / "sub/item") == "third" : !fs::exists(root));
        if (change < 4) {
          TEST_CHECK(!fs::exists(f.source("one/tree")) && !fs::exists(f.source("two/tree")));
        } else {
          TEST_CHECK(read(f.source("one/tree/sub/item")) == "first" && read(f.source("two/tree/sub/item")) == "second");
        }
      }
      drainHistoryCleanup();
      (void)luciole::CopyReceipt::takeCleanupErrors();
    }
  }
  void testOverlappingMoveCancellation(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    const auto sources = overlappingCopies(f);
    pump.run(service, sources, f.destination(""), TransferAction::Move, [](const auto& c) {
      return c.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
    });
    auto receipt = service.receipt();
    TEST_CHECK(receipt);
    luciole::CancelToken token;
    std::atomic<std::size_t> completed{0};
    std::jthread interrupt([&](const std::stop_token& stop) {
      while (!stop.stop_requested() && completed.load() == 0) {
        std::this_thread::yield();
      }
      token.cancel();
    });
    auto result = receipt->reverse(token, completed);
    interrupt.request_stop();
    interrupt.join();
    for (const std::string value : {"first", "second", "third"}) {
      unsigned count = 0;
      for (const auto& entry : fs::recursive_directory_iterator(f.root)) {
        if (!entry.is_symlink() && entry.is_regular_file() && read(entry.path()) == value) {
          ++count;
        }
      }
      TEST_CHECK(count == 1);
    }
    const bool retired = result.retireHistory;
    result.receipt.reset();
    receipt.reset();
    dropTransferHistory(pump, service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() == !retired);
  }

  void testOverlappingMoveProofBudget(Pump& pump) {
    Fixture f;
    {
      luciole::TransferService service;
      fs::create_directories(f.source("one/tree"));
      fs::create_directories(f.source("two/tree/fresh"));
      for (int n = 0; n < 8192; ++n) {
        write(f.source("two/tree/fresh/" + std::to_string(n)), std::to_string(n));
      }
      pump.run(
          service, {location(f.source("one/tree")), location(f.source("two/tree"))}, f.destination(""),
          TransferAction::Move, [](const auto&) { return TransferConflictChoice::Merge; }
      );
      TEST_CHECK(!service.receipt());
      TEST_CHECK(!fs::exists(f.source("one/tree")) && !fs::exists(f.source("two/tree")));
      TEST_CHECK(read(f.destination("tree/fresh/0")) == "0" && read(f.destination("tree/fresh/8191")) == "8191");
      TEST_CHECK(service.report().contains(".luciole-create-"));
    }
    drainHistoryCleanup();
    TEST_CHECK(!luciole::CopyReceipt::takeCleanupErrors().empty());
  }

  void testOverlappingEmptyMoves(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    fs::create_directories(f.source("one/tree"));
    fs::create_directories(f.source("two/tree"));
    const auto first = luciole::transfer::info(AT_FDCWD, f.source("one/tree").native());
    const auto second = luciole::transfer::info(AT_FDCWD, f.source("two/tree").native());
    pump.run(
        service, {location(f.source("one/tree")), location(f.source("two/tree"))}, f.destination(""),
        TransferAction::Move, [](const auto&) { return TransferConflictChoice::Merge; }
    );
    TEST_CHECK(service.receipt() && fs::is_empty(f.destination("tree")));
    for (int cycle = 0; cycle < 3; ++cycle) {
      reverseTransfer(pump, service, TransferAction::Move);
      TEST_CHECK(service.receipt() && !fs::exists(f.destination("tree")));
      TEST_CHECK(luciole::sameIdentity(first, luciole::transfer::info(AT_FDCWD, f.source("one/tree").native())));
      TEST_CHECK(luciole::sameIdentity(second, luciole::transfer::info(AT_FDCWD, f.source("two/tree").native())));
      reverseTransfer(pump, service, TransferAction::Move);
      TEST_CHECK(service.receipt() && fs::is_empty(f.destination("tree")));
      TEST_CHECK(luciole::sameIdentity(first, luciole::transfer::info(AT_FDCWD, f.destination("tree").native())));
    }
    dropTransferHistory(pump, service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
  }

  void testRejectedSnapshotThenRepeatedCopies(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    fs::create_directory(f.destination("item"));
    for (int n = 0; n < 16384; ++n) {
      write(f.destination("item/" + std::to_string(n)), "old");
    }
    std::vector<luciole::Location> sources;
    sources.reserve(128);
    for (int n = 0; n < 128; ++n) {
      fs::create_directory(f.source(std::to_string(n)));
      write(f.source(std::to_string(n) + "/item"), "version-" + std::to_string(n));
      sources.push_back(location(f.source(std::to_string(n) + "/item")));
    }
    pump.run(service, sources, f.destination(""), TransferAction::Copy, [](const auto&) {
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(!service.copyReceipt() && read(f.destination("item")) == "version-127");
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == TransferOutcome::Copied && !result.error;
    }));
    dropTransferHistory(pump, service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    for (const auto& path : fs::recursive_directory_iterator(f.root)) {
      TEST_CHECK(!path.path().filename().native().starts_with(".luciole-create-"));
    }
    f.clean();
  }
  void testMoveHistory(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    const std::vector<std::string> names{"file 文.txt", "folder", "broken", raw};
    write(f.source(names[0]), "original");
    fs::create_directories(f.source("folder/sub"));
    write(f.source("folder/sub/inside"), "nested");
    fs::create_symlink("absent", f.source("broken"));
    write(f.source(raw), "raw");
    std::vector<luciole::Location> sources;
    sources.reserve(names.size());
    for (const auto& name : names) {
      sources.push_back(location(f.source(name)));
    }
    pump.run(service, sources, f.root / "destination", TransferAction::Move);
    TEST_CHECK(service.receipt() && !service.historyRetired());
    write(f.destination("folder/sub/inside"), "edited nested");
    for (int cycle = 0; cycle < 3; ++cycle) {
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && !service.historyRetired() && service.completed() == names.size());
      TEST_CHECK(read(f.source(names[0])) == "original" && read(f.source(raw)) == "raw");
      TEST_CHECK(
          read(f.source("folder/sub/inside")) == "edited nested" && fs::read_symlink(f.source("broken")) == "absent"
      );
      TEST_CHECK(fs::is_empty(f.root / "destination"));
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && !service.historyRetired());
      TEST_CHECK(read(f.destination(names[0])) == "original" && read(f.destination(raw)) == "raw");
      TEST_CHECK(read(f.destination("folder/sub/inside")) == "edited nested");
      TEST_CHECK(fs::read_symlink(f.destination("broken")) == "absent" && fs::is_empty(f.root / "source"));
    }
    TEST_CHECK(!service.retryFailed() && !service.replay({}));
    f.clean();
  }

  void testMoveHistoryGuards(Pump& pump) {
    for (int scenario = 0; scenario < 5; ++scenario) {
      Fixture f;
      luciole::TransferService service;
      write(f.source("a"), "A");
      write(f.source("b"), "B");
      pump.run(
          service, {location(f.source("a")), location(f.source("b"))}, f.root / "destination", TransferAction::Move
      );
      const auto receipt = service.receipt();
      TEST_CHECK(receipt);
      if (scenario == 0) {
        fs::create_symlink("missing", f.source("b"));
      } else if (scenario == 1) {
        struct stat stamp{};
        TEST_CHECK(::stat(f.destination("b").c_str(), &stamp) == 0);
        write(f.destination("b"), "X");
        const struct timespec times[]{stamp.st_atim, stamp.st_mtim};
        TEST_CHECK(::utimensat(AT_FDCWD, f.destination("b").c_str(), times, 0) == 0);
      } else if (scenario == 2) {
        fs::rename(f.destination("b"), f.destination("held"));
        write(f.destination("b"), "foreign");
      } else if (scenario == 3) {
        fs::rename(f.root / "source", f.root / "held-source");
        fs::create_directory(f.root / "source");
      } else {
        fs::rename(f.root / "destination", f.root / "held-destination");
        fs::create_directory(f.root / "destination");
      }
      pump.replay(service, receipt);
      TEST_CHECK(!service.receipt() && service.results().size() == 2);
      TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
        return result.outcome == TransferOutcome::Failed && !result.retryable;
      }));
      TEST_CHECK(service.historyRetired() == (scenario != 0));
      TEST_CHECK(!fs::exists(f.source("a")));
      TEST_CHECK(read((scenario == 4 ? f.root / "held-destination/a" : f.destination("a"))) == "A");
      if (scenario == 0) {
        TEST_CHECK(fs::read_symlink(f.source("b")) == "missing");
        TEST_CHECK(fs::remove(f.source("b")));
        pump.replay(service, receipt);
        TEST_CHECK(service.receipt() && read(f.source("a")) == "A" && read(f.source("b")) == "B");
      } else if (scenario == 1) {
        TEST_CHECK(read(f.destination("b")) == "X");
      } else if (scenario == 2) {
        TEST_CHECK(read(f.destination("b")) == "foreign" && read(f.destination("held")) == "B");
      }
    }
  }

  void testMoveHistoryAliasesAndNames(Pump& pump) {
    Fixture f;
    write(f.source("a"), "alias");
    fs::create_hard_link(f.source("a"), f.source("b"));
    write(f.destination("a"), "existing");
    luciole::TransferService service;
    pump.run(
        service, {location(f.source("a")), location(f.source("b"))}, f.root / "destination", TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(service.receipt() && fs::equivalent(f.destination("a (1)"), f.destination("b")));
    for (int cycle = 0; cycle < 3; ++cycle) {
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && fs::equivalent(f.source("a"), f.source("b")));
      TEST_CHECK(read(f.destination("a")) == "existing");
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && fs::equivalent(f.destination("a (1)"), f.destination("b")));
    }
    pump.replay(service, service.receipt());
    pump.run(
        service, {location(f.source("a")), location(f.source("a"))}, f.root / "destination", TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(service.receipt() && service.results()[1].outcome == TransferOutcome::Skipped);
    pump.replay(service, service.receipt());
    TEST_CHECK(service.receipt() && read(f.source("a")) == "alias");
  }

  void testMoveHistoryInterruption(Pump& pump) {
    for (const bool collision : {false, true}) {
      Fixture f;
      luciole::TransferService service;
      constexpr std::size_t count = 256;
      std::vector<luciole::Location> sources;
      for (std::size_t i = 0; i < count; ++i) {
        write(f.source(std::to_string(i)), std::to_string(i));
        sources.push_back(location(f.source(std::to_string(i))));
      }
      pump.run(service, sources, f.destination(""), TransferAction::Move);
      const auto receipt = service.receipt();
      TEST_CHECK(receipt);
      luciole::CancelToken token;
      token.cancel();
      std::atomic<std::size_t> completed{0};
      const auto cancelled = receipt->reverse(token, completed);
      TEST_CHECK(!cancelled.receipt && !cancelled.retireHistory && completed.load() == count);
      TEST_CHECK(std::ranges::all_of(cancelled.results, [](const auto& result) {
        return result.outcome == TransferOutcome::Cancelled;
      }));
      token = luciole::CancelToken{};
      completed.store(0);
      std::jthread interrupt([&](const std::stop_token& stop) {
        while (!stop.stop_requested() && completed.load() == 0) {
          std::this_thread::yield();
        }
        if (!stop.stop_requested() && completed.load() < count) {
          if (collision) {
            const int occupied =
                ::open(f.source(std::to_string(count - 1)).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (occupied >= 0) {
              TEST_CHECK(::write(occupied, "occupied", 8) == 8);
              TEST_CHECK(::close(occupied) == 0);
            } else {
              TEST_CHECK(errno == EEXIST);
            }
          } else {
            token.cancel();
          }
        }
      });
      const auto result = receipt->reverse(token, completed);
      interrupt.request_stop();
      interrupt.join();
      TEST_CHECK(completed.load() == count && result.results.size() == count);
      std::size_t moved = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const auto& item = result.results[i];
        const auto name = std::to_string(i);
        if (item.outcome == TransferOutcome::Moved) {
          ++moved;
          TEST_CHECK(read(f.source(name)) == name && !fs::exists(f.destination(name)));
        } else {
          TEST_CHECK(read(f.destination(name)) == name);
          TEST_CHECK(
              !fs::exists(f.source(name)) || (collision && i == count - 1 && read(f.source(name)) == "occupied")
          );
          TEST_CHECK(item.error);
        }
      }
      TEST_CHECK(moved > 0);
      TEST_CHECK(moved == count ? result.receipt != nullptr : result.retireHistory && !result.receipt);
      f.clean();
    }
  }

  void testTransferHistoryBounds(Pump& pump) {
    Fixture f;
    std::vector<luciole::Location> sources;
    for (int i = 0; i < 100; ++i) {
      const auto name = std::to_string(i);
      write(f.source(name), name);
      sources.push_back(location(f.source(name)));
    }
    struct LimitGuard {
      struct rlimit previous{};
      LimitGuard() {
        TEST_CHECK(::getrlimit(RLIMIT_NOFILE, &previous) == 0);
        const struct rlimit limited{std::min<rlim_t>(previous.rlim_cur, 192), previous.rlim_max};
        TEST_CHECK(::setrlimit(RLIMIT_NOFILE, &limited) == 0);
      }
      ~LimitGuard() { TEST_CHECK(::setrlimit(RLIMIT_NOFILE, &previous) == 0); }
    } guard;
    luciole::TransferService service;
    pump.run(service, sources, f.root / "destination", TransferAction::Copy);
    TEST_CHECK(!service.copyReceipt());
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == TransferOutcome::Copied;
    }));
    for (int i = 0; i < 100; ++i) {
      TEST_CHECK(read(f.destination(std::to_string(i))) == std::to_string(i));
      TEST_CHECK(fs::remove(f.destination(std::to_string(i))));
    }
    pump.run(service, sources, f.root / "destination", TransferAction::Move);
    TEST_CHECK(!service.receipt());
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == TransferOutcome::Moved;
    }));
    for (int i = 0; i < 100; ++i) {
      TEST_CHECK(read(f.destination(std::to_string(i))) == std::to_string(i));
    }
  }

  void testMoveAndCopy(Pump& pump) {
    Fixture fixture;
    luciole::TransferService service;
    write(fixture.source("file.txt"), "payload");
    fs::create_directories(fixture.source("folder/sub"));
    write(fixture.source("folder/sub/.hidden"), "nested");
    const bool attributes =
        ::setxattr(fixture.source("folder/sub/.hidden").c_str(), "user.luciole-test", "metadata", 8, 0) == 0;
    fs::create_symlink("../../outside", fixture.source("folder/link"));
    write(fixture.root / "outside", "outside must survive");
    pump.run(
        service, {location(fixture.source("file.txt")), location(fixture.source("folder"))}, fixture.destination(""),
        TransferAction::Move
    );
    TEST_CHECK(service.results().size() == 2 && std::ranges::all_of(service.results(), [](const auto& result) {
                 return result.outcome == TransferOutcome::Moved && !result.error;
               }));
    TEST_CHECK(
        !fs::exists(fixture.source("file.txt"))
        && read(fixture.destination("file.txt")) == "payload"
        && read(fixture.destination("folder/sub/.hidden")) == "nested"
    );
    TEST_CHECK(
        fs::is_symlink(fixture.destination("folder/link")) && read(fixture.root / "outside") == "outside must survive"
    );
    pump.run(service, {location(fixture.destination("folder"))}, fixture.source(""), TransferAction::Copy);
    TEST_CHECK(!service.receipt());
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.source("folder/sub/.hidden")) == "nested"
        && fs::read_symlink(fixture.source("folder/link")) == "../../outside"
    );
    if (attributes) {
      char value[16]{};
      TEST_CHECK(
          ::getxattr(fixture.source("folder/sub/.hidden").c_str(), "user.luciole-test", value, sizeof(value)) == 8
          && std::string(value, 8) == "metadata"
      );
    }
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.source(raw), "raw filename");
    pump.run(service, {location(fixture.source(raw))}, fixture.destination(""), TransferAction::Copy);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied && read(fixture.destination(raw)) == "raw filename"
    );
    fixture.clean();
  }
  void testConflicts(Pump& pump) {
    Fixture fixture;
    luciole::TransferService service;
    write(fixture.source("image.png"), "incoming");
    write(fixture.destination("image.png"), "existing");
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Skip; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Skipped && read(fixture.destination("image.png")) == "existing"
    );
    write(fixture.destination("image (1).png"), "occupied");
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.destination("image (2).png")) == "incoming"
        && read(fixture.destination("image.png")) == "existing"
    );
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Moved
        && !fs::exists(fixture.source("image.png"))
        && read(fixture.destination("image.png")) == "incoming"
    );
    TEST_CHECK(service.receipt());
    fs::create_directories(fixture.source("folder"));
    fs::create_directories(fixture.destination("folder"));
    write(fixture.source("folder/new"), "new");
    write(fixture.destination("folder/old"), "old");
    pump.run(
        service, {location(fixture.source("folder"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.destination("folder/new")) == "new"
        && !fs::exists(fixture.destination("folder/old"))
        && fs::exists(fixture.source("folder/new"))
    );
    fs::create_symlink(fixture.root / "protected", fixture.destination("link"));
    write(fixture.root / "protected", "protected");
    write(fixture.source("link"), "regular incoming");
    pump.run(
        service, {location(fixture.source("link"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && !fs::is_symlink(fixture.destination("link"))
        && read(fixture.root / "protected") == "protected"
    );
    fixture.clean();
  }
  void testGuardsAndRaces(Pump& pump) {
    Fixture fixture;
    luciole::TransferService service;
    fs::create_directories(fixture.source("tree/child"));
    write(fixture.source("tree/file"), "tree");
    fs::create_directory_symlink(fixture.source("tree/child"), fixture.root / "alias");
    pump.run(service, {location(fixture.source("tree"))}, fixture.root / "alias", TransferAction::Copy);
    TEST_CHECK(service.results()[0].outcome == TransferOutcome::Failed && fs::is_empty(fixture.source("tree/child")));
    pump.run(
        service, {location(fixture.source("tree/file")), location(fixture.source("tree"))}, fixture.destination(""),
        TransferAction::Move
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Skipped
        && service.results()[1].outcome == TransferOutcome::Moved
        && read(fixture.destination("tree/file")) == "tree"
    );
    write(fixture.source("race.txt"), "incoming");
    write(fixture.destination("race.txt"), "initial destination");
    int decisions = 0;
    pump.run(
        service, {location(fixture.source("race.txt"))}, fixture.destination(""), TransferAction::Copy,
        [&](const auto&) {
          if (++decisions == 1) {
            write(fixture.root / "replacement", "changed destination");
            fs::rename(fixture.root / "replacement", fixture.destination("race.txt"));
            return TransferConflictChoice::Replace;
          }
          return TransferConflictChoice::Skip;
        }
    );
    TEST_CHECK(
        decisions == 2
        && service.results()[0].outcome == TransferOutcome::Skipped
        && read(fixture.destination("race.txt")) == "changed destination"
    );
    pump.run(
        service, {location(fixture.source("race.txt"))}, fixture.destination(""), TransferAction::Move,
        [&](const auto&) {
          write(fixture.root / "new source", "replaced source");
          fs::rename(fixture.root / "new source", fixture.source("race.txt"));
          return TransferConflictChoice::KeepBoth;
        }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Failed
        && read(fixture.source("race.txt")) == "replaced source"
        && !fs::exists(fixture.destination("race (1).txt"))
    );
    TEST_CHECK(::mkfifo(fixture.source("pipe").c_str(), 0600) == 0);
    pump.run(
        service, {location(fixture.source("pipe")), location(fixture.source("race.txt"))}, fixture.destination(""),
        TransferAction::Copy, [](const auto&) { return TransferConflictChoice::Skip; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Failed
        && service.results()[1].outcome == TransferOutcome::Skipped
    );
    fixture.clean();
  }
  void testCancellationAndNumbering(Pump& pump) {
    Fixture fixture;
    luciole::TransferService service;
    write(fixture.source("same.txt"), "same");
    pump.run(service, {location(fixture.source("same.txt"))}, fixture.source(""), TransferAction::Copy);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied && read(fixture.source("same (1).txt")) == "same"
    );
    const std::string longName(255, 'a');
    write(fixture.source(longName), "long");
    pump.run(service, {location(fixture.source(longName))}, fixture.source(""), TransferAction::Copy);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.source(std::string(251, 'a') + " (1)")) == "long"
    );
    write(fixture.destination("same.txt"), "existing");
    pump.run(
        service, {location(fixture.source("same.txt")), location(fixture.source("same (1).txt"))},
        fixture.destination(""), TransferAction::Move, [](const auto&) { return TransferConflictChoice::Cancel; }
    );
    TEST_CHECK(
        std::ranges::all_of(
            service.results(), [](const auto& item) { return item.outcome == TransferOutcome::Cancelled; }
        )
        && read(fixture.destination("same.txt")) == "existing"
    );
    write(fixture.source("large"), "");
    fs::resize_file(fixture.source("large"), 1024ULL * 1024 * 1024);
    TEST_CHECK(
        service.start({{location(fixture.source("large"))}, location(fixture.destination("")), TransferAction::Copy})
    );
    TEST_CHECK(!service.start({{location(fixture.source("same.txt"))}, location(fixture.destination(""))}));
    pump.until([&] {
      if (service.bytes() != 0) {
        service.cancel();
      }
      return !service.busy();
    });
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Cancelled
        && fs::exists(fixture.source("large"))
        && !fs::exists(fixture.destination("large"))
    );
    fixture.clean();
  }
  void noPrivateMoves(const Fixture& fixture) {
    drainHistoryCleanup();
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    for (const auto& item : fs::recursive_directory_iterator(fixture.root)) {
      TEST_CHECK(!item.path().filename().native().starts_with(".luciole-"));
    }
  }
  bool differentDevices(const Fixture& source, const Fixture& target) {
    struct stat from{}, to{};
    TEST_CHECK(::stat(source.root.c_str(), &from) == 0 && ::stat(target.root.c_str(), &to) == 0);
    return from.st_dev != to.st_dev;
  }
  void testCopiedMoveHistory(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      std::println("Copied Move history skipped: /dev/shm unavailable");
      return;
    }
    Fixture source;
    Fixture target("/dev/shm");
    if (!differentDevices(source, target)) {
      std::println("Copied Move history skipped: fixtures share a device");
      return;
    }
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    fs::create_directories(source.source("tree/nested/empty"));
    write(source.source("tree/nested/文.txt"), "nested data");
    fs::create_hard_link(source.source("tree/nested/文.txt"), source.source("tree/nested/alias"));
    fs::create_symlink("missing", source.source("tree/dangling"));
    write(source.source(raw), "raw data");
    write(source.source("plain"), "aliased source");
    fs::create_hard_link(source.source("plain"), source.source("linked"));
    struct stat original{}, originalFile{}, plain{};
    TEST_CHECK(::stat(source.source("tree").c_str(), &original) == 0);
    TEST_CHECK(::stat(source.source("tree/nested/文.txt").c_str(), &originalFile) == 0);
    TEST_CHECK(::stat(source.source("plain").c_str(), &plain) == 0);
    luciole::TransferService service;
    pump.run(
        service,
        {location(source.source("tree")), location(source.source(raw)), location(source.source("plain")),
         location(source.source("linked"))},
        target.destination(""), TransferAction::Move
    );
    auto receipt = service.takeReceipt();
    for (const auto& outcome : service.results()) {
      if (outcome.error) {
        std::println(stderr, "Cross-filesystem Move: {}", outcome.error->message);
      }
    }
    TEST_CHECK(receipt && receipt->size() == 4 && !fs::exists(source.source("tree")));
    noPrivateMoves(source);
    // Open payload descriptors would keep deleted source data allocated after a Move.
    for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
      struct stat held{};
      if (::stat(entry.path().c_str(), &held) == 0) {
        TEST_CHECK(!luciole::sameIdentity(plain, held));
      }
    }
    struct stat copied{};
    TEST_CHECK(::stat(target.destination("tree").c_str(), &copied) == 0);
    write(target.destination("later"), "keep destination sibling");
    write(source.source("later"), "keep source sibling");
    for (int repeat = 0; repeat < 3; ++repeat) {
      pump.replay(service, receipt);
      auto redo = service.takeReceipt();
      TEST_CHECK(redo && !service.historyRetired());
      TEST_CHECK(read(source.source("tree/nested/文.txt")) == "nested data" && !fs::exists(target.destination("tree")));
      struct stat actual{}, alias{};
      TEST_CHECK(::stat(source.source("tree").c_str(), &actual) == 0 && S_ISDIR(actual.st_mode));
      TEST_CHECK(
          ::stat(source.source("tree/nested/alias").c_str(), &alias) == 0 && alias.st_size == originalFile.st_size
      );
      TEST_CHECK(fs::read_symlink(source.source("tree/dangling")) == "missing");
      TEST_CHECK(read(source.source(raw)) == "raw data");
      pump.replay(service, receipt);
      TEST_CHECK(!service.receipt() && service.historyRetired());
      pump.replay(service, redo);
      receipt = service.takeReceipt();
      TEST_CHECK(receipt && !fs::exists(source.source("tree")));
      TEST_CHECK(::stat(target.destination("tree").c_str(), &actual) == 0 && luciole::sameIdentity(copied, actual));
      TEST_CHECK(read(target.destination("linked")) == "aliased source");
    }
    TEST_CHECK(
        read(target.destination("later")) == "keep destination sibling"
        && read(source.source("later")) == "keep source sibling"
    );
    receipt.reset();
    noPrivateMoves(source);
    noPrivateMoves(target);
  }
  void testCopiedMoveReplacements(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (unsigned shape = 0; shape < 4; ++shape) {
      Fixture source;
      Fixture target("/dev/shm");
      if (!differentDevices(source, target)) {
        return;
      }
      if (shape & 1U) {
        fs::create_directory(source.source("item"));
        write(source.source("item/new"), "incoming");
      } else {
        write(source.source("item"), "incoming");
      }
      if (shape & 2U) {
        fs::create_directory(target.destination("item"));
        write(target.destination("item/old"), "previous");
      } else {
        write(target.destination("item"), "previous");
      }
      struct stat original{}, previous{};
      TEST_CHECK(
          ::stat(source.source("item").c_str(), &original) == 0
          && ::stat(target.destination("item").c_str(), &previous) == 0
      );
      luciole::TransferService service;
      pump.run(
          service, {location(source.source("item"))}, target.destination(""), TransferAction::Move,
          [](const auto&) { return TransferConflictChoice::Replace; }
      );
      auto receipt = service.takeReceipt();
      TEST_CHECK(receipt);
      pump.replay(service, receipt);
      receipt = service.takeReceipt();
      TEST_CHECK(receipt && read((shape & 1U) ? source.source("item/new") : source.source("item")) == "incoming");
      TEST_CHECK(read((shape & 2U) ? target.destination("item/old") : target.destination("item")) == "previous");
      struct stat actual{};
      TEST_CHECK(
          ::stat(source.source("item").c_str(), &actual) == 0
          && (actual.st_mode & S_IFMT) == (original.st_mode & S_IFMT)
      );
      TEST_CHECK(::stat(target.destination("item").c_str(), &actual) == 0 && luciole::sameIdentity(previous, actual));
      if (shape & 1U) {
        pump.replay(service, receipt);
        receipt = service.takeReceipt();
        TEST_CHECK(receipt && !fs::exists(source.source("item")));
        TEST_CHECK(read(target.destination("item/new")) == "incoming");
      }
      receipt.reset();
      noPrivateMoves(source);
      noPrivateMoves(target);
    }
  }
  void testCopiedMoveGuards(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (unsigned changed = 0; changed < 4; ++changed) {
      Fixture source;
      Fixture target("/dev/shm");
      if (!differentDevices(source, target)) {
        return;
      }
      write(source.source("a"), "source a");
      write(source.source("b"), "source b");
      write(target.destination("a"), "previous");
      {
        luciole::TransferService service;
        pump.run(
            service, {location(source.source("a")), location(source.source("b"))}, target.destination(""),
            TransferAction::Move, [](const auto&) { return TransferConflictChoice::Replace; }
        );
        auto receipt = service.takeReceipt();
        TEST_CHECK(receipt);
        if (changed == 0) {
          write(source.source("b"), "occupied");
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && !service.historyRetired());
          TEST_CHECK(
              !fs::exists(source.source("a"))
              && read(source.source("b")) == "occupied"
              && read(target.destination("a")) == "source a"
          );
          fs::remove(source.source("b"));
          pump.replay(service, receipt);
          auto redo = service.takeReceipt();
          TEST_CHECK(redo && read(source.source("a")) == "source a");
          luciole::CancelToken token;
          token.cancel();
          std::atomic<std::size_t> completed{0};
          const auto cancelled = redo->reverse(token, completed);
          TEST_CHECK(!cancelled.receipt && !cancelled.retireHistory && completed.load() == 2);
          TEST_CHECK(read(source.source("b")) == "source b");
        } else if (changed == 1) {
          write(target.destination("b"), "user changed target");
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && service.historyRetired());
          TEST_CHECK(!fs::exists(source.source("a")) && read(target.destination("b")) == "user changed target");
        } else if (changed == 2) {
          fs::rename(source.source(""), source.root / "held-source");
          fs::create_directory(source.source(""));
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && service.historyRetired());
          TEST_CHECK(fs::is_empty(source.source("")) && read(target.destination("a")) == "source a");
        } else {
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          TEST_CHECK(receipt);
          write(source.source("b"), "edited after undo");
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && service.historyRetired());
          TEST_CHECK(
              read(source.source("a")) == "source a"
              && read(source.source("b")) == "edited after undo"
              && read(target.destination("a")) == "previous"
          );
        }
      }
      drainHistoryCleanup();
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == (changed == 0));
    }
  }
  void testCopiedMovePartialAndNames(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    Fixture source;
    Fixture target("/dev/shm");
    if (!differentDevices(source, target)) {
      return;
    }
    write(source.source("a"), "first data");
    write(source.source("b"), "second data");
    write(target.destination("b"), "previous");
    {
      luciole::TransferService service;
      pump.run(
          service, {location(source.source("a")), location(source.source("b"))}, target.destination(""),
          TransferAction::Move, [&](const auto&) {
            write(source.source("a"), "edited before retirement");
            return TransferConflictChoice::Replace;
          }
      );
      TEST_CHECK(
          !service.receipt()
          && service.results()[0].outcome == TransferOutcome::Partial
          && service.results()[1].outcome == TransferOutcome::Moved
      );
      TEST_CHECK(
          read(source.source("a")) == "edited before retirement" && read(target.destination("a")) == "first data"
      );
      TEST_CHECK(!fs::exists(source.source("b")) && read(target.destination("b")) == "second data");
      TEST_CHECK(service.results()[1].error && service.results()[1].error->message.contains("Inspect"));
    }
    drainHistoryCleanup();
    TEST_CHECK(!luciole::CopyReceipt::takeCleanupErrors().empty());
    write(source.source("named.txt"), "numbered");
    write(source.source("skip"), "source skip");
    write(target.destination("named.txt"), "leave original");
    write(target.destination("skip"), "target skip");
    {
      luciole::TransferService service;
      pump.run(
          service, {location(source.source("named.txt")), location(source.source("skip"))}, target.destination(""),
          TransferAction::Move, [](const auto& conflict) {
            return conflict.name == "skip" ? TransferConflictChoice::Skip : TransferConflictChoice::KeepBoth;
          }
      );
      auto receipt = service.takeReceipt();
      TEST_CHECK(receipt && receipt->size() == 1 && read(target.destination("named (1).txt")) == "numbered");
      pump.replay(service, receipt);
      TEST_CHECK(
          service.receipt()
          && read(source.source("named.txt")) == "numbered"
          && !fs::exists(target.destination("named (1).txt"))
      );
      TEST_CHECK(
          read(target.destination("named.txt")) == "leave original"
          && read(source.source("skip")) == "source skip"
          && read(target.destination("skip")) == "target skip"
      );
    }
    drainHistoryCleanup();
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
  }

  void testMixedMoveMerges(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (bool mixed : {false, true}) {
      for (bool discardUndone : {false, true}) {
        Fixture source;
        Fixture target("/dev/shm");
        if (!differentDevices(source, target)) {
          return;
        }
        fs::create_directories(source.source("tree/sub/empty"));
        fs::create_directories(target.destination("tree/sub/empty"));
        const auto raw = std::string("raw-") + static_cast<char>(0xff);
        write(source.source("tree/sub/value"), "incoming");
        write(source.source("tree/sub/文.txt"), "Unicode");
        write(source.source("tree/sub/" + raw), "raw");
        fs::create_symlink("missing", source.source("tree/sub/broken"));
        write(target.destination("tree/sub/value"), "previous");
        write(target.destination("tree/sub/keep"), "unrelated");
        std::vector<luciole::Location> selected{location(source.source("tree"))};
        struct stat original{}, sub{}, destination{}, previous{}, native{};
        TEST_CHECK(::stat(source.source("tree").c_str(), &original) == 0);
        TEST_CHECK(::stat(source.source("tree/sub").c_str(), &sub) == 0);
        TEST_CHECK(::stat(target.destination("tree/sub").c_str(), &destination) == 0);
        TEST_CHECK(::stat(target.destination("tree/sub/value").c_str(), &previous) == 0);
        if (mixed) {
          write(target.source("native"), "renamed");
          write(target.destination("native"), "native predecessor");
          fs::create_directories(target.source("merged/empty"));
          fs::create_directories(target.destination("merged/empty"));
          write(target.source("merged/file"), "native merge");
          TEST_CHECK(::stat(target.source("native").c_str(), &native) == 0);
          selected.push_back(location(target.source("native")));
          selected.push_back(location(target.source("merged")));
        }
        luciole::TransferService service;
        pump.run(service, selected, target.destination(""), TransferAction::Move, [](const auto& conflict) {
          return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
        });
        auto receipt = service.takeReceipt();
        TEST_CHECK(receipt && !fs::exists(source.source("tree")));
        write(target.destination("tree/sub/later"), "later sibling");
        for (int cycle = 0; cycle < 2; ++cycle) {
          const auto old = receipt;
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          if (!receipt) {
            for (const auto& result : service.results()) {
              if (result.error) {
                std::println("Merge Undo: {}", result.error->message);
              }
            }
          }
          TEST_CHECK(receipt);
          TEST_CHECK(read(source.source("tree/sub/value")) == "incoming");
          TEST_CHECK(read(source.source("tree/sub/文.txt")) == "Unicode");
          TEST_CHECK(read(source.source("tree/sub/" + raw)) == "raw");
          TEST_CHECK(fs::read_symlink(source.source("tree/sub/broken")) == "missing");
          struct stat actual{};
          TEST_CHECK(::stat(source.source("tree").c_str(), &actual) == 0 && luciole::sameIdentity(original, actual));
          TEST_CHECK(::stat(source.source("tree/sub").c_str(), &actual) == 0 && luciole::sameIdentity(sub, actual));
          TEST_CHECK(
              ::stat(target.destination("tree/sub/value").c_str(), &actual) == 0
              && luciole::sameIdentity(previous, actual)
          );
          if (mixed) {
            TEST_CHECK(::stat(target.source("native").c_str(), &actual) == 0 && luciole::sameIdentity(native, actual));
            TEST_CHECK(read(target.source("merged/file")) == "native merge");
            TEST_CHECK(read(target.destination("native")) == "native predecessor");
          }
          if (discardUndone && cycle == 1) {
            break;
          }
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          if (!receipt) {
            for (const auto& result : service.results()) {
              if (result.error) {
                std::println("Merge Redo: {}", result.error->message);
              }
            }
          }
          TEST_CHECK(receipt && !fs::exists(source.source("tree")));
          TEST_CHECK(read(target.destination("tree/sub/value")) == "incoming");
          if (mixed) {
            TEST_CHECK(!fs::exists(target.source("native")) && !fs::exists(target.source("merged")));
            TEST_CHECK(
                ::stat(target.destination("native").c_str(), &actual) == 0 && luciole::sameIdentity(native, actual)
            );
          }
        }
        TEST_CHECK(read(target.destination("tree/sub/keep")) == "unrelated");
        TEST_CHECK(read(target.destination("tree/sub/later")) == "later sibling");
        struct stat actual{};
        TEST_CHECK(
            ::stat(target.destination("tree/sub").c_str(), &actual) == 0 && luciole::sameIdentity(destination, actual)
        );
        receipt.reset();
        noPrivateMoves(source);
        noPrivateMoves(target);
      }
    }
  }
  void testMixedMoveGuards(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
      [&] {
        Fixture source;
        Fixture target("/dev/shm");
        if (!differentDevices(source, target)) {
          return;
        }
        fs::create_directories(source.source("tree/sub"));
        fs::create_directories(target.destination("tree/sub"));
        write(source.source("tree/sub/file"), "copied");
        write(target.source("native"), "renamed");
        luciole::TransferService service;
        pump.run(
            service, {location(source.source("tree")), location(target.source("native"))}, target.destination(""),
            TransferAction::Move, [](const auto&) { return TransferConflictChoice::Merge; }
        );
        auto receipt = service.takeReceipt();
        TEST_CHECK(receipt);
        if (scenario == 0) {
          fs::create_directory(source.source("tree"));
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && !service.historyRetired());
          TEST_CHECK(!fs::exists(target.source("native")) && read(target.destination("native")) == "renamed");
          fs::remove(source.source("tree"));
        } else if (scenario == 1) {
          luciole::CancelToken cancelled;
          cancelled.cancel();
          std::atomic<std::size_t> completed{0};
          const auto reply = receipt->reverse(cancelled, completed);
          TEST_CHECK(!reply.receipt && !reply.retireHistory && completed.load() == receipt->size());
          TEST_CHECK(!fs::exists(source.source("tree")) && !fs::exists(target.source("native")));
        } else if (scenario == 2) {
          write(target.destination("tree/sub/file"), "modified copied item");
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && service.historyRetired());
          TEST_CHECK(!fs::exists(source.source("tree")) && !fs::exists(target.source("native")));
          return;
        }
        pump.replay(service, receipt);
        auto redo = service.takeReceipt();
        TEST_CHECK(
            redo && read(source.source("tree/sub/file")) == "copied" && read(target.source("native")) == "renamed"
        );
        if (scenario == 3 || scenario == 4) {
          if (scenario == 3) {
            write(source.source("tree/sub/later"), "do not move");
          } else {
            fs::rename(source.source("tree"), source.source("held"));
            fs::create_directories(source.source("tree/sub"));
          }
          pump.replay(service, redo);
          TEST_CHECK(!service.receipt() && service.historyRetired());
          TEST_CHECK(read(target.source("native")) == "renamed" && !fs::exists(target.destination("native")));
          TEST_CHECK(!fs::exists(target.destination("tree/sub/file")));
          return;
        }
        pump.replay(service, receipt); // Old owner cannot replay a later generation.
        TEST_CHECK(!service.receipt() && service.historyRetired());
        pump.replay(service, redo);
        TEST_CHECK(service.receipt() && !fs::exists(source.source("tree")));
      }();
      drainHistoryCleanup();
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == (scenario < 2));
    }
  }
  void testCopiedMergeEmptyAndPartial(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (bool partial : {false, true}) {
      [&] {
        Fixture source;
        Fixture target("/dev/shm");
        if (!differentDevices(source, target)) {
          return;
        }
        fs::create_directories(source.source("tree/a"));
        fs::create_directories(target.destination("tree/a"));
        if (partial) {
          write(source.source("tree/b"), "incoming");
          write(target.destination("tree/b"), "existing");
        }
        luciole::TransferService service;
        pump.run(
            service, {location(source.source("tree"))}, target.destination(""), TransferAction::Move,
            [](const auto& conflict) {
              return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Skip;
            }
        );
        auto receipt = service.takeReceipt();
        TEST_CHECK(static_cast<bool>(receipt) == !partial);
        if (partial) {
          TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial);
          TEST_CHECK(read(source.source("tree/b")) == "incoming" && read(target.destination("tree/b")) == "existing");
          TEST_CHECK(!fs::exists(source.source("tree/a")) && service.report().contains("Retained item for inspection"));
        } else {
          TEST_CHECK(!fs::exists(source.source("tree")));
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          TEST_CHECK(receipt && fs::is_directory(source.source("tree/a")));
          pump.replay(service, receipt);
          TEST_CHECK(service.receipt() && !fs::exists(source.source("tree")));
        }
      }();
      drainHistoryCleanup();
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == !partial);
    }
  }

  void testNativeParentCopiedChildren(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (bool replacingParent : {false, true}) {
      for (bool discardUndone : {false, true}) {
        Fixture source;
        Fixture target("/dev/shm");
        if (!differentDevices(source, target)) {
          return;
        }
        fs::create_directories(target.source("one/tree/sub"));
        fs::create_directories(target.source("two/tree/nested/sub"));
        fs::create_directories(source.source("tree/sub"));
        fs::create_directories(source.source("tree/nested/sub"));
        fs::create_directories(source.source("tree/whole/deep"));
        write(target.source("one/tree/sub/value"), "original native value");
        write(target.source("one/tree/keep"), "original native sibling");
        write(target.source("two/tree/nested/sub/value"), "nested native value");
        write(source.source("tree/sub/value"), "copied replacement");
        write(source.source("tree/nested/sub/value"), "nested copied replacement");
        write(source.source("tree/whole/deep/文.txt"), "whole copied folder");
        const auto raw = std::string("raw-") + static_cast<char>(0xff);
        write(source.source("tree/" + raw), "raw copied name");
        fs::create_symlink("missing", source.source("tree/broken"));
        fs::create_hard_link(target.source("one/tree/sub/value"), target.source("loose"));
        if (replacingParent) {
          fs::create_directory(target.destination("tree"));
          write(target.destination("tree/previous"), "original destination root");
        }
        write(target.destination("outside"), "unrelated destination sibling");
        struct stat parent{}, nested{}, previous{};
        TEST_CHECK(::stat(target.source("one/tree").c_str(), &parent) == 0);
        TEST_CHECK(::stat(target.source("two/tree/nested").c_str(), &nested) == 0);
        if (replacingParent) {
          TEST_CHECK(::stat(target.destination("tree").c_str(), &previous) == 0);
        }
        luciole::TransferService service;
        const auto first = location(target.source("one/tree"));
        pump.run(
            service,
            {first, location(target.source("two/tree")), location(target.source("loose")),
             location(source.source("tree"))},
            target.destination(""), TransferAction::Move, [&](const auto& conflict) {
              return conflict.source == first ? TransferConflictChoice::Replace
                  : conflict.canMerge         ? TransferConflictChoice::Merge
                                              : TransferConflictChoice::Replace;
            }
        );
        auto receipt = service.takeReceipt();
        if (!receipt) {
          for (const auto& result : service.results()) {
            if (result.error) {
              std::println("Parent capture: {}", result.error->message);
            }
          }
        }
        TEST_CHECK(receipt && !fs::exists(target.source("one/tree")) && !fs::exists(source.source("tree")));
        TEST_CHECK(read(target.destination("tree/sub/value")) == "copied replacement");
        for (int cycle = 0; cycle < 2; ++cycle) {
          const auto old = receipt;
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          if (!receipt) {
            for (const auto& result : service.results()) {
              if (result.error) {
                std::println("Parent Undo: {}", result.error->message);
              }
            }
          }
          TEST_CHECK(receipt && !service.historyRetired());
          TEST_CHECK(read(target.source("one/tree/sub/value")) == "original native value");
          TEST_CHECK(read(target.source("two/tree/nested/sub/value")) == "nested native value");
          TEST_CHECK(read(source.source("tree/sub/value")) == "copied replacement");
          TEST_CHECK(read(source.source("tree/nested/sub/value")) == "nested copied replacement");
          TEST_CHECK(read(source.source("tree/whole/deep/文.txt")) == "whole copied folder");
          TEST_CHECK(
              read(source.source("tree/" + raw)) == "raw copied name"
              && fs::read_symlink(source.source("tree/broken")) == "missing"
          );
          struct stat actual{}, alias{};
          TEST_CHECK(::stat(target.source("one/tree").c_str(), &actual) == 0 && luciole::sameIdentity(parent, actual));
          TEST_CHECK(
              ::stat(target.source("two/tree/nested").c_str(), &actual) == 0 && luciole::sameIdentity(nested, actual)
          );
          TEST_CHECK(
              ::stat(target.source("one/tree/sub/value").c_str(), &actual) == 0
              && ::stat(target.source("loose").c_str(), &alias) == 0
              && luciole::sameIdentity(actual, alias)
          );
          if (replacingParent) {
            TEST_CHECK(read(target.destination("tree/previous")) == "original destination root");
            TEST_CHECK(
                ::stat(target.destination("tree").c_str(), &actual) == 0 && luciole::sameIdentity(previous, actual)
            );
          } else {
            TEST_CHECK(!fs::exists(target.destination("tree")));
          }
          if (discardUndone && cycle == 1) {
            break;
          }
          pump.replay(service, old);
          TEST_CHECK(!service.receipt() && service.historyRetired());
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          if (!receipt) {
            for (const auto& result : service.results()) {
              if (result.error) {
                std::println("Parent Redo: {}", result.error->message);
              }
            }
          }
          if (!receipt || fs::exists(target.source("one/tree")) || fs::exists(source.source("tree"))) {
            std::println(
                stderr,
                "Parent Redo paths: native={}, copied={}, cycle={}, replacing={}, receipt={}, results={}, retired={}",
                fs::exists(target.source("one/tree")), fs::exists(source.source("tree")), cycle, replacingParent,
                static_cast<bool>(receipt), service.results().size(), service.historyRetired()
            );
            for (const auto& result : service.results()) {
              std::println(
                  stderr, "Parent result {} -> {} ({})", result.source.uri(),
                  result.destination ? result.destination->uri() : "none", static_cast<int>(result.outcome)
              );
            }
          }
          TEST_CHECK(receipt && !fs::exists(target.source("one/tree")) && !fs::exists(source.source("tree")));
          TEST_CHECK(read(target.destination("tree/sub/value")) == "copied replacement");
          TEST_CHECK(read(target.destination("tree/nested/sub/value")) == "nested copied replacement");
          TEST_CHECK(read(target.destination("tree/whole/deep/文.txt")) == "whole copied folder");
          TEST_CHECK(read(target.destination("tree/keep")) == "original native sibling");
          TEST_CHECK(!fs::exists(target.destination("tree/previous")));
        }
        TEST_CHECK(read(target.destination("outside")) == "unrelated destination sibling");
        receipt.reset();
        noPrivateMoves(source);
        noPrivateMoves(target);
      }
    }
  }
  void testNativeParentCopyGuards(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
      [&] {
        Fixture source;
        Fixture target("/dev/shm");
        if (!differentDevices(source, target)) {
          return;
        }
        fs::create_directories(target.source("one/tree/sub"));
        fs::create_directories(source.source("tree/sub"));
        write(target.source("one/tree/sub/value"), "native baseline");
        write(target.source("one/tree/keep"), "native sibling");
        write(source.source("tree/sub/value"), "copied payload");
        luciole::TransferService service;
        pump.run(
            service, {location(target.source("one/tree")), location(source.source("tree"))}, target.destination(""),
            TransferAction::Move, [](const auto& conflict) {
              return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
            }
        );
        auto receipt = service.takeReceipt();
        TEST_CHECK(receipt);
        if (scenario == 0) {
          fs::create_directory(target.source("one/tree"));
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && !service.historyRetired());
          TEST_CHECK(
              !fs::exists(source.source("tree")) && read(target.destination("tree/sub/value")) == "copied payload"
          );
          fs::remove(target.source("one/tree"));
          luciole::CancelToken cancelled;
          cancelled.cancel();
          std::atomic<std::size_t> completed{0};
          const auto reply = receipt->reverse(cancelled, completed);
          TEST_CHECK(!reply.receipt && !reply.retireHistory && completed.load() == receipt->size());
          pump.replay(service, receipt);
          TEST_CHECK(service.receipt());
          return;
        }
        if (scenario == 1) {
          write(target.destination("tree/sub/value"), "changed copied destination");
        } else if (scenario == 2) {
          write(target.destination("tree/new-child"), "later user child");
        } else {
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          TEST_CHECK(receipt);
          if (scenario == 3) {
            write(source.source("tree/sub/value"), "edited source after Undo");
          } else if (scenario == 4) {
            write(target.source("one/tree/keep"), "edited native parent");
          } else {
            fs::rename(target.source("one"), target.source("held"));
            fs::create_directories(target.source("one/tree/sub"));
            write(target.source("one/tree/sub/foreign"), "unrelated source binding");
          }
        }
        pump.replay(service, receipt);
        TEST_CHECK(!service.receipt() && service.historyRetired());
        if (scenario <= 2) {
          TEST_CHECK(!fs::exists(source.source("tree")) && !fs::exists(target.source("one/tree")));
          TEST_CHECK(fs::exists(target.destination("tree/sub/value")));
        } else {
          TEST_CHECK(!fs::exists(target.destination("tree")));
          TEST_CHECK(
              read(source.source("tree/sub/value")) == (scenario == 3 ? "edited source after Undo" : "copied payload")
          );
          if (scenario == 5) {
            TEST_CHECK(read(target.source("one/tree/sub/foreign")) == "unrelated source binding");
          }
        }
      }();
      drainHistoryCleanup();
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == (scenario == 0));
    }
  }

  void testNativeParentCaptureFailure(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    for (bool changed : {false, true}) {
      [&] {
        Fixture source;
        Fixture target("/dev/shm");
        if (!differentDevices(source, target)) {
          return;
        }
        fs::create_directory(target.source("tree"));
        fs::create_directory(source.source("tree"));
        write(target.source("tree/b"), "native predecessor");
        write(target.source("tree/keep"), "native sibling");
        write(source.source("tree/a"), "first copied source");
        write(source.source("tree/b"), "second copied source");
        struct stat old{};
        TEST_CHECK(::stat(target.source("tree/b").c_str(), &old) == 0);
        luciole::TransferService service;
        pump.run(
            service, {location(target.source("tree")), location(source.source("tree"))}, target.destination(""),
            TransferAction::Move, [&](const auto& conflict) {
              if (conflict.canMerge) {
                return TransferConflictChoice::Merge;
              }
              if (changed) {
                write(target.destination("tree/keep"), "user changed the native parent");
                return TransferConflictChoice::Replace;
              }
              return TransferConflictChoice::Skip;
            }
        );
        TEST_CHECK(!service.receipt() && read(target.destination("tree/a")) == "first copied source");
        if (changed) {
          TEST_CHECK(service.results()[1].outcome == TransferOutcome::Moved && !fs::exists(source.source("tree")));
          TEST_CHECK(read(target.destination("tree/b")) == "second copied source");
          const auto retained = retainedItem(target, old);
          TEST_CHECK(read(retained) == "native predecessor" && service.report().contains(location(retained).uri()));
        } else {
          TEST_CHECK(service.results()[1].outcome == TransferOutcome::Partial);
          TEST_CHECK(
              read(source.source("tree/b")) == "second copied source"
              && read(target.destination("tree/b")) == "native predecessor"
          );
          noPrivateMoves(source);
          noPrivateMoves(target);
        }
      }();
      drainHistoryCleanup();
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == !changed);
    }
  }
  void testCopiedParentHistoryBoundary(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      return;
    }
    Fixture source;
    Fixture target("/dev/shm");
    if (!differentDevices(source, target)) {
      return;
    }
    fs::create_directory(source.source("tree"));
    fs::create_directory(target.source("tree"));
    write(source.source("tree/original"), "copied parent payload");
    write(target.source("tree/later"), "native child payload");
    luciole::TransferService service;
    pump.run(
        service, {location(source.source("tree")), location(target.source("tree"))}, target.destination(""),
        TransferAction::Move, [](const auto& conflict) {
          return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
        }
    );
    TEST_CHECK(!service.receipt() && !fs::exists(source.source("tree")) && !fs::exists(target.source("tree")));
    TEST_CHECK(
        read(target.destination("tree/original")) == "copied parent payload"
        && read(target.destination("tree/later")) == "native child payload"
    );
    noPrivateMoves(source);
    noPrivateMoves(target);
  }
  void testCrossFilesystem(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      std::println("Cross-filesystem check skipped: /dev/shm unavailable");
      return;
    }
    Fixture source;
    Fixture target("/dev/shm");
    struct stat a{}, b{};
    TEST_CHECK(::stat(source.root.c_str(), &a) == 0 && ::stat(target.root.c_str(), &b) == 0);
    if (a.st_dev == b.st_dev) {
      std::println("Cross-filesystem check skipped: fixtures share a filesystem");
      return;
    }
    fs::create_directories(source.source("tree/sub"));
    write(source.source("tree/sub/data"), "cross filesystem");
    fs::create_hard_link(source.source("tree/sub/data"), source.source("tree/sub/linked"));
    fs::create_symlink("missing", source.source("tree/broken"));
    luciole::TransferService service;
    pump.run(service, {location(source.source("tree"))}, target.destination(""), TransferAction::Move);
    TEST_CHECK(service.receipt());
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Moved
        && !fs::exists(source.source("tree"))
        && read(target.destination("tree/sub/linked")) == "cross filesystem"
        && fs::is_symlink(target.destination("tree/broken"))
    );
    fs::create_directories(source.source("merge/sub"));
    fs::create_directories(target.destination("merge/sub"));
    write(source.source("merge/sub/value"), "cross-filesystem merge");
    write(target.destination("merge/sub/keep"), "existing");
    pump.run(
        service, {location(source.source("merge"))}, target.destination(""), TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::Merge; }
    );
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Moved && !fs::exists(source.source("merge")));
    TEST_CHECK(
        read(target.destination("merge/sub/value")) == "cross-filesystem merge"
        && read(target.destination("merge/sub/keep")) == "existing"
    );
    // Sources are removed in durability batches; every item, including a final partial batch, still completes.
    std::vector<luciole::Location> many;
    for (int index = 0; index < 150; ++index) {
      const auto name = std::format("batch-{:03d}", index);
      write(source.source(name), name);
      many.push_back(location(source.source(name)));
    }
    fs::create_directories(source.source("wide"));
    fs::create_directories(target.destination("wide"));
    for (int index = 0; index < 70; ++index) {
      write(source.source(std::format("wide/child-{:02d}", index)), "child");
    }
    many.push_back(location(source.source("wide")));
    pump.run(service, many, target.destination(""), TransferAction::Move, [](const auto&) {
      return TransferConflictChoice::Merge;
    });
    TEST_CHECK(service.results().size() == 151);
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == TransferOutcome::Moved;
    }));
    TEST_CHECK(!fs::exists(source.source("batch-000")) && !fs::exists(source.source("batch-149")));
    TEST_CHECK(read(target.destination("batch-149")) == "batch-149" && !fs::exists(source.source("wide")));
    TEST_CHECK(read(target.destination("wide/child-69")) == "child");
    source.clean();
    target.clean();
  }
  void finish(
      Pump& pump, luciole::TransferService& service,
      const std::function<TransferConflictChoice(const luciole::TransferConflict&)>& choose, bool all = false
  ) {
    std::uint64_t handled = 0;
    pump.until([&] {
      if (const auto conflict = service.conflict(); conflict && conflict->id != handled) {
        handled = conflict->id;
        TEST_CHECK(service.resolve(conflict->id, choose(*conflict), all));
      }
      return !service.busy();
    });
  }
  void testMerge(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("tree/sub"));
      fs::create_directories(f.destination("tree/sub"));
      write(f.source("tree/sub/value"), "incoming");
      write(f.destination("tree/sub/value"), "old value");
      write(f.destination("tree/keep"), "destination-only");
      fs::create_symlink("missing", f.source("tree/broken"));
      TEST_CHECK(::chmod(f.destination("tree").c_str(), 0750) == 0);
      int prompts = 0;
      pump.run(service, {location(f.source("tree"))}, f.destination(""), action, [&](const auto& conflict) {
        ++prompts;
        if (conflict.sourceDirectory && conflict.destinationDirectory) {
          TEST_CHECK(conflict.canMerge);
          return TransferConflictChoice::Merge;
        }
        TEST_CHECK(!conflict.canMerge && conflict.destination == location(f.destination("tree/sub/value")));
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(
          prompts == 3
          && service.results().front().outcome
              == (action == TransferAction::Copy ? TransferOutcome::Copied : TransferOutcome::Moved)
      );
      TEST_CHECK(
          read(f.destination("tree/sub/value")) == "incoming" && read(f.destination("tree/keep")) == "destination-only"
      );
      TEST_CHECK(fs::is_symlink(f.destination("tree/broken")));
      TEST_CHECK(fs::exists(f.source("tree")) == (action == TransferAction::Copy));
      TEST_CHECK((fs::status(f.destination("tree")).permissions() & fs::perms::all) == fs::perms(0750));
      TEST_CHECK(!service.retryFailed() && service.report().contains("/tree/sub/value"));
      TEST_CHECK(static_cast<bool>(service.receipt()) == (action == TransferAction::Move));
      f.clean();
    }
  }
  void testMergeSkipAndCancel(Pump& pump) {
    for (const auto choice : {TransferConflictChoice::Skip, TransferConflictChoice::Cancel}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("tree"));
      fs::create_directories(f.destination("tree"));
      write(f.source("tree/a-done"), "done");
      write(f.source("tree/z-conflict"), "incoming");
      write(f.destination("tree/z-conflict"), "old");
      pump.run(
          service, {location(f.source("tree"))}, f.destination(""), TransferAction::Move,
          [&](const auto& conflict) { return conflict.canMerge ? TransferConflictChoice::Merge : choice; }
      );
      TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial);
      TEST_CHECK(read(f.destination("tree/a-done")) == "done" && !fs::exists(f.source("tree/a-done")));
      TEST_CHECK(read(f.destination("tree/z-conflict")) == "old" && read(f.source("tree/z-conflict")) == "incoming");
      TEST_CHECK(service.retryable() == 0 && service.results().front().children.size() == 2);
      f.clean();
    }
  }
  void testScopedChoices(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    for (const auto* folder : {"a", "b"}) {
      fs::create_directories(f.source(folder));
      fs::create_directories(f.destination(folder));
      write(f.source(std::string(folder) + "/value"), "incoming");
      write(f.destination(std::string(folder) + "/value"), "existing");
    }
    TEST_CHECK(service.start(
        {{location(f.source("a")), location(f.source("b"))}, location(f.destination("")), TransferAction::Copy}
    ));
    int prompts = 0;
    finish(
        pump, service,
        [&](const auto& conflict) {
          ++prompts;
          TEST_CHECK(conflict.canApplyToAll && !conflict.scopeLabel.empty());
          return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::KeepBoth;
        },
        true
    );
    TEST_CHECK(
        prompts == 2 && read(f.destination("a/value")) == "existing" && read(f.destination("b/value (1)")) == "incoming"
    );
    // New jobs do not inherit any policy.
    prompts = 0;
    pump.run(service, {location(f.source("a"))}, f.destination(""), TransferAction::Copy, [&](const auto&) {
      ++prompts;
      return TransferConflictChoice::Skip;
    });
    TEST_CHECK(prompts == 1);
    write(f.source("x"), "incoming x");
    write(f.source("y"), "incoming y");
    write(f.destination("x"), "old x");
    write(f.destination("y"), "old y");
    TEST_CHECK(service.start(
        {{location(f.source("x")), location(f.source("y"))}, location(f.destination("")), TransferAction::Copy}
    ));
    std::uint64_t handled = 0;
    prompts = 0;
    pump.until([&] {
      if (const auto c = service.conflict(); c && c->id != handled) {
        handled = c->id;
        ++prompts;
        if (prompts == 1) {
          write(f.root / "replacement", "foreign");
          fs::rename(f.root / "replacement", f.destination("x"));
          TEST_CHECK(service.resolve(c->id, TransferConflictChoice::Replace, true));
        } else {
          TEST_CHECK(service.resolve(c->id, TransferConflictChoice::Skip));
        }
      }
      return !service.busy();
    });
    TEST_CHECK(prompts == 2 && read(f.destination("x")) == "foreign" && read(f.destination("y")) == "incoming y");
    f.clean();
  }
  void testMergeBindings(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    fs::create_directories(f.source("tree"));
    fs::create_directories(f.destination("tree"));
    write(f.source("tree/value"), "incoming");
    write(f.destination("tree/old"), "existing");
    int prompts = 0;
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Copy, [&](const auto&) {
      if (++prompts == 1) {
        fs::rename(f.destination("tree"), f.destination("held"));
        fs::create_directory(f.destination("tree"));
        write(f.destination("tree/foreign"), "foreign");
        return TransferConflictChoice::Merge;
      }
      return TransferConflictChoice::Skip;
    });
    TEST_CHECK(
        prompts == 2 && !fs::exists(f.destination("tree/value")) && read(f.destination("held/old")) == "existing"
    );
    fs::remove_all(f.destination("tree"));
    fs::rename(f.destination("held"), f.destination("tree"));
    write(f.destination("tree/value"), "old");
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Copy, [&](const auto& c) {
      if (c.canMerge) {
        return TransferConflictChoice::Merge;
      }
      fs::rename(f.destination("tree"), f.destination("held"));
      fs::create_directory_symlink(f.root / "outside", f.destination("tree"));
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(!fs::exists(f.root / "outside/value") && read(f.destination("held/value")) == "old");
    TEST_CHECK(service.results().front().outcome != TransferOutcome::Copied);
    f.clean();
  }
  void testMergeLimitsAndNewEntries(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    fs::create_directory(f.source("tree"));
    fs::create_directory(f.destination("tree"));
    write(f.source("tree/a"), "captured");
    write(f.destination("tree/a"), "old");
    struct stat old{};
    TEST_CHECK(::lstat(f.destination("tree/a").c_str(), &old) == 0);
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Move, [&](const auto& c) {
      if (c.canMerge) {
        return TransferConflictChoice::Merge;
      }
      write(f.source("tree/new-arrival"), "not captured");
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial);
    TEST_CHECK(read(f.source("tree/new-arrival")) == "not captured" && !fs::exists(f.destination("tree/new-arrival")));
    TEST_CHECK(read(f.destination("tree/a")) == "captured");
    const auto retained = retainedItem(f, old);
    TEST_CHECK(read(retained) == "old" && service.report().contains(location(retained).uri()));
    drainHistoryCleanup();
    const auto recovery = luciole::CopyReceipt::takeCleanupErrors();
    TEST_CHECK(recovery.size() == 1 && recovery.front().message.contains(retained.native()));
    for (unsigned i = 0; i < 16384; ++i) {
      write(f.source("tree/" + std::to_string(i)), "x");
    }
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Copy, [](const auto&) {
      return TransferConflictChoice::Merge;
    });
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Failed);
    TEST_CHECK(
        service.results().front().error
        && service.results().front().error->message.contains("16384")
        && !fs::exists(f.destination("tree/0"))
    );
    f.clean();
  }
  void testRetry(Pump& pump) {
    if (::geteuid() == 0) {
      return; // Permission-denial fixture needs an ordinary process.
    }
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("tree"));
      fs::create_directories(f.destination("tree"));
      write(f.source("tree/a-done"), "completed");
      write(f.source("tree/z-failed"), "retry me");
      write(f.destination("tree/z-failed"), "existing");
      pump.run(service, {location(f.source("tree"))}, f.destination(""), action, [&](const auto& c) {
        if (c.canMerge) {
          return TransferConflictChoice::Merge;
        }
        TEST_CHECK(::chmod(f.destination("tree").c_str(), 0555) == 0);
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial && service.retryable() == 1);
      struct stat before{}, after{};
      TEST_CHECK(::stat(f.destination("tree/a-done").c_str(), &before) == 0);
      TEST_CHECK(::chmod(f.destination("tree").c_str(), 0755) == 0);
      TEST_CHECK(service.retryFailed());
      finish(pump, service, [](const auto&) { return TransferConflictChoice::Replace; });
      TEST_CHECK(
          service.results().front().outcome
          == (action == TransferAction::Copy ? TransferOutcome::Copied : TransferOutcome::Moved)
      );
      TEST_CHECK(read(f.destination("tree/z-failed")) == "retry me" && service.retryable() == 0);
      TEST_CHECK(::stat(f.destination("tree/a-done").c_str(), &after) == 0 && before.st_ino == after.st_ino);
      TEST_CHECK(fs::exists(f.source("tree")) == (action == TransferAction::Copy));
      f.clean();
    }
    {
      Fixture f;
      luciole::TransferService service;
      fs::create_directories(f.source("tree"));
      fs::create_directories(f.destination("tree"));
      write(f.source("tree/a-done"), "completed");
      write(f.source("tree/z-failed"), "pending");
      write(f.destination("tree/z-failed"), "old");
      pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Move, [&](const auto& c) {
        if (c.canMerge) {
          return TransferConflictChoice::Merge;
        }
        TEST_CHECK(::chmod(f.destination("tree").c_str(), 0555) == 0);
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(service.retryable() == 1);
      TEST_CHECK(service.retryFailed());
      service.cancel();
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial);
      TEST_CHECK(read(f.destination("tree/a-done")) == "completed" && !fs::exists(f.source("tree/a-done")));
      TEST_CHECK(read(f.source("tree/z-failed")) == "pending" && read(f.destination("tree/z-failed")) == "old");
      TEST_CHECK(::chmod(f.destination("tree").c_str(), 0755) == 0);
      f.clean();
    }
    // A failed standalone item retries without replaying a completed sibling.
    Fixture f;
    luciole::TransferService service;
    write(f.source("a"), "done");
    write(f.source("b"), "retry");
    write(f.destination("b"), "old");
    pump.run(
        service, {location(f.source("a")), location(f.source("b"))}, f.destination(""), TransferAction::Copy,
        [&](const auto&) {
          TEST_CHECK(::chmod(f.destination("").c_str(), 0555) == 0);
          return TransferConflictChoice::Replace;
        }
    );
    TEST_CHECK(service.retryable() == 1);
    TEST_CHECK(::chmod(f.destination("").c_str(), 0755) == 0);
    write(f.root / "new-b", "foreign source");
    fs::rename(f.root / "new-b", f.source("b"));
    TEST_CHECK(service.retryFailed());
    finish(pump, service, [](const auto&) { return TransferConflictChoice::Replace; });
    TEST_CHECK(service.retryable() == 0 && read(f.destination("b")) == "old" && read(f.destination("a")) == "done");
    TEST_CHECK(
        service.results().front().outcome == TransferOutcome::Copied
        && service.results().back().outcome == TransferOutcome::Failed
    );
    f.clean();
  }
  void testSymbolicLinkDrops(Pump& pump) {
    Fixture f;
    fs::create_directories(f.source("folder/nested"));
    write(f.source("folder/nested/child"), "inside");
    write(f.source("café.txt"), "source bytes");
    fs::create_symlink("missing", f.source("broken"));
    luciole::TransferService service;
    pump.run(
        service,
        {location(f.source("folder")), location(f.source("folder/nested/child")), location(f.source("café.txt")),
         location(f.source("broken"))},
        f.root / "destination", TransferAction::Link
    );
    TEST_CHECK(service.results().size() == 4 && service.bytes() == 0);
    for (const auto& result : service.results()) {
      TEST_CHECK(result.outcome == TransferOutcome::Linked && result.destination && !result.error);
      TEST_CHECK(fs::read_symlink(*result.destination->nativePath()) == *result.source.nativePath());
    }
    TEST_CHECK(read(f.source("café.txt")) == "source bytes" && read(f.destination("café.txt")) == "source bytes");
    TEST_CHECK(read(f.source("folder/nested/child")) == "inside" && fs::is_symlink(f.destination("folder")));
    TEST_CHECK(fs::is_symlink(f.source("broken")) && fs::is_symlink(f.destination("broken")));
    TEST_CHECK(service.report().contains("Linked"));
    // Collision decisions never merge directories for a symbolic-link operation.
    pump.run(
        service, {location(f.source("folder"))}, f.root / "destination", TransferAction::Link,
        [](const auto& conflict) {
          TEST_CHECK(!conflict.canMerge);
          return TransferConflictChoice::KeepBoth;
        }
    );
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Linked);
    TEST_CHECK(fs::read_symlink(f.destination("folder (1)")) == f.source("folder"));
    fs::remove(f.destination("café.txt"));
    write(f.destination("café.txt"), "old destination");
    pump.run(service, {location(f.source("café.txt"))}, f.root / "destination", TransferAction::Link, [](const auto&) {
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        service.results().front().outcome == TransferOutcome::Linked && fs::is_symlink(f.destination("café.txt"))
    );
    TEST_CHECK(read(f.source("café.txt")) == "source bytes");
    pump.run(service, {location(f.source("café.txt"))}, f.root / "source", TransferAction::Link);
    TEST_CHECK(fs::is_symlink(f.source("café (1).txt")) && read(f.source("café.txt")) == "source bytes");
    // A captured source replaced while a collision is pending is never linked.
    TEST_CHECK(
        service.start({{location(f.source("café.txt"))}, location(f.root / "destination"), TransferAction::Link})
    );
    pump.until([&] { return service.conflict().has_value(); });
    fs::rename(f.source("café.txt"), f.source("original"));
    write(f.source("café.txt"), "replacement");
    TEST_CHECK(service.resolve(service.conflict()->id, TransferConflictChoice::KeepBoth));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Failed);
    TEST_CHECK(!fs::exists(f.destination("café (1).txt")) && read(f.source("café.txt")) == "replacement");
    pump.run(service, {location(f.source("folder"))}, f.source("folder/nested"), TransferAction::Link);
    TEST_CHECK(
        service.results().front().outcome == TransferOutcome::Failed && !fs::exists(f.source("folder/nested/folder"))
    );
    f.clean();
  }

  struct MountedFixture {
    Fixture storage, aliases;
    MountedFixture() {
      bind(storage.source(""), aliases.source(""));
      bind(storage.destination(""), aliases.destination(""));
    }
    ~MountedFixture() {
      // Owned mount bindings must outlive deferred history cleanup from the finished service.
      drainHistoryCleanup();
      TEST_CHECK(::umount2(aliases.destination("").c_str(), MNT_DETACH) == 0);
      TEST_CHECK(::umount2(aliases.source("").c_str(), MNT_DETACH) == 0);
    }
    static void bind(const fs::path& original, const fs::path& target) {
      TEST_CHECK(::mount(original.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) == 0);
    }
    static void rebind(const fs::path& original, const fs::path& target) {
      TEST_CHECK(::umount2(target.c_str(), MNT_DETACH) == 0);
      bind(original, target);
    }
  };
  void testDetachedStagePath() {
    MountedFixture f;
    const auto target =
        luciole::checkedFd(::open(f.aliases.destination("").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    luciole::CreationStage stage(target.get(), false);
    const auto original = stage.recoveryPath();
    TEST_CHECK(original.starts_with((f.aliases.root / "destination").native() + "/.luciole-create-"));
    fs::create_directory(f.storage.root / "foreign");
    MountedFixture::rebind(f.storage.root / "foreign", f.aliases.destination(""));
    TEST_CHECK(stage.recoveryPath() == original);
    MountedFixture::rebind(f.storage.destination(""), f.aliases.destination(""));
    TEST_CHECK(stage.recoveryPath() == original && fs::is_directory(fs::path(original).parent_path()));
  }
  void testMountedMoveHistory(Pump& pump, bool reverseDirection) {
    MountedFixture f;
    const auto from = reverseDirection ? f.aliases.source("") : f.storage.source("");
    const auto to = reverseDirection ? f.storage.destination("") : f.aliases.destination("");
    const auto sourceFd = luciole::checkedFd(::open(from.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto targetFd = luciole::checkedFd(::open(to.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    TEST_CHECK(luciole::statFd(sourceFd.get()).st_dev == luciole::statFd(targetFd.get()).st_dev);
    TEST_CHECK(!luciole::sameMount(sourceFd.get(), targetFd.get()));
    write(from / "probe", "probe");
    TEST_CHECK(::syscall(SYS_renameat2, sourceFd.get(), "probe", targetFd.get(), "probe", RENAME_NOREPLACE) == -1);
    TEST_CHECK(errno == EXDEV && fs::exists(from / "probe") && !fs::exists(to / "probe"));
    fs::remove(from / "probe");
    fs::create_directories(from / "tree/sub/empty");
    fs::create_directories(to / "tree/sub/empty");
    fs::create_directories(from / "whole/deep");
    fs::create_directories(to / "inbox");
    write(from / "tree/sub/value", "merged payload");
    write(to / "tree/sub/value", "merged predecessor");
    write(to / "tree/sub/keep", "destination only");
    write(from / "whole/deep/file", "whole folder payload");
    write(from / "plain", "copied payload");
    write(to / "plain", "native payload");
    // The native source aliases a predecessor retained by the copied branch of the same job.
    fs::create_hard_link(to / "plain", to / "inbox/native");
    write(to / "native", "native predecessor");
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    write(from / raw, "raw payload");
    fs::create_symlink("missing", from / "broken");
    struct stat tree{}, previous{}, native{}, container{};
    TEST_CHECK(::stat((from / "tree").c_str(), &tree) == 0);
    TEST_CHECK(::stat((to / "plain").c_str(), &previous) == 0);
    TEST_CHECK(::stat((to / "inbox/native").c_str(), &native) == 0);
    TEST_CHECK(::stat((to / "tree/sub").c_str(), &container) == 0);
    luciole::TransferService service;
    pump.run(
        service,
        {location(from / "tree"), location(from / "whole"), location(from / "plain"), location(from / raw),
         location(from / "broken"), location(to / "inbox/native")},
        to, TransferAction::Move, [](const auto& conflict) {
          return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
        }
    );
    auto receipt = service.takeReceipt();
    TEST_CHECK(receipt && !fs::exists(from / "tree") && read(to / "plain") == "copied payload");
    write(to / "tree/sub/later", "later destination sibling");
    for (int cycle = 0; cycle < 3; ++cycle) {
      pump.replay(service, receipt);
      receipt = service.takeReceipt();
      if (!receipt) {
        for (const auto& result : service.results()) {
          if (result.error) {
            std::println("Mounted Undo: {}", result.error->message);
          }
        }
      }
      TEST_CHECK(receipt && read(from / "plain") == "copied payload" && read(to / "plain") == "native payload");
      TEST_CHECK(read(from / "whole/deep/file") == "whole folder payload");
      TEST_CHECK(
          read(from / "tree/sub/value") == "merged payload" && read(to / "tree/sub/value") == "merged predecessor"
      );
      TEST_CHECK(read(from / raw) == "raw payload" && fs::read_symlink(from / "broken") == "missing");
      struct stat actual{};
      TEST_CHECK(::stat((from / "tree").c_str(), &actual) == 0 && luciole::sameIdentity(tree, actual));
      TEST_CHECK(::stat((to / "plain").c_str(), &actual) == 0 && luciole::sameIdentity(previous, actual));
      TEST_CHECK(::stat((to / "inbox/native").c_str(), &actual) == 0 && luciole::sameIdentity(native, actual));
      TEST_CHECK(read(to / "native") == "native predecessor");
      pump.replay(service, receipt);
      receipt = service.takeReceipt();
      TEST_CHECK(receipt && !fs::exists(from / "tree") && !fs::exists(to / "inbox/native"));
      TEST_CHECK(read(to / "plain") == "copied payload" && read(to / "native") == "native payload");
      TEST_CHECK(
          read(to / "tree/sub/keep") == "destination only" && read(to / "tree/sub/later") == "later destination sibling"
      );
      TEST_CHECK(::stat((to / "tree/sub").c_str(), &actual) == 0 && luciole::sameIdentity(container, actual));
    }
    receipt.reset();
    noPrivateMoves(f.storage);
  }
  void testMountedMoveGuards(Pump& pump) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
      [&] {
        MountedFixture f;
        const auto from = f.aliases.source(""), to = f.aliases.destination("");
        fs::create_directories(from / "tree/sub");
        fs::create_directories(to / "tree/sub");
        fs::create_directories(to / "inbox");
        write(from / "tree/sub/file", "copied payload");
        write(to / "inbox/native", "native payload");
        luciole::TransferService service;
        pump.run(
            service, {location(from / "tree"), location(to / "inbox/native")}, to, TransferAction::Move,
            [](const auto&) { return TransferConflictChoice::Merge; }
        );
        auto receipt = service.takeReceipt();
        TEST_CHECK(receipt);
        if (scenario == 0) {
          fs::create_directory(from / "tree");
          pump.replay(service, receipt);
          TEST_CHECK(!service.receipt() && !service.historyRetired());
          TEST_CHECK(!fs::exists(to / "inbox/native") && read(to / "native") == "native payload");
          fs::remove(from / "tree");
          luciole::CancelToken cancelled;
          cancelled.cancel();
          std::atomic<std::size_t> completed{0};
          const auto reply = receipt->reverse(cancelled, completed);
          TEST_CHECK(!reply.receipt && !reply.retireHistory && completed.load() == receipt->size());
          pump.replay(service, receipt);
          TEST_CHECK(service.receipt() && read(from / "tree/sub/file") == "copied payload");
          return;
        }
        if (scenario == 1) {
          write(to / "tree/sub/file", "changed destination");
        } else if (scenario == 2) {
          pump.replay(service, receipt);
          receipt = service.takeReceipt();
          TEST_CHECK(receipt);
          write(from / "tree/sub/later", "do not move");
        } else {
          fs::create_directory(f.storage.root / "foreign");
          write(f.storage.root / "foreign/keep", "foreign payload");
          MountedFixture::rebind(f.storage.root / "foreign", scenario == 3 ? to : from);
        }
        pump.replay(service, receipt);
        TEST_CHECK(!service.receipt() && service.historyRetired());
        if (scenario == 2) {
          TEST_CHECK(read(from / "tree/sub/later") == "do not move");
          TEST_CHECK(read(to / "inbox/native") == "native payload" && !fs::exists(to / "native"));
        } else {
          TEST_CHECK(!fs::exists(f.storage.source("tree")));
          TEST_CHECK(
              !fs::exists(f.storage.destination("inbox/native"))
              && read(f.storage.destination("native")) == "native payload"
          );
        }
        if (scenario >= 3) {
          TEST_CHECK(read(f.storage.root / "foreign/keep") == "foreign payload");
          MountedFixture::rebind(
              scenario == 3 ? f.storage.destination("") : f.storage.source(""), scenario == 3 ? to : from
          );
        }
      }();
      drainHistoryCleanup();
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == (scenario == 0));
    }
  }

  void testMountedNativeParent(Pump& pump, bool reverseDirection) {
    MountedFixture f;
    const auto from = reverseDirection ? f.aliases.source("") : f.storage.source("");
    const auto to = reverseDirection ? f.storage.destination("") : f.aliases.destination("");
    fs::create_directories(to / "inbox/tree/sub");
    fs::create_directories(from / "tree/sub");
    write(to / "inbox/tree/sub/value", "native baseline");
    write(from / "tree/sub/value", "bind-mounted copied child");
    struct stat original{};
    TEST_CHECK(::stat((to / "inbox/tree").c_str(), &original) == 0);
    luciole::TransferService service;
    pump.run(
        service, {location(to / "inbox/tree"), location(from / "tree")}, to, TransferAction::Move,
        [](const auto& conflict) {
          return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
        }
    );
    auto receipt = service.takeReceipt();
    TEST_CHECK(receipt);
    for (int cycle = 0; cycle < 2; ++cycle) {
      pump.replay(service, receipt);
      receipt = service.takeReceipt();
      TEST_CHECK(receipt);
      TEST_CHECK(
          read(to / "inbox/tree/sub/value") == "native baseline"
          && read(from / "tree/sub/value") == "bind-mounted copied child"
      );
      struct stat actual{};
      TEST_CHECK(::stat((to / "inbox/tree").c_str(), &actual) == 0 && luciole::sameIdentity(original, actual));
      TEST_CHECK(!fs::exists(to / "tree"));
      pump.replay(service, receipt);
      receipt = service.takeReceipt();
      TEST_CHECK(receipt);
      TEST_CHECK(read(to / "tree/sub/value") == "bind-mounted copied child" && !fs::exists(from / "tree"));
    }
    receipt.reset();
    noPrivateMoves(f.storage);
  }
  int mountedHistoryChecks() {
    // Always create a private mount namespace before changing propagation or making any bind mount.
    if (::unshare(CLONE_NEWNS) != 0) {
      std::println(
          "Bind-mount history checks unavailable: cannot create a private mount namespace ({})",
          luciole::errnoError(errno).message
      );
      return 77;
    }
    TEST_CHECK(::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0);
    Pump pump;
    testDetachedStagePath();
    testMountedMoveHistory(pump, false);
    testMountedMoveHistory(pump, true);
    testMountedMoveGuards(pump);
    testMountedNativeParent(pump, false);
    testMountedNativeParent(pump, true);
    std::println("Same-device bind-mount Move history checks passed");
    return 0;
  }

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--mount-history") {
      return mountedHistoryChecks();
    }
    Pump pump;
    if (argc == 3 && std::string_view(argv[1]) == "--replacement-filesystem") {
      fixtureParent = argv[2];
      TEST_CHECK(fs::is_directory(fixtureParent));
      testRenameRevisions();
      testReplacementHistory(pump);
      testReplacementGuards(pump);
      testFailedRetentionHandoff();
      testMoveReplacementRecovery(pump);
      testMoveRecoveryGuards(pump);
      testMoveRetentionHandoffFailure();
      testReplacementSourceGuard(pump);
      testReplacementTimestampGuards(pump);
      drainHistoryCleanup();
      std::println("Replacement filesystem checks passed: {}", fixtureParent);
      return 0;
    }
    TEST_CHECK(argc == 1);
    testRenameRevisions();
    testReplacementSourceGuard(pump);
    testReplacementTimestampGuards(pump);
    testSymbolicLinkDrops(pump);
    testCopyHistory(pump);
    testCopyHistoryGuards(pump);
    testCopyHistoryRetention(pump);
    testCopyHistoryCancellation(pump);
    testReplacementHistory(pump);
    testCopyMergeHistory(pump);
    testReplacementGuards(pump);
    testReplacementAlias(pump);
    testIncompleteReplacement(pump);
    testReplacementCancellation(pump);
    testFailedRetentionHandoff();
    testAliasRemovalDuringPublication();
    testRepeatedDestinationRecovery(pump);
    testMoveReplacementRecovery(pump);
    testMoveMergeRecovery(pump);
    testMoveRecoveryGuards(pump);
    testMoveRetentionHandoffFailure();
    testMoveReplacementCancellation(pump);
    testMoveRecoveryAliasesAndChains(pump);
    testAliasedPreviousTrees(pump);
    testAliasMutationGuards(pump);
    testRepeatedTransferVersions(pump);
    testRepeatedMergeLeaves(pump);
    testIncomingAliasChains(pump);
    testRepeatedHistoryGuards(pump);
    testRepeatedMoveCancellation(pump);
    testOverlappingCopies(pump);
    testOverlappingCopyGuards(pump);
    testChangedOverlapDuringTransfer(pump);
    testCopyReplacedAncestor(pump);
    testCopyIntoExistingAncestor(pump);
    testNestedCopyAncestor(pump);
    testCopyAncestorGuards(pump);
    testPrivateParentReconstructionCancellation();
    testMoveAncestorReplacement(pump);
    testMoveAncestorGenerations(pump);
    testMoveAncestorGuards(pump);
    testMoveAncestorCancellation(pump);
    testOverlappingMoves(pump);
    testOverlappingMoveGuards(pump);
    testOverlappingMoveCancellation(pump);
    testOverlappingMoveProofBudget(pump);
    testOverlappingEmptyMoves(pump);
    testRejectedSnapshotThenRepeatedCopies(pump);
    testMoveHistory(pump);
    testMoveHistoryGuards(pump);
    testMoveHistoryAliasesAndNames(pump);
    testMoveHistoryInterruption(pump);
    testTransferHistoryBounds(pump);
    testMoveAndCopy(pump);
    testConflicts(pump);
    testGuardsAndRaces(pump);
    testCancellationAndNumbering(pump);
    testCopiedMoveHistory(pump);
    testCopiedMoveReplacements(pump);
    testCopiedMoveGuards(pump);
    testCopiedMovePartialAndNames(pump);
    testMixedMoveMerges(pump);
    testMixedMoveGuards(pump);
    testCopiedMergeEmptyAndPartial(pump);
    testNativeParentCopiedChildren(pump);
    testNativeParentCopyGuards(pump);
    testNativeParentCaptureFailure(pump);
    testCopiedParentHistoryBoundary(pump);
    testCrossFilesystem(pump);
    testMerge(pump);
    testMergeSkipAndCancel(pump);
    testScopedChoices(pump);
    testMergeBindings(pump);
    testRetry(pump);
    testMergeLimitsAndNewEntries(pump);
    pump.until([] { return !luciole::CopyReceipt::cleanupPending(); });
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    std::println("Transfer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Transfer test: {}", error.what());
    return 1;
  }
}
