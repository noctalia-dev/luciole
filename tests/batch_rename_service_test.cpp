#include "event_loop/glib_poll_source.h"
#include "fs/filename.h"
#include "fs/gio_interop.h"
#include "operations/batch_rename_service.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/fs.h>
#include <print>
#include <sched.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using S = luciole::BatchRenameService;
  using P = luciole::BatchRenamePhase;
  using O = luciole::BatchRenameOutcome;
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/luciole-batch-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()));
      root = path;
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  void write(const fs::path& path, const std::string& value) { std::ofstream(path) << value; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }
  luciole::DirectoryEntry entry(const fs::path& path) {
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(location(path)).get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info);
    luciole::DirectoryEntry result{
        .id = 1,
        .place = location(path),
        .name = path.filename().native(),
        .type = luciole::fileType(g_file_info_get_file_type(info.get())),
        .symlink = g_file_info_get_is_symlink(info.get()) != FALSE,
        .size = {},
        .modifiedSeconds = {}
    };
    result.setDisplayName(luciole::filenameDisplayText(path.filename().native()));
    result.setIdentity(luciole::readFileIdentity(info.get()));
    return result;
  }
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
    void settle(S& service) {
      until([&] { return !service.busy(); });
    }
    void prepare(S& service, std::vector<luciole::DirectoryEntry> entries) {
      TEST_CHECK(service.prepare(std::move(entries)));
      settle(service);
      TEST_CHECK(service.snapshot().phase == P::Ready);
    }
    void preview(S& service, const luciole::BatchRenameOptions& options) {
      TEST_CHECK(service.preview(options));
      TEST_CHECK(!service.snapshot().canApply);
      settle(service);
    }
    void apply(S& service) {
      TEST_CHECK(service.apply());
      settle(service);
    }

  private:
    GlibPollSource m_source;
  };
  void names() {
    luciole::BatchRenameOptions o;
    TEST_CHECK(luciole::batchRenameName("résumé.png", false, 2, o) == "résumé_003.png");
    TEST_CHECK(luciole::batchRenameName(".profile", false, 0, o) == ".profile_001");
    TEST_CHECK(luciole::batchRenameName("folder.ext", true, 0, o) == "folder.ext_001");
    o.pattern = "{{{number}}}_{name}";
    o.start = 9;
    o.digits = 1;
    TEST_CHECK(luciole::batchRenameName("a.tar.gz", false, 1, o) == "{10}_a.tar.gz");
    o.mode = luciole::BatchRenameMode::Replace;
    o.find = "文";
    o.replacement = "é";
    TEST_CHECK(luciole::batchRenameName("文文.文", false, 0, o) == "éé.文");
    o.keepExtensions = false;
    TEST_CHECK(luciole::batchRenameName("文文.文", false, 0, o) == "éé.é");
    const auto rejects = [](const luciole::BatchRenameOptions& options) {
      bool rejected = false;
      try {
        (void)luciole::batchRenameName("a", false, 0, options);
      } catch (const std::exception&) {
        rejected = true;
      }
      TEST_CHECK(rejected);
    };
    o.find.clear();
    rejects(o);
    o = {};
    o.pattern = "{bad}";
    rejects(o);
    o = {};
    o.digits = 10;
    rejects(o);
    o = {};
    o.start = -1;
    rejects(o);
    o = {};
    o.pattern.assign(4097, 'x');
    rejects(o);
    o = {};
    o.mode = luciole::BatchRenameMode::Replace;
    o.find = "a";
    o.replacement.assign(4096, 'b');
    bool rejected = false;
    try {
      (void)luciole::batchRenameName("aa", false, 0, o);
    } catch (const std::exception&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
  }
  void basicAndValidation(Pump& pump) {
    Fixture f;
    write(f.root / "a.txt", "A");
    write(f.root / "b.txt", "B");
    S service;
    TEST_CHECK(!service.prepare({}));
    TEST_CHECK(!service.prepare({entry(f.root / "a.txt")}));
    TEST_CHECK(!service.prepare(std::vector<luciole::DirectoryEntry>(4097, entry(f.root / "a.txt"))));
    TEST_CHECK(service.prepare({entry(f.root / "a.txt"), entry(f.root / "a.txt")}));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
    auto invalid = entry(f.root / "b.txt");
    invalid.setIdentity(std::nullopt);
    TEST_CHECK(service.prepare({entry(f.root / "a.txt"), invalid}));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
    pump.prepare(service, {entry(f.root / "b.txt"), entry(f.root / "a.txt")});
    TEST_CHECK(service.snapshot().canApply);
    TEST_CHECK(service.snapshot().rows->front().after == "b_001.txt");
    luciole::BatchRenameOptions o;
    o.pattern = "same";
    pump.preview(service, o);
    TEST_CHECK(!service.snapshot().canApply && !service.snapshot().rows->front().error.empty());
    for (const auto& name : std::vector<std::string>{"", "..", "a/b", std::string(256, 'x')}) {
      o.pattern = name;
      o.keepExtensions = false;
      pump.preview(service, o);
      TEST_CHECK(!service.snapshot().canApply);
    }
    o = {};
    o.pattern = "{name}";
    pump.preview(service, o);
    TEST_CHECK(!service.snapshot().canApply);
    o = {};
    o.pattern = "new-{number}";
    write(f.root / "new-001.txt", "external");
    pump.preview(service, o);
    TEST_CHECK(!service.snapshot().canApply);
    fs::remove(f.root / "new-001.txt");
    fs::create_symlink("missing", f.root / "new-001.txt");
    pump.preview(service, o);
    TEST_CHECK(!service.snapshot().canApply);
    fs::remove(f.root / "new-001.txt");
    fs::create_directory(f.root / "new-001.txt");
    pump.preview(service, o);
    TEST_CHECK(!service.snapshot().canApply);
    fs::remove(f.root / "new-001.txt");
    TEST_CHECK(::mkfifo((f.root / "new-001.txt").c_str(), 0600) == 0);
    pump.preview(service, o);
    TEST_CHECK(!service.snapshot().canApply);
    fs::remove(f.root / "new-001.txt");
    if (::geteuid() != 0) {
      fs::permissions(f.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.preview(service, o);
      TEST_CHECK(!service.snapshot().canApply);
      fs::permissions(f.root, fs::perms::owner_all);
    }
    pump.preview(service, o);
    pump.apply(service);
    TEST_CHECK(service.snapshot().phase == P::Finished);
    TEST_CHECK(read(f.root / "new-001.txt") == "B" && read(f.root / "new-002.txt") == "A");
    TEST_CHECK(service.snapshot().results->size() == 2 && service.completed() == 2);
    TEST_CHECK(!service.snapshot().report.empty());
    for (const auto& result : *service.snapshot().results) {
      TEST_CHECK(result.outcome == O::Renamed && result.actual && fs::exists(*result.actual->nativePath()));
    }
    TEST_CHECK(service.snapshot().receipt);
    TEST_CHECK(std::distance(fs::directory_iterator(f.root), fs::directory_iterator{}) == 2);
  }
  void cyclesAndKinds(Pump& pump) {
    Fixture f;
    S service;
    for (int count : {2, 3, 128}) {
      std::vector<luciole::DirectoryEntry> entries;
      for (int i = 1; i <= count; ++i) {
        write(f.root / ("file" + std::to_string(i)), std::to_string(i));
      }
      // Last first creates a swap or a full cycle in the numbering order.
      entries.push_back(entry(f.root / ("file" + std::to_string(count))));
      for (int i = 1; i < count; ++i) {
        entries.push_back(entry(f.root / ("file" + std::to_string(i))));
      }
      pump.prepare(service, entries);
      luciole::BatchRenameOptions o;
      o.pattern = "file{number}";
      o.digits = 1;
      pump.preview(service, o);
      pump.apply(service);
      TEST_CHECK(read(f.root / "file1") == std::to_string(count));
      for (int i = 2; i <= count; ++i) {
        TEST_CHECK(read(f.root / ("file" + std::to_string(i))) == std::to_string(i - 1));
      }
      TEST_CHECK(service.replay(service.snapshot().receipt));
      pump.settle(service);
      TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().receipt);
      for (int i = 1; i <= count; ++i) {
        TEST_CHECK(read(f.root / ("file" + std::to_string(i))) == std::to_string(i));
      }
      TEST_CHECK(service.replay(service.snapshot().receipt));
      pump.settle(service);
      TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().receipt);
      TEST_CHECK(read(f.root / "file1") == std::to_string(count));
      for (int i = 2; i <= count; ++i) {
        TEST_CHECK(read(f.root / ("file" + std::to_string(i))) == std::to_string(i - 1));
      }
      for (int i = 1; i <= count; ++i) {
        fs::remove(f.root / ("file" + std::to_string(i)));
      }
      TEST_CHECK(fs::is_empty(f.root));
    }
    fs::create_directory(f.root / "dir.ext");
    write(f.root / "dir.ext/child", "child");
    fs::create_symlink("absent", f.root / "broken");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(f.root / raw, "raw");
    pump.prepare(service, {entry(f.root / "dir.ext"), entry(f.root / "broken"), entry(f.root / raw)});
    luciole::BatchRenameOptions o;
    o.pattern = "result-{number}";
    pump.preview(service, o);
    pump.apply(service);
    TEST_CHECK(read(f.root / "result-001/child") == "child");
    TEST_CHECK(fs::read_symlink(f.root / "result-002") == "absent");
    TEST_CHECK(read(f.root / "result-003") == "raw");
    TEST_CHECK(service.replay(service.snapshot().receipt));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().receipt);
    TEST_CHECK(read(f.root / "dir.ext/child") == "child");
    TEST_CHECK(fs::read_symlink(f.root / "broken") == "absent");
    TEST_CHECK(read(f.root / raw) == "raw");
    TEST_CHECK(service.replay(service.snapshot().receipt));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().receipt);
    pump.prepare(service, {entry(f.root / "result-002"), entry(f.root / "result-003")});
    o.mode = luciole::BatchRenameMode::Replace;
    o.find = "result";
    o.replacement = "changed";
    pump.preview(service, o);
    pump.apply(service);
    TEST_CHECK(fs::is_symlink(f.root / "changed-002") && read(f.root / "changed-003") == "raw");
  }
  void replayGuards(Pump& pump) {
    using E = luciole::FileErrorKind;
    for (const auto kind : {E::Exists, E::Changed, E::Missing, E::NotDirectory}) {
      Fixture f;
      fs::create_directory(f.root / "folder");
      const auto folder = f.root / "folder";
      write(folder / "a", "A");
      write(folder / "b", "B");
      S service;
      pump.prepare(service, {entry(folder / "a"), entry(folder / "b")});
      pump.apply(service);
      const auto receipt = service.snapshot().receipt;
      TEST_CHECK(receipt && !service.replay({}));
      if (kind == E::Exists) {
        fs::create_symlink("missing", folder / "b");
      } else if (kind == E::Changed) {
        write(folder / "b_002", "changed");
      } else if (kind == E::Missing) {
        fs::remove(folder / "b_002");
      } else {
        fs::rename(folder, f.root / "held");
        fs::create_directory(folder);
        write(folder / "foreign", "foreign");
      }
      TEST_CHECK(service.replay(receipt));
      TEST_CHECK(!service.replay(receipt));
      pump.settle(service);
      TEST_CHECK(service.snapshot().phase == P::Error && service.snapshot().error);
      TEST_CHECK(service.snapshot().error->kind == (kind == E::Exists ? E::Exists : E::Changed));
      TEST_CHECK(!service.snapshot().receipt);
      TEST_CHECK(read((kind == E::NotDirectory ? f.root / "held" : folder) / "a_001") == "A");
      TEST_CHECK(!fs::exists(folder / "a"));
      if (kind == E::Exists) {
        TEST_CHECK(fs::read_symlink(folder / "b") == "missing");
        fs::remove(folder / "b");
        TEST_CHECK(service.replay(receipt));
        pump.settle(service);
        TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().receipt);
        TEST_CHECK(read(folder / "a") == "A" && read(folder / "b") == "B");
      } else if (kind == E::NotDirectory) {
        TEST_CHECK(read(folder / "foreign") == "foreign");
      }
    }
    Fixture f;
    write(f.root / "a", "A");
    write(f.root / "b", "B");
    S service;
    pump.prepare(service, {entry(f.root / "a"), entry(f.root / "b")});
    pump.apply(service);
    const auto receipt = service.snapshot().receipt;
    fs::rename(f.root / "a_001", f.root / "held");
    write(f.root / "a_001", "A");
    TEST_CHECK(service.replay(receipt));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Error && service.snapshot().error->kind == E::Changed);
    TEST_CHECK(read(f.root / "held") == "A" && read(f.root / "a_001") == "A" && read(f.root / "b_002") == "B");
  }

  void replayAliasesAndUnchanged(Pump& pump) {
    Fixture f;
    write(f.root / "a", "alias");
    fs::create_hard_link(f.root / "a", f.root / "b");
    fs::create_symlink("a", f.root / "link");
    TEST_CHECK(::mkfifo((f.root / "fifo").c_str(), 0600) == 0);
    S service;
    pump.prepare(service, {entry(f.root / "a"), entry(f.root / "b"), entry(f.root / "link"), entry(f.root / "fifo")});
    pump.apply(service);
    for (unsigned direction = 0; direction < 4; ++direction) {
      TEST_CHECK(service.snapshot().receipt && service.replay(service.snapshot().receipt));
      pump.settle(service);
      TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().receipt);
      const bool original = direction % 2 == 0;
      TEST_CHECK(fs::equivalent(f.root / (original ? "a" : "a_001"), f.root / (original ? "b" : "b_002")));
      TEST_CHECK(fs::read_symlink(f.root / (original ? "link" : "link_003")) == "a");
      TEST_CHECK(fs::is_fifo(f.root / (original ? "fifo" : "fifo_004")));
    }
    Fixture g;
    write(g.root / "change", "changed item");
    write(g.root / "keep", "unchanged item");
    pump.prepare(service, {entry(g.root / "change"), entry(g.root / "keep")});
    luciole::BatchRenameOptions options;
    options.mode = luciole::BatchRenameMode::Replace;
    options.find = "change";
    options.replacement = "new";
    pump.preview(service, options);
    pump.apply(service);
    write(g.root / "keep", "external change to an unchanged item");
    TEST_CHECK(service.replay(service.snapshot().receipt));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().results->size() == 1);
    TEST_CHECK(read(g.root / "change") == "changed item");
    TEST_CHECK(read(g.root / "keep") == "external change to an unchanged item");
  }
  void staleAndCancellation(Pump& pump) {
    Fixture f;
    write(f.root / "a", "A");
    write(f.root / "b", "B");
    S service;
    auto a = entry(f.root / "a");
    fs::rename(f.root / "a", f.root / "old-a");
    write(f.root / "a", "replacement");
    TEST_CHECK(service.prepare({a, entry(f.root / "b")}));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
    pump.prepare(service, {entry(f.root / "a"), entry(f.root / "b")});
    write(f.root / "a_001", "collision after preview");
    pump.apply(service);
    TEST_CHECK(service.snapshot().phase == P::Ready && !service.snapshot().canApply);
    TEST_CHECK(read(f.root / "a") == "replacement");
    fs::remove(f.root / "a_001");
    luciole::BatchRenameOptions o;
    for (int i = 0; i < 100; ++i) {
      o.pattern = "v" + std::to_string(i) + "-{number}";
      TEST_CHECK(service.preview(o));
    }
    pump.settle(service);
    TEST_CHECK(service.snapshot().rows->front().after == "v99-001");
    fs::rename(f.root / "a", f.root / "new-a");
    write(f.root / "a", "another");
    pump.apply(service);
    TEST_CHECK(service.snapshot().phase == P::Ready && !service.snapshot().canApply);
    TEST_CHECK(read(f.root / "a") == "another" && read(f.root / "new-a") == "replacement");
    pump.prepare(service, {entry(f.root / "a"), entry(f.root / "b")});
    service.cancel();
    TEST_CHECK(!service.apply());
    TEST_CHECK(service.prepare({entry(f.root / "a"), entry(f.root / "b")}));
    service.cancel();
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Cancelled);
    pump.prepare(service, {entry(f.root / "a"), entry(f.root / "b")});
    TEST_CHECK(service.apply());
    service.cancel();
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Finished);
    for (const auto& result : *service.snapshot().results) {
      TEST_CHECK((result.outcome == O::Cancelled || result.outcome == O::Renamed) && result.actual);
    }
    bool notified = false;
    {
      auto temporary = std::make_unique<S>();
      temporary->setChangedCallback([&] { notified = true; });
      TEST_CHECK(temporary->prepare({entry(f.root / "old-a"), entry(f.root / "new-a")}));
      temporary.reset();
      notified = false;
    }
    pump.until([] { return !g_main_context_pending(nullptr); });
    // Drain the queued worker through a subsequent task in the same pool.
    pump.prepare(service, {entry(f.root / "old-a"), entry(f.root / "new-a")});
    TEST_CHECK(!notified);
    fs::create_directory(f.root / "other");
    write(f.root / "other/c", "C");
    TEST_CHECK(service.prepare({entry(f.root / "old-a"), entry(f.root / "other/c")}));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
  }
  void parentReplacement(Pump& pump) {
    Fixture f;
    fs::create_directory(f.root / "folder");
    write(f.root / "folder/a", "A");
    write(f.root / "folder/b", "B");
    S service;
    pump.prepare(service, {entry(f.root / "folder/a"), entry(f.root / "folder/b")});
    fs::rename(f.root / "folder", f.root / "held");
    fs::create_directory(f.root / "folder");
    write(f.root / "folder/a", "foreign");
    pump.apply(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
    TEST_CHECK(read(f.root / "folder/a") == "foreign" && read(f.root / "held/a") == "A");
    fs::remove_all(f.root / "folder");
    fs::create_directory_symlink(f.root / "held", f.root / "folder");
    pump.prepare(service, {entry(f.root / "folder/a"), entry(f.root / "folder/b")});
    fs::remove(f.root / "folder");
    fs::create_directory(f.root / "folder");
    pump.apply(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
    TEST_CHECK(read(f.root / "held/b") == "B" && fs::is_empty(f.root / "folder"));
  }
  void cancellationAfterWork(Pump& pump) {
    for (const bool connected : {false, true}) {
      Fixture f;
      constexpr int count = 256;
      std::vector<luciole::DirectoryEntry> entries;
      for (int i = 0; i < count; ++i) {
        const auto path = f.root / ("file" + std::to_string(i));
        write(path, std::to_string(i));
        entries.push_back(entry(path));
      }
      S service;
      pump.prepare(service, entries);
      luciole::BatchRenameOptions o;
      o.pattern = connected ? "file{number}" : "new-{number}";
      o.digits = 1;
      pump.preview(service, o);
      TEST_CHECK(service.apply());
      pump.until([&] {
        if (!service.busy() || (!connected && service.completed() > 0)) {
          return true;
        }
        return connected && std::ranges::any_of(fs::directory_iterator(f.root), [](const auto& item) {
                 return item.path().filename().native().starts_with(".luciole-rename-");
               });
      });
      service.cancel();
      pump.settle(service);
      TEST_CHECK(service.snapshot().phase == P::Finished);
      for (int i = 0; i < count; ++i) {
        const auto& result = (*service.snapshot().results)[static_cast<std::size_t>(i)];
        TEST_CHECK(result.actual && read(*result.actual->nativePath()) == std::to_string(i));
        TEST_CHECK(result.outcome == O::Renamed || result.outcome == O::Cancelled);
        if (connected) {
          TEST_CHECK(result.outcome == service.snapshot().results->front().outcome);
        }
      }
      if (std::ranges::any_of(*service.snapshot().results, [](const auto& result) {
            return result.outcome != O::Renamed;
          })) {
        TEST_CHECK(!service.snapshot().receipt);
      }
      TEST_CHECK(std::distance(fs::directory_iterator(f.root), fs::directory_iterator{}) == count);
    }
  }
  void runtimeCollision(Pump& pump, bool replay) {
    Fixture f;
    constexpr int count = 256;
    std::vector<luciole::DirectoryEntry> entries;
    for (int i = 0; i < count; ++i) {
      const auto path = f.root / ("file" + std::to_string(i));
      write(path, std::to_string(i));
      entries.push_back(entry(path));
    }
    S service;
    pump.prepare(service, entries);
    luciole::BatchRenameOptions o;
    o.pattern = "file{number}";
    o.digits = 1;
    pump.preview(service, o);
    std::shared_ptr<const luciole::BatchRenameReceipt> receipt;
    if (replay) {
      pump.apply(service);
      receipt = service.snapshot().receipt;
      TEST_CHECK(receipt);
    }
    const auto* originalName = replay ? "file1" : "file0";
    const auto* targetName = replay ? "file0" : "file256";
    const int watch = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    TEST_CHECK(watch >= 0 && ::inotify_add_watch(watch, f.root.c_str(), IN_CREATE) >= 0);
    bool originalInserted = false, targetInserted = false;
    std::jthread rival([&](const std::stop_token& stop) {
      bool seen = false;
      std::array<char, 4096> events{};
      while (!stop.stop_requested()) {
        if (!seen) {
          seen = ::read(watch, events.data(), events.size()) > 0;
        }
        if (seen) {
          const int original = ::open((f.root / originalName).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
          if (original >= 0) {
            originalInserted = true;
            TEST_CHECK(::write(original, "foreign", 7) == 7);
            ::close(original);
          }
          if (originalInserted) {
            const int target = ::open((f.root / targetName).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (target >= 0) {
              targetInserted = true;
              TEST_CHECK(::write(target, "foreign", 7) == 7);
              ::close(target);
            }
            return;
          }
        }
        std::this_thread::yield();
      }
    });
    if (replay) {
      TEST_CHECK(service.replay(receipt));
      pump.settle(service);
    } else {
      pump.apply(service);
    }
    rival.request_stop();
    rival.join();
    ::close(watch);
    TEST_CHECK(service.snapshot().phase == P::Finished);
    for (int i = 0; i < count; ++i) {
      const auto& result = (*service.snapshot().results)[static_cast<std::size_t>(i)];
      TEST_CHECK(result.actual && read(*result.actual->nativePath()) == std::to_string(i));
    }
    if (originalInserted) {
      TEST_CHECK(read(f.root / originalName) == "foreign");
    }
    if (targetInserted) {
      TEST_CHECK(read(f.root / targetName) == "foreign");
      TEST_CHECK(service.snapshot().results->front().outcome == O::NeedsAttention);
      TEST_CHECK(service.snapshot().report.contains("Retained at"));
      TEST_CHECK(!service.snapshot().receipt);
    }
    std::println(
        "Runtime collision fixture: replay={}, original={}, destination={}", replay, originalInserted, targetInserted
    );
  }

  void replayCancellation(Pump& pump) {
    Fixture f;
    std::vector<luciole::DirectoryEntry> entries;
    constexpr int count = 256;
    for (int i = 0; i < count; ++i) {
      const auto path = f.root / ("item" + std::to_string(i));
      write(path, std::to_string(i));
      entries.push_back(entry(path));
    }
    S service;
    pump.prepare(service, std::move(entries));
    pump.apply(service);
    const auto receipt = service.snapshot().receipt;
    TEST_CHECK(receipt && service.replay(receipt));
    pump.until([&] { return !service.busy() || service.completed() > 0; });
    service.cancel();
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Finished && service.snapshot().results->size() == count);
    bool cancelled = false;
    for (int i = 0; i < count; ++i) {
      const auto& result = (*service.snapshot().results)[static_cast<std::size_t>(i)];
      TEST_CHECK(result.actual && read(*result.actual->nativePath()) == std::to_string(i));
      TEST_CHECK(result.outcome == O::Renamed || result.outcome == O::Cancelled);
      cancelled |= result.outcome == O::Cancelled;
    }
    TEST_CHECK(cancelled ? !service.snapshot().receipt : service.snapshot().receipt != nullptr);
    TEST_CHECK(std::distance(fs::directory_iterator(f.root), fs::directory_iterator{}) == count);
  }
  void caseOnlyRename(const fs::path& root) {
    const int dir = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int flags = 0;
    // musl declares ioctl(int, int, ...) and FS_IOC_GETFLAGS exceeds INT_MAX; the kernel reads the low 32 bits.
    TEST_CHECK(dir >= 0 && ::ioctl(dir, static_cast<int>(FS_IOC_GETFLAGS), &flags) == 0);
    flags |= FS_CASEFOLD_FL;
    TEST_CHECK(::ioctl(dir, FS_IOC_SETFLAGS, &flags) == 0 && ::close(dir) == 0);
    write(root / "a.txt", "A");
    write(root / "b.txt", "B");
    TEST_CHECK(fs::exists(root / "A.TXT"));
    Pump pump;
    S service;
    pump.prepare(service, {entry(root / "a.txt"), entry(root / "b.txt")});
    luciole::BatchRenameOptions o;
    o.mode = luciole::BatchRenameMode::Replace;
    o.find = "a";
    o.replacement = "A";
    pump.preview(service, o);
    TEST_CHECK(service.snapshot().canApply);
    pump.apply(service);
    TEST_CHECK(service.snapshot().phase == P::Finished);
    std::vector<std::string> names;
    for (const auto& item : fs::directory_iterator(root)) {
      names.push_back(item.path().filename());
    }
    std::ranges::sort(names);
    TEST_CHECK((names == std::vector<std::string>{"A.txt", "b.txt"}) && read(root / "A.txt") == "A");
  }
  // A casefold tmpfs is mountable only inside a private user namespace; 77 means unavailable.
  int caseOnlyRenameChild() {
    Fixture f;
    const auto uid = std::to_string(::getuid());
    const auto gid = std::to_string(::getgid());
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
      return 77;
    }
    write("/proc/self/setgroups", "deny");
    write("/proc/self/uid_map", "0 " + uid + " 1");
    write("/proc/self/gid_map", "0 " + gid + " 1");
    if (::mount("tmpfs", f.root.c_str(), "tmpfs", 0, "casefold") != 0) {
      return 77;
    }
    caseOnlyRename(f.root);
    TEST_CHECK(::umount2(f.root.c_str(), MNT_DETACH) == 0);
    return 0;
  }
  // Runs in a child forked before any threads exist, since unshare(CLONE_NEWUSER) needs a single thread.
  void caseInsensitiveFolder() {
    const pid_t child = ::fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
      std::exit(caseOnlyRenameChild());
    }
    int status = 0;
    TEST_CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status));
    if (WEXITSTATUS(status) == 77) {
      std::println("Case-insensitive folder checks unavailable: cannot mount a casefold tmpfs");
    } else {
      TEST_CHECK(WEXITSTATUS(status) == 0);
    }
  }
} // namespace
int main() {
  caseInsensitiveFolder();
  Pump pump;
  names();
  basicAndValidation(pump);
  cyclesAndKinds(pump);
  replayGuards(pump);
  replayAliasesAndUnchanged(pump);
  staleAndCancellation(pump);
  parentReplacement(pump);
  cancellationAfterWork(pump);
  replayCancellation(pump);
  runtimeCollision(pump, false);
  runtimeCollision(pump, true);
  std::println("batch rename checks passed");
}
