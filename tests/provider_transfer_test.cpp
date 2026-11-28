#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "operations/copy_receipt.h"
#include "operations/history_revisions.h"
#include "operations/provider_transfer.h"
#include "operations/transfer_estimate.h"
#include "operations/transfer_service.h"
#include "operations/transfer_staging.h"
#include "test_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace luciole;
  using enum TransferAction;
  using enum TransferConflictChoice;
  using enum TransferOutcome;
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream file(path);
    return {(std::istreambuf_iterator<char>(file)), {}};
  }
  void
  wait(TransferService& service, const std::function<TransferConflictChoice(const TransferConflict&)>& choose = {}) {
    const auto deadline = g_get_monotonic_time() + 20000000;
    while (service.busy()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (const auto conflict = service.conflict()) {
        TEST_CHECK(choose);
        TEST_CHECK(service.resolve(conflict->id, choose(*conflict)));
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
  }
  void mount(const Location& root) {
    // These CLI fixtures only target explicitly supplied private servers.
    const GioPtr<GMountOperation> operation(g_mount_operation_new());
    g_signal_connect(
        operation.get(), "ask-question",
        G_CALLBACK(+[](GMountOperation* operation, const char*, const char* const*, gpointer) {
          g_mount_operation_set_choice(operation, 0);
          g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
        }),
        nullptr
    );
    g_signal_connect(
        operation.get(), "ask-password",
        G_CALLBACK(+[](GMountOperation* operation, const char*, const char*, const char*, GAskPasswordFlags flags,
                       gpointer) {
          if ((flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0) {
            g_mount_operation_set_anonymous(operation, true);
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
          } else {
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
          }
        }),
        nullptr
    );
    std::optional<bool> mounted;
    g_file_mount_enclosing_volume(
        gioFile(root).get(), G_MOUNT_MOUNT_NONE, operation.get(), nullptr,
        [](GObject* file, GAsyncResult* result, gpointer data) {
          GError* error = nullptr;
          const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(file), result, &error);
          *static_cast<std::optional<bool>*>(data) =
              ok || (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED));
          if (error && !static_cast<std::optional<bool>*>(data)->value()) {
            std::println(stderr, "mount: {}", error->message);
          }
          g_clear_error(&error);
        },
        &mounted
    );
    const auto deadline = g_get_monotonic_time() + 15000000;
    while (!mounted) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(*mounted);
  }
  void disconnect(const Location& root) {
    GError* error = nullptr;
    const GioPtr<GMount> mounted(g_file_find_enclosing_mount(gioFile(root).get(), nullptr, &error));
    TEST_CHECK(mounted && !error);
    std::optional<bool> done;
    g_mount_unmount_with_operation(
        mounted.get(), G_MOUNT_UNMOUNT_FORCE, nullptr, nullptr,
        [](GObject* mount, GAsyncResult* result, gpointer data) {
          *static_cast<std::optional<bool>*>(data) =
              g_mount_unmount_with_operation_finish(G_MOUNT(mount), result, nullptr);
        },
        &done
    );
    const auto deadline = g_get_monotonic_time() + 5000000;
    while (!done) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(*done);
  }
  struct DownloadResult {
    std::vector<TransferResult> results;
    std::shared_ptr<const CopyReceipt> receipt;
  };
  DownloadResult download(
      const std::vector<Location>& sources, const Location& destination,
      const ProviderTransfer::Choose& choose = [](const auto& conflict) {
        return conflict.canMerge ? Merge : TransferConflictChoice::Replace;
      }
  ) {
    const TransferRequest request{sources, destination, TransferAction::Copy};
    if (std::ranges::any_of(sources, [](const auto& source) { return !source.isNative(); })) {
      TransferService service;
      TEST_CHECK(service.start(request));
      wait(service, choose);
      return {service.results(), service.takeCopyReceipt()};
    }
    // CI exercises the same provider publication path against ordinary temporary files.
    ProviderTransfer provider(request);
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::size_t> completed{0};
    transfer::Work work({}, bytes, {});
    work.history = std::make_shared<HistoryRevisions>();
    const auto retention = checkedFd(::open(destination.nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    std::size_t remaining = 16384;
    std::vector<std::shared_ptr<const CopyEffect>> effects, prepared;
    work.checkpointCopy = [&](const auto& target, bool replacing) {
      if (replacing) {
        effects = CopyReceipt::checkpoint(effects, target, TransferAction::Copy, {});
      }
    };
    work.prepareCopy = [&](std::size_t, const auto& target, int parent, int stage, bool replacing) {
      auto effect = CopyEffect::prepare(target, parent, stage, replacing, remaining, {}, work.history);
      prepared.push_back(effect);
      return effect;
    };
    work.recordCopy = [&](const auto& effect, int stage) {
      effects.push_back(effect->published(stage, retention.get()));
    };
    auto results = provider.run({}, bytes, completed, choose, &work);
    const auto count = provider.completedCopies();
    auto receipt = count && *count > 0 && *count == effects.size()
        ? CopyReceipt::capture(effects, TransferAction::Copy, {})
        : nullptr;
    return {std::move(results), std::move(receipt)};
  }
  std::shared_ptr<const CopyReceipt> reverse(const std::shared_ptr<const CopyReceipt>& receipt) {
    TEST_CHECK(receipt);
    TransferService service;
    TEST_CHECK(service.replayCopy(receipt));
    wait(service);
    for (const auto& result : service.results()) {
      if (result.error) {
        std::println(stderr, "replay: {}", result.error->message);
      }
      TEST_CHECK(!result.error);
    }
    TEST_CHECK(service.copyReceipt());
    return service.takeCopyReceipt();
  }
  void drainHistory() {
    const auto deadline = g_get_monotonic_time() + 10000000;
    while (CopyReceipt::cleanupPending()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(CopyReceipt::takeCleanupErrors().empty());
  }
  void downloadFailureProofs(const fs::path& root) {
    for (int mode = 0; mode < 6; ++mode) {
      const auto fixture = root / std::to_string(mode);
      fs::create_directories(fixture / "source");
      fs::create_directories(fixture / "destination");
      write(fixture / "source/item", "incoming");
      if (mode != 5) {
        write(fixture / "destination/item", "previous");
      }
      const auto source = Location::fromPath((fixture / "source/item").native());
      const auto destination = Location::fromPath((fixture / "destination").native());
      ProviderTransfer provider({{source}, destination, TransferAction::Copy});
      std::atomic<std::uint64_t> bytes{0};
      std::atomic<std::size_t> completed{0};
      transfer::Work work({}, bytes, {});
      work.history = std::make_shared<HistoryRevisions>();
      std::size_t remaining = 16384;
      if (mode != 3) {
        work.prepareCopy = [&](std::size_t, const auto& target, int parent, int stage, bool replacing) {
          auto proof = CopyEffect::prepare(target, parent, stage, replacing, remaining, {}, work.history);
          if (mode == 0) {
            fs::rename(fixture / "destination", fixture / "held");
            fs::create_directory(fixture / "destination");
          } else if (mode == 1) {
            write(fixture / "destination/item", "changed later version");
          } else if (mode == 4) {
            work.token.cancel();
          } else if (mode == 5) {
            write(fixture / "destination/item", "new occupant");
          }
          return proof;
        };
        work.recordCopy = [](const auto&, int) {
          throwFileError(FileErrorKind::Other, "Injected retention handoff failure.");
        };
      }
      const auto results = provider.run(
          work.token, bytes, completed, [](const auto&) { return TransferConflictChoice::Replace; }, &work
      );
      const auto& result = results.front();
      if (mode == 0) {
        TEST_CHECK(result.outcome == Failed && result.error->kind == FileErrorKind::Changed);
        TEST_CHECK(fs::is_empty(fixture / "destination") && read(fixture / "held/item") == "previous");
      } else if (mode == 1) {
        TEST_CHECK(result.outcome == Failed && result.error->kind == FileErrorKind::Changed);
        TEST_CHECK(read(fixture / "destination/item") == "changed later version");
      } else if (mode == 2) {
        TEST_CHECK(result.outcome == Partial && !provider.completedCopies());
        TEST_CHECK(read(fixture / "destination/item") == "incoming");
        bool retained = false;
        for (const auto& item : fs::directory_iterator(fixture / "destination")) {
          if (item.path().filename().native().starts_with(".luciole-transfer-")) {
            TEST_CHECK(read(item.path() / "item") == "previous");
            TEST_CHECK(result.error->message.contains(item.path().filename().native()));
            retained = true;
          }
        }
        TEST_CHECK(retained);
      } else if (mode == 3) {
        TEST_CHECK(result.outcome == Copied && !result.error);
        TEST_CHECK(read(fixture / "destination/item") == "incoming");
        TEST_CHECK(std::distance(fs::directory_iterator(fixture / "destination"), fs::directory_iterator{}) == 1);
      } else if (mode == 4) {
        TEST_CHECK(result.outcome == Cancelled && result.error->kind == FileErrorKind::Cancelled);
        TEST_CHECK(read(fixture / "destination/item") == "previous");
      } else {
        TEST_CHECK(result.outcome == Failed && result.error->kind == FileErrorKind::Exists);
        TEST_CHECK(read(fixture / "destination/item") == "new occupant");
      }
      TEST_CHECK(read(fixture / "source/item") == "incoming");
    }
    fs::remove_all(root);
    drainHistory();
  }
  void providerWriteback(const fs::path& root) {
    fs::create_directories(root / "source");
    fs::create_directory(root / "destination");
    const auto source = root / "source/file", target = root / "destination/file";
    const auto from = Location::fromPath(source.native()), to = Location::fromPath(target.parent_path().native());
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::size_t> count{0};
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      write(source, "incoming");
      write(target, "old");
      ProviderTransfer job({{from}, to, action});
      transfer::Work work({}, bytes, {});
      unsigned calls = 0;
      work.writeback.ops.removable = [](int, dev_t) { return true; };
      work.writeback.ops.sync = [&](int) {
        TEST_CHECK(read(source) == "incoming");
        if (++calls == 2) {
          TEST_CHECK(read(target) == "incoming");
          throwErrno(EIO);
        }
        TEST_CHECK(read(target) == "old");
      };
      const auto choose = [](const auto&) { return TransferConflictChoice::Replace; };
      const auto results = job.run(work.token, bytes, count, choose, &work);
      TEST_CHECK(calls == 2 && results.front().outcome == TransferOutcome::Partial);
      TEST_CHECK(read(source) == "incoming" && !job.retryable() && !job.completedCopies());
      bool retained = false;
      for (const auto& entry : fs::directory_iterator(target.parent_path())) {
        if (results.front().error->message.contains(entry.path().filename().native()) && entry.is_directory()) {
          retained |= read(entry.path() / (action == TransferAction::Move ? "previous" : "item")) == "old";
        }
      }
      TEST_CHECK(retained);
      TEST_CHECK(
          job.run(
                 work.token, bytes, count,
                 [](const auto&) {
                   TEST_CHECK(false);
                   return Cancel;
                 },
                 &work
          )
              .front()
              .outcome
          == Partial
      );
      TEST_CHECK(calls == 2);
    }

    // Retry only explicitly retryable failures: a cancelled sibling in a provider move must never be moved on retry.
    {
      const auto sub = root / "retry-cancelled";
      fs::create_directories(sub / "source");
      fs::create_directories(sub / "destination");
      write(sub / "source/failed", "new-a");
      write(sub / "destination/failed", "old-a");
      write(sub / "source/cancelled", "keep-b");
      write(sub / "destination/cancelled", "old-b");
      ProviderTransfer job(
          {{Location::fromPath((sub / "source/failed").native()),
            Location::fromPath((sub / "source/cancelled").native())},
           Location::fromPath((sub / "destination").native()),
           TransferAction::Move}
      );
      transfer::Work first({}, bytes, {});
      first.writeback.ops.removable = [](int, dev_t) { return true; };
      first.writeback.ops.sync = [](int) { throwErrno(EIO); };
      const auto initial = job.run(
          first.token, bytes, count,
          [](const TransferConflict& conflict) {
            return conflict.name == "failed" ? TransferConflictChoice::Replace : TransferConflictChoice::Cancel;
          },
          &first
      );
      TEST_CHECK(initial[0].outcome == Failed && initial[0].retryable);
      TEST_CHECK(initial[1].outcome == Cancelled && !initial[1].retryable);
      TEST_CHECK(job.retryable() == 1 && job.hasRetryableSource(0) && !job.hasRetryableSource(1));
      transfer::Work retryWork({}, bytes, {});
      retryWork.writeback.ops.removable = [](int, dev_t) { return true; };
      retryWork.writeback.ops.sync = [](int) {};
      const auto retried = job.run(
          retryWork.token, bytes, count,
          [](const TransferConflict& conflict) {
            TEST_CHECK(conflict.name == "failed");
            return TransferConflictChoice::Replace;
          },
          &retryWork
      );
      TEST_CHECK(retried[0].outcome == Moved && !fs::exists(sub / "source/failed"));
      TEST_CHECK(read(sub / "destination/failed") == "new-a");
      TEST_CHECK(retried[1].outcome == Cancelled && read(sub / "source/cancelled") == "keep-b");
      TEST_CHECK(read(sub / "destination/cancelled") == "old-b");
    }

    // Mixed-job retry must not re-enter a terminal Partial merged folder or wipe its writeback error.
    {
      const auto sub = root / "retry-partial-merge";
      fs::create_directories(sub / "source/tree");
      fs::create_directories(sub / "destination/tree");
      write(sub / "source/tree/leaf", "new-leaf");
      write(sub / "destination/tree/leaf", "old-leaf");
      write(sub / "source/other", "retry-other");
      ProviderTransfer job(
          {{Location::fromPath((sub / "source/tree").native()), Location::fromPath((sub / "source/other").native())},
           Location::fromPath((sub / "destination").native()),
           TransferAction::Copy}
      );
      transfer::Work first({}, bytes, {});
      unsigned calls = 0;
      first.writeback.ops.removable = [](int, dev_t) { return true; };
      first.writeback.ops.sync = [&](int) {
        if (++calls == 2) {
          throwErrno(ENOSPC);
        }
      };
      const auto initial = job.run(
          first.token, bytes, count,
          [](const TransferConflict& conflict) {
            return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace;
          },
          &first
      );
      TEST_CHECK(initial[0].outcome == Partial && initial[0].error && initial[0].error->kind == FileErrorKind::NoSpace);
      TEST_CHECK(initial[1].outcome == Failed && initial[1].retryable);
      TEST_CHECK(job.retryable() == 1 && !job.hasRetryableSource(0) && job.hasRetryableSource(1));
      const auto preservedError = initial[0].error->message;
      // Remove the merged source folder so any accidental re-entry would fail or overwrite the error.
      fs::remove_all(sub / "source/tree");
      transfer::Work retryWork({}, bytes, {});
      retryWork.writeback.ops.removable = [](int, dev_t) { return true; };
      retryWork.writeback.ops.sync = [](int) {};
      const auto retried = job.run(
          retryWork.token, bytes, count,
          [](const TransferConflict&) {
            TEST_CHECK(false);
            return TransferConflictChoice::Cancel;
          },
          &retryWork
      );
      TEST_CHECK(retried[0].outcome == Partial && retried[0].error && retried[0].error->message == preservedError);
      TEST_CHECK(retried[0].error->kind == FileErrorKind::NoSpace);
      TEST_CHECK(retried[1].outcome == Copied && read(sub / "destination/other") == "retry-other");
    }

    // A deferred writeback failure on an already-Partial Copy merge surfaces the I/O error on the parent.
    {
      const auto sub = root / "partial-merge-deferred";
      fs::create_directories(sub / "source/tree");
      fs::create_directories(sub / "destination/tree");
      write(sub / "source/tree/a", "copied");
      write(sub / "source/tree/b", "skipped");
      write(sub / "destination/tree/b", "existing");
      ProviderTransfer job(
          {{Location::fromPath((sub / "source/tree").native())},
           Location::fromPath((sub / "destination").native()),
           TransferAction::Copy}
      );
      transfer::Work work({}, bytes, {});
      work.writeback.ops.removable = [](int, dev_t) { return true; };
      work.writeback.ops.sync = [](int) { throwErrno(EIO); };
      const auto results = job.run(
          work.token, bytes, count,
          [](const TransferConflict& conflict) {
            return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Skip;
          },
          &work
      );
      TEST_CHECK(results[0].outcome == Partial && results[0].error);
      TEST_CHECK(results[0].children[0].outcome == Partial && results[0].children[0].error);
      TEST_CHECK(results[0].error->message.contains(results[0].children[0].error->message));
    }

    // Fixed-disk provider downloads and replacing links pay no post-exchange or terminal flush.
    {
      const auto sub = root / "fixed-disk-counts";
      fs::create_directories(sub / "source");
      fs::create_directories(sub / "destination");
      write(sub / "source/plain", "plain");
      write(sub / "source/rep", "new");
      write(sub / "destination/rep", "old");
      write(sub / "source/lnk", "link-target");
      write(sub / "destination/lnk", "old-link");

      unsigned calls = 0;
      const auto useFixedDisk = [&](transfer::Work& w) {
        w.writeback.ops.removable = [](int, dev_t) { return false; };
        w.writeback.ops.sync = [&](int) { ++calls; };
      };
      {
        transfer::Work work({}, bytes, {});
        useFixedDisk(work);
        ProviderTransfer plain(
            {{Location::fromPath((sub / "source/plain").native())},
             Location::fromPath((sub / "destination").native()),
             TransferAction::Copy}
        );
        const auto res = plain.run(work.token, bytes, count, [](const auto&) { return Cancel; }, &work);
        TEST_CHECK(res[0].outcome == Copied && calls == 0);
      }
      {
        transfer::Work work({}, bytes, {});
        useFixedDisk(work);
        ProviderTransfer replacing(
            {{Location::fromPath((sub / "source/rep").native())},
             Location::fromPath((sub / "destination").native()),
             TransferAction::Copy}
        );
        const auto res =
            replacing.run(work.token, bytes, count, [](const auto&) { return TransferConflictChoice::Replace; }, &work);
        TEST_CHECK(res[0].outcome == Copied && calls == 1);
      }
      {
        calls = 0;
        transfer::Work work({}, bytes, {});
        useFixedDisk(work);
        ProviderTransfer linkReplace(
            {{Location::fromPath((sub / "source/lnk").native())},
             Location::fromPath((sub / "destination").native()),
             TransferAction::Link}
        );
        const auto res = linkReplace.run(
            work.token, bytes, count, [](const auto&) { return TransferConflictChoice::Replace; }, &work
        );
        TEST_CHECK(res[0].outcome == Linked && calls == 1);
      }
    }
  }
  // A replacing move whose source removal fails keeps the displaced destination instead of deleting it.
  void moveSourceRemovalFailure(const fs::path& root) {
    if (geteuid() == 0) {
      return;
    }
    fs::create_directories(root / "source");
    fs::create_directories(root / "destination");
    write(root / "source/item", "incoming");
    write(root / "destination/item", "previous");
    fs::permissions(root / "source", fs::perms::owner_read | fs::perms::owner_exec);
    ProviderTransfer provider(
        {{Location::fromPath((root / "source/item").native())},
         Location::fromPath((root / "destination").native()),
         TransferAction::Move}
    );
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::size_t> completed{0};
    const auto results =
        provider.run({}, bytes, completed, [](const auto&) { return TransferConflictChoice::Replace; });
    fs::permissions(root / "source", fs::perms::owner_all);
    TEST_CHECK(results.front().outcome == Partial && read(root / "destination/item") == "incoming");
    TEST_CHECK(read(root / "source/item") == "incoming");
    bool retained = false;
    for (const auto& item : fs::directory_iterator(root / "destination")) {
      if (item.path().filename().native().starts_with(".luciole-transfer-")) {
        retained = read(item.path() / "previous") == "previous";
        TEST_CHECK(results.front().error->message.contains(item.path().filename().native()));
      }
    }
    TEST_CHECK(retained);
    fs::remove_all(root);
  }

  // Transfers measure local, phone and camera sources before copying; other providers stay unmeasured.
  void measurableSources() {
    const auto uri = [](const char* text) { return Location::fromUri(text); };
    TEST_CHECK(luciole::measurableSource(Location::fromPath("/tmp/photo.jpg")));
    TEST_CHECK(luciole::measurableSource(uri("mtp://Xiaomi_Redmi_Note_11_c4f4af9c/Internal%20shared%20storage/DCIM")));
    TEST_CHECK(luciole::measurableSource(uri("gphoto2://Canon_EOS/store_00010001/DCIM/100CANON/IMG_0001.JPG")));
    TEST_CHECK(!luciole::measurableSource(uri("sftp://host/home/me/file.txt")));
    // The device itself is not an item on it.
    TEST_CHECK(!luciole::measurableSource(uri("mtp://Xiaomi_Redmi_Note_11_c4f4af9c/")));
  }

  // Provider downloads report each verified regular file, so progress counts phone copies file by file. Copy
  // publishes through the download path and Move through staging; both must count.
  void downloadsCountFiles(const fs::path& root) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      const auto base = root / (action == TransferAction::Copy ? "copy" : "move");
      fs::create_directories(base / "remote/tree/nested");
      fs::create_directories(base / "destination");
      write(base / "remote/tree/one.txt", "one");
      write(base / "remote/tree/nested/two.txt", "two");
      write(base / "remote/single.txt", "single");
      const auto remote = Location::fromPath((base / "remote").native());
      ProviderTransfer provider(
          {{remote.child("tree"), remote.child("single.txt")},
           Location::fromPath((base / "destination").native()),
           action}
      );
      std::atomic<std::uint64_t> bytes{0};
      std::atomic<std::size_t> completed{0};
      transfer::Work work({}, bytes, {});
      std::size_t files = 0;
      work.fileObserver = [&files] { ++files; };
      const auto results = provider.run(
          {}, bytes, completed, [](const TransferConflict&) { return TransferConflictChoice::Cancel; }, &work
      );
      TEST_CHECK(results.size() == 2 && files == 3);
      TEST_CHECK(read(base / "destination/tree/nested/two.txt") == "two");
      TEST_CHECK(action == TransferAction::Copy || !fs::exists(base / "remote/tree"));
    }
  }

  // The GIO walk phones and cameras use, run here on local files through GIO: it counts regular files and their
  // bytes, and marks anything a provider copy would not count as done, or could not reach, as partial.
  void gioEstimates(const fs::path& root) {
    fs::create_directories(root / "tree/nested");
    write(root / "tree/one.txt", "abc");
    write(root / "tree/nested/two.txt", "hello");
    write(root / "tree/nested/empty.txt", "");
    const auto tree = Location::fromPath((root / "tree").native());
    int updates = 0;
    const auto plain = luciole::estimateGioSource(tree, {}, [&updates](const auto&) { ++updates; });
    TEST_CHECK(plain.files == 3 && plain.bytes == 8 && !plain.partial && updates > 0);
    const auto single = luciole::estimateGioSource(tree.child("one.txt"), {}, {});
    TEST_CHECK(single.files == 1 && single.bytes == 3 && !single.partial);
    fs::create_symlink("one.txt", root / "tree/link");
    const auto linked = luciole::estimateGioSource(tree, {}, {});
    TEST_CHECK(linked.files == 3 && linked.bytes == 8 && linked.partial);
    TEST_CHECK(luciole::estimateGioSource(tree.child("missing"), {}, {}).partial);
    CancelToken cancelled;
    cancelled.cancel();
    const auto stopped = luciole::estimateGioSource(tree, cancelled, {});
    TEST_CHECK(stopped.partial && stopped.files == 0);
  }

  void downloadHistory(const fs::path& root, const Location& remote, const fs::path& backend) {
    fs::create_directories(root / "destination");
    fs::create_directories(backend / "tree/nested");
    write(backend / "tree/nested/file.txt", "downloaded tree");
    write(backend / "café.txt", "downloaded file");
    const auto target = Location::fromPath((root / "destination").native());
    {
      auto done = download({remote.child("tree"), remote.child("café.txt")}, target);
      TEST_CHECK(done.receipt && done.receipt->size() == 2);
      TEST_CHECK(read(root / "destination/tree/nested/file.txt") == "downloaded tree");
      // Reversal never reads or recreates the original remote source.
      fs::rename(backend / "tree", backend / "tree-offline");
      fs::rename(backend / "café.txt", backend / "café-offline.txt");
      auto redo = reverse(done.receipt);
      TEST_CHECK(!fs::exists(root / "destination/tree") && !fs::exists(root / "destination/café.txt"));
      auto undo = reverse(redo);
      TEST_CHECK(read(root / "destination/tree/nested/file.txt") == "downloaded tree");
      TEST_CHECK(read(root / "destination/café.txt") == "downloaded file");
      fs::rename(backend / "tree-offline", backend / "tree");
      fs::rename(backend / "café-offline.txt", backend / "café.txt");
    }
    drainHistory();
    {
      fs::remove_all(root / "destination/tree");
      write(root / "destination/tree", "old file replaced by folder");
      struct stat old{};
      TEST_CHECK(::lstat((root / "destination/tree").c_str(), &old) == 0);
      auto done = download({remote.child("tree")}, target);
      TEST_CHECK(done.receipt);
      auto redo = reverse(done.receipt);
      struct stat restored{};
      TEST_CHECK(::lstat((root / "destination/tree").c_str(), &restored) == 0);
      TEST_CHECK(old.st_ino == restored.st_ino && read(root / "destination/tree") == "old file replaced by folder");
      auto undo = reverse(redo);
      TEST_CHECK(read(root / "destination/tree/nested/file.txt") == "downloaded tree");
    }
    drainHistory();
    {
      write(root / "destination/tree/nested/file.txt", "merge predecessor");
      write(root / "destination/tree/keep.txt", "destination-only");
      write(backend / "tree/new.txt", "new merge child");
      auto done = download({remote.child("tree")}, target);
      TEST_CHECK(done.receipt && done.receipt->size() == 2);
      write(root / "destination/tree/keep.txt", "later destination-only edit");
      auto redo = reverse(done.receipt);
      TEST_CHECK(read(root / "destination/tree/nested/file.txt") == "merge predecessor");
      TEST_CHECK(!fs::exists(root / "destination/tree/new.txt"));
      TEST_CHECK(read(root / "destination/tree/keep.txt") == "later destination-only edit");
      auto undo = reverse(redo);
      TEST_CHECK(read(root / "destination/tree/nested/file.txt") == "downloaded tree");
      TEST_CHECK(read(root / "destination/tree/new.txt") == "new merge child");
    }
    drainHistory();
    {
      auto skipped = download({remote.child("café.txt")}, target, [](const auto&) { return Skip; });
      TEST_CHECK(!skipped.receipt && skipped.results.front().outcome == Skipped);
      auto numbered = download({remote.child("café.txt")}, target, [](const auto&) { return KeepBoth; });
      TEST_CHECK(numbered.receipt && read(root / "destination/café (1).txt") == "downloaded file");
      auto redo = reverse(numbered.receipt);
      write(root / "destination/café (1).txt", "unrelated later file");
      TransferService refusal;
      TEST_CHECK(refusal.replayCopy(redo));
      wait(refusal);
      TEST_CHECK(refusal.results().front().error->kind == FileErrorKind::Exists);
      TEST_CHECK(read(root / "destination/café (1).txt") == "unrelated later file");
      fs::remove(root / "destination/café (1).txt");
      auto undo = reverse(redo);
      write(root / "destination/café (1).txt", "edited download");
      TEST_CHECK(refusal.replayCopy(undo));
      wait(refusal);
      TEST_CHECK(refusal.historyRetired() && refusal.results().front().error->kind == FileErrorKind::Changed);
      TEST_CHECK(read(root / "destination/café (1).txt") == "edited download");
    }
    drainHistory();
    {
      fs::remove(root / "destination/café.txt");
      auto done = download({remote.child("café.txt")}, target);
      TEST_CHECK(done.receipt);
      const CancelToken cancelled;
      cancelled.cancel();
      std::atomic<std::size_t> completed{0};
      const auto reply = done.receipt->reverse(cancelled, completed);
      TEST_CHECK(!reply.receipt && reply.results.front().error->kind == FileErrorKind::Cancelled);
      TEST_CHECK(read(root / "destination/café.txt") == "downloaded file");
      auto redo = reverse(done.receipt);
      auto undo = reverse(redo);
    }
    drainHistory();
    for (const auto& item : fs::recursive_directory_iterator(root)) {
      TEST_CHECK(!item.path().filename().native().starts_with(".luciole-"));
    }
  }

  void portable(
      const fs::path& root, const Location& remote, const fs::path& backend, const Location& readOnly,
      const fs::path& readOnlyBackend
  ) {
    fs::create_directories(root / "source/tree/nested");
    fs::create_directories(root / "destination");
    const auto source = Location::fromPath((root / "source").native()),
               local = Location::fromPath((root / "destination").native());
    write(root / "source/tree/nested/file.txt", "nested portable bytes");
    write(root / "source/tree/.hidden", "hidden portable bytes");
    write(root / "source/café space.txt", "unicode portable bytes");
    const auto transfer = [&](std::vector<Location> sources, const Location& destination, TransferAction action,
                              const std::function<TransferConflictChoice(const TransferConflict&)>& answer = {}) {
      TransferService service;
      TEST_CHECK(service.start({std::move(sources), destination, action}));
      wait(service, answer);
      for (const auto& result : service.results()) {
        if (result.error) {
          std::println(stderr, "result: {}", result.error->message);
        }
      }
      return service.results();
    };
    auto result = transfer({source.child("tree"), source.child("café space.txt")}, remote, TransferAction::Copy);
    TEST_CHECK(result.size() == 2 && result[0].outcome == Copied && result[1].outcome == Copied);
    TEST_CHECK(
        read(backend / "tree/nested/file.txt") == "nested portable bytes"
        && read(backend / "tree/.hidden") == "hidden portable bytes"
    );
    TEST_CHECK(read(backend / "café space.txt") == "unicode portable bytes");
    result = transfer({remote.child("tree")}, local, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Moved
        && !fs::exists(backend / "tree")
        && read(root / "destination/tree/nested/file.txt") == "nested portable bytes"
    );
    fs::create_directories(backend / "tree/nested");
    write(backend / "tree/keep.txt", "destination-only");
    write(backend / "tree/nested/file.txt", "old");
    result = transfer({source.child("tree")}, remote, TransferAction::Copy, [](const auto& conflict) {
      return conflict.canMerge ? Merge : TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        result.front().outcome == Copied
        && read(backend / "tree/keep.txt") == "destination-only"
        && read(backend / "tree/nested/file.txt") == "nested portable bytes"
    );
    result =
        transfer({source.child("café space.txt")}, remote, TransferAction::Copy, [](const auto&) { return KeepBoth; });
    TEST_CHECK(result.front().outcome == Copied && read(backend / "café space (1).txt") == "unicode portable bytes");
    result = transfer({source.child("café space.txt")}, remote, TransferAction::Move, [](const auto&) { return Skip; });
    TEST_CHECK(result.front().outcome == Skipped && fs::exists(root / "source/café space.txt"));
    // Source changes while a conflict is pending must not replace the destination.
    result = transfer({source.child("café space.txt")}, remote, TransferAction::Move, [&](const auto&) {
      write(root / "source/café space.txt", "changed while waiting with another length");
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        result.front().outcome == Failed
        && result.front().error->kind == FileErrorKind::Changed
        && read(backend / "café space.txt") == "unicode portable bytes"
    );
    // New names upload and move in the opposite direction as well.
    write(root / "source/upload.txt", "move upload");
    result = transfer({source.child("upload.txt")}, remote, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Moved
        && !fs::exists(root / "source/upload.txt")
        && read(backend / "upload.txt") == "move upload"
    );
    // A protocol unable to represent local symlinks must preserve the whole source tree.
    fs::create_directory(root / "source/links");
    write(root / "outside.txt", "outside canary");
    fs::create_symlink(root / "outside.txt", root / "source/links/link");
    result = transfer({source.child("links")}, remote, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Failed
        && fs::is_symlink(root / "source/links/link")
        && !fs::exists(backend / "links")
        && read(root / "outside.txt") == "outside canary"
    );
    const std::string raw = std::string("raw-") + static_cast<char>(0xff) + ".txt";
    write(root / "source" / raw, "raw original");
    result = transfer({source.child(raw)}, remote, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Failed
        && result.front().error->kind == FileErrorKind::InvalidArgument
        && fs::exists(root / "source" / raw)
    );
    if (remote.uri().starts_with("smb:")) {
      for (const auto* name : {"notes:stream", "CON.txt", "trailing.", "bad?.txt"}) {
        write(root / "source" / name, "unsupported name");
        result = transfer({source.child(name)}, remote, TransferAction::Move);
        TEST_CHECK(
            result.front().outcome == Failed
            && result.front().error->kind == FileErrorKind::InvalidArgument
            && fs::exists(root / "source" / name)
        );
      }
      fs::create_directory(backend / "CaseFolder");
      write(backend / "CaseFolder/kept.txt", "case canary");
      result = transfer({remote.child("CaseFolder/kept.txt")}, remote.child("casefolder"), TransferAction::Move);
      TEST_CHECK(result.front().outcome == Skipped && read(backend / "CaseFolder/kept.txt") == "case canary");
      result = transfer({remote.child("CaseFolder")}, remote.child("casefolder"), TransferAction::Copy);
      TEST_CHECK(
          result.front().outcome == Failed
          && result.front().error->kind == FileErrorKind::WouldRecurse
          && read(backend / "CaseFolder/kept.txt") == "case canary"
      );
    }
    const auto readName = "readonly-" + *remote.basename() + ".txt";
    write(readOnlyBackend / readName, "readonly original");
    result = transfer({readOnly.child(readName)}, local, TransferAction::Copy);
    TEST_CHECK(result.front().outcome == Copied && read(root / "destination" / readName) == "readonly original");
    result = transfer({readOnly.child(readName)}, local, TransferAction::Move, [](const auto&) {
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        result.front().outcome == Partial
        && fs::exists(readOnlyBackend / readName)
        && read(root / "destination" / readName) == "readonly original"
    );
    result = transfer({source.child("café space.txt")}, readOnly, TransferAction::Copy);
    TEST_CHECK(result.front().outcome == Failed && !fs::exists(readOnlyBackend / "café space.txt"));
    // Cancellation while streaming leaves the original and no published partial file.
    {
      std::ofstream large(root / "source/large.bin");
      large.seekp(128 * 1024 * 1024 - 1);
      large.put('X');
    }
    TransferService streaming;
    TEST_CHECK(streaming.start({{source.child("large.bin")}, remote, TransferAction::Copy}));
    const auto deadline = g_get_monotonic_time() + 15000000;
    while (streaming.busy() && streaming.bytes() == 0) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(streaming.busy());
    streaming.cancel();
    wait(streaming);
    TEST_CHECK(
        streaming.results().front().outcome == Cancelled
        && !fs::exists(backend / "large.bin")
        && fs::exists(root / "source/large.bin")
    );
    fs::remove(root / "source/large.bin");
    for (const auto& item : fs::recursive_directory_iterator(backend)) {
      TEST_CHECK(!item.path().filename().native().starts_with(".luciole-transfer-"));
    }
    write(root / "destination/upload.txt", "destination survives disconnect");
    TransferService disconnected;
    TEST_CHECK(disconnected.start({{remote.child("upload.txt")}, local, TransferAction::Move}));
    wait(disconnected, [&](const auto&) {
      disconnect(remote);
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        disconnected.results().front().outcome == Failed
        && read(root / "destination/upload.txt") == "destination survives disconnect"
        && read(backend / "upload.txt") == "move upload"
    );
  }
  void run(const fs::path& root, const Location& remote, const fs::path& backend, bool provider) {
    fs::create_directories(root / "source/tree/nested");
    fs::create_directories(root / "destination");
    write(root / "source/tree/nested/file.txt", "nested bytes");
    write(root / "source/tree/.hidden", "hidden bytes");
    fs::create_symlink("nested/file.txt", root / "source/tree/link");
    write(root / "source/café space.txt", "unicode bytes");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff) + ".txt";
    write(root / "source" / raw, "raw bytes");
    const auto native = Location::fromPath((root / "source").native());
    const auto local = Location::fromPath((root / "destination").native());
    auto transfer = [&](std::vector<Location> sources, const Location& destination, TransferAction action,
                        TransferConflictChoice choice = TransferConflictChoice::Replace) {
      if (provider) {
        TransferService service;
        TEST_CHECK(service.start({std::move(sources), destination, action}));
        wait(service, [choice](const auto&) { return choice; });
        return service.results();
      }
      ProviderTransfer service({std::move(sources), destination, action});
      std::atomic<std::uint64_t> bytes = 0;
      std::atomic<std::size_t> count = 0;
      return service.run(CancelToken{}, bytes, count, [choice](const auto& conflict) {
        TEST_CHECK(conflict.sourceModified && conflict.destinationModified);
        return choice;
      });
    };
    auto results = transfer(
        {native.child("tree"), native.child("café space.txt"), native.child(raw)}, remote, TransferAction::Copy
    );
    TEST_CHECK(results.size() == 3);
    for (const auto& result : results) {
      if (result.error) {
        std::println(stderr, "copy: {}", result.error->message);
      }
      TEST_CHECK(result.outcome == Copied);
    }
    TEST_CHECK(
        read(backend / "tree/nested/file.txt") == "nested bytes" && read(backend / "tree/.hidden") == "hidden bytes"
    );
    TEST_CHECK(fs::is_symlink(backend / "tree/link") && fs::read_symlink(backend / "tree/link") == "nested/file.txt");
    TEST_CHECK(read(backend / "café space.txt") == "unicode bytes" && read(backend / raw) == "raw bytes");
    results = transfer({remote.child("tree")}, local, TransferAction::Move);
    TEST_CHECK(
        results.front().outcome == Moved
        && !fs::exists(backend / "tree")
        && read(root / "destination/tree/nested/file.txt") == "nested bytes"
    );
    // Folder merge keeps destination-only entries and commits child conflicts independently.
    fs::create_directories(backend / "tree/nested");
    write(backend / "tree/only-destination", "keep");
    write(backend / "tree/nested/file.txt", "old");
    if (provider) {
      TransferService service;
      TEST_CHECK(service.start({{native.child("tree")}, remote, TransferAction::Copy}));
      wait(service, [](const auto& conflict) { return conflict.canMerge ? Merge : TransferConflictChoice::Replace; });
      TEST_CHECK(service.results().front().outcome == Copied);
    } else {
      ProviderTransfer service({{native.child("tree")}, remote, TransferAction::Copy});
      std::atomic<std::uint64_t> bytes = 0;
      std::atomic<std::size_t> count = 0;
      TEST_CHECK(
          service
              .run(
                  CancelToken{}, bytes, count,
                  [](const auto& conflict) { return conflict.canMerge ? Merge : TransferConflictChoice::Replace; }
              )
              .front()
              .outcome
          == Copied
      );
    }
    TEST_CHECK(
        read(backend / "tree/only-destination") == "keep" && read(backend / "tree/nested/file.txt") == "nested bytes"
    );
    results = transfer({native.child("café space.txt")}, remote, TransferAction::Copy, KeepBoth);
    TEST_CHECK(results.front().outcome == Copied && read(backend / "café space (1).txt") == "unicode bytes");
    results = transfer({native.child(raw)}, remote, TransferAction::Move, Skip);
    TEST_CHECK(results.front().outcome == Skipped && fs::exists(root / "source" / raw));
    results = transfer({native.child("tree")}, native.child("tree/nested"), TransferAction::Copy);
    TEST_CHECK(results.front().outcome == Failed && results.front().error->kind == FileErrorKind::WouldRecurse);
    // Replacing the captured source while the conflict question is open must not publish it.
    const CancelToken token;
    ProviderTransfer changed({{native.child(raw)}, remote, TransferAction::Move});
    std::atomic<std::uint64_t> bytes = 0;
    std::atomic<std::size_t> count = 0;
    results = changed.run(token, bytes, count, [&](const auto&) {
      write(root / "source" / raw, "replacement source with different length");
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        results.front().outcome == Failed
        && results.front().error->kind == FileErrorKind::Changed
        && read(backend / raw) == "raw bytes"
    );
    ProviderTransfer cancelled({{native.child(raw)}, remote, TransferAction::Move});
    results = cancelled.run(token, bytes, count, [&](const auto&) {
      token.cancel();
      return Cancel;
    });
    TEST_CHECK(results.front().outcome == Cancelled && cancelled.retryable() == 0 && fs::exists(root / "source" / raw));
    // A write-denied copy can be explicitly retried after permissions are corrected.
    fs::create_directory(backend / "denied");
    fs::permissions(backend / "denied", fs::perms::owner_read | fs::perms::owner_exec);
    if (geteuid() != 0) {
      TransferService retry;
      TEST_CHECK(retry.start({{native.child("café space.txt")}, remote.child("denied"), TransferAction::Copy}));
      wait(retry);
      // The normal service routes all-local jobs through its native implementation too.
      TEST_CHECK(retry.results().front().outcome == Failed);
      fs::permissions(backend / "denied", fs::perms::owner_all);
      if (provider) {
        TEST_CHECK(retry.retryable() == 1 && retry.retryFailed());
        wait(retry);
        TEST_CHECK(retry.results().front().outcome == Copied);
      }
    }
    fs::permissions(backend / "denied", fs::perms::owner_all);
    if (provider) {
      write(root / "source/to-remote.txt", "move upload");
      results = transfer({native.child("to-remote.txt")}, remote, TransferAction::Move);
      TEST_CHECK(
          results.front().outcome == Moved
          && !fs::exists(root / "source/to-remote.txt")
          && read(backend / "to-remote.txt") == "move upload"
      );
      // A completed upload never permits a failed source deletion to masquerade as a successful move.
      fs::create_directory(backend / "read-source");
      write(backend / "read-source/stays.txt", "retain original");
      fs::permissions(backend / "read-source", fs::perms::owner_read | fs::perms::owner_exec);
      results = transfer({remote.child("read-source/stays.txt")}, local, TransferAction::Move);
      TEST_CHECK(
          results.front().outcome == Partial
          && fs::exists(backend / "read-source/stays.txt")
          && read(root / "destination/stays.txt") == "retain original"
      );
      fs::permissions(backend / "read-source", fs::perms::owner_all);
      // Cancel while the real provider is streaming, not just while a question is pending.
      {
        std::ofstream large(root / "source/large.bin");
        large.seekp(128 * 1024 * 1024 - 1);
        large.put('X');
      }
      TransferService streaming;
      TEST_CHECK(streaming.start({{native.child("large.bin")}, remote, TransferAction::Copy}));
      const auto deadline = g_get_monotonic_time() + 15000000;
      while (streaming.busy() && streaming.bytes() == 0) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
      TEST_CHECK(streaming.busy());
      streaming.cancel();
      wait(streaming);
      TEST_CHECK(
          streaming.results().front().outcome == Cancelled
          && !fs::exists(backend / "large.bin")
          && fs::exists(root / "source/large.bin")
      );
      fs::remove(root / "source/large.bin");
    }
    // No temporary publication directories remain after successful/conflicted/cancelled jobs.
    for (const auto& item : fs::recursive_directory_iterator(backend)) {
      TEST_CHECK(!item.path().filename().native().starts_with(".luciole-transfer-"));
    }
  }
  void put(const Location& location, const std::string& text) {
    GError* error = nullptr;
    TEST_CHECK(g_file_replace_contents(
        gioFile(location).get(), text.data(), text.size(), nullptr, false, G_FILE_CREATE_NONE, nullptr, nullptr, &error
    ));
  }
  std::string fetch(const Location& location) {
    char* data = nullptr;
    gsize size = 0;
    if (!g_file_load_contents(gioFile(location).get(), nullptr, &data, &size, nullptr, nullptr)) {
      return {};
    }
    std::string text(data, size);
    g_free(data);
    return text;
  }
  // Names in `folder` left by staging, which a finished or cancelled transfer must never leave behind.
  bool staged(const Location& folder) {
    const GioPtr<GFileEnumerator> entries(g_file_enumerate_children(
        gioFile(folder).get(), "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    while (entries) {
      const GioPtr<GFileInfo> entry(g_file_enumerator_next_file(entries.get(), nullptr, nullptr));
      if (!entry) {
        break;
      }
      if (std::string_view(g_file_info_get_name(entry.get())).starts_with(".luciole-")) {
        return true;
      }
    }
    return false;
  }
  bool present(const Location& location) {
    const GioPtr<GFileInfo> found(g_file_query_info(
        gioFile(location).get(), "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    return found != nullptr;
  }
  void removeTree(const Location& location) {
    const GioPtr<GFileEnumerator> entries(g_file_enumerate_children(
        gioFile(location).get(), "standard::name,standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    while (entries) {
      const GioPtr<GFileInfo> entry(g_file_enumerator_next_file(entries.get(), nullptr, nullptr));
      if (!entry) {
        break;
      }
      const auto child = location.child(g_file_info_get_name(entry.get()));
      if (g_file_info_get_file_type(entry.get()) == G_FILE_TYPE_DIRECTORY) {
        removeTree(child);
      } else {
        g_file_delete(gioFile(child).get(), nullptr, nullptr);
      }
    }
    g_file_delete(gioFile(location).get(), nullptr, nullptr);
  }
  // Copies and moves onto the phone: from local folders and within the phone, with conflicts, cancelling and Undo.
  void phoneWrites(const Location& root, const fs::path& local) {
    const auto folder = Location::fromPath((local / "moved").native());
    TransferService upload;
    TEST_CHECK(upload.start({{folder}, root, Copy}));
    wait(upload);
    TEST_CHECK(upload.results().front().outcome == Copied);
    TEST_CHECK(fetch(root.child("moved").child("Camera").child("IMG_0002.jpg")) == "second photo");

    // Replace swaps in the new file only once it has fully arrived; Keep Both picks a free name.
    write(local / "note café.txt", "from the computer");
    const auto note = Location::fromPath((local / "note café.txt").native());
    TransferService replace;
    TEST_CHECK(replace.start({{note}, root, Copy}));
    wait(replace, [](const auto&) { return Replace; });
    TEST_CHECK(
        replace.results().front().outcome == Copied && fetch(root.child("note café.txt")) == "from the computer"
    );
    TransferService both;
    TEST_CHECK(both.start({{note}, root, Copy}));
    wait(both, [](const auto&) { return KeepBoth; });
    TEST_CHECK(fetch(root.child("note café (1).txt")) == "from the computer");
    TEST_CHECK(fetch(root.child("note café.txt")) == "from the computer");

    // A cancelled upload leaves nothing behind, and an existing file it would replace stays as it was.
    write(local / "large.bin", std::string(32U << 20U, 'Y'));
    put(root.child("large.bin"), "kept");
    TransferService cancelled;
    TEST_CHECK(cancelled.start({{Location::fromPath((local / "large.bin").native())}, root, Copy}));
    cancelled.cancel();
    wait(cancelled, [](const auto&) { return Replace; });
    const auto outcome = cancelled.results().front().outcome;
    TEST_CHECK(outcome == Cancelled || outcome == Copied);
    if (outcome == Cancelled) {
      TEST_CHECK(fetch(root.child("large.bin")) == "kept");
    }

    // Within the phone: Copy keeps the original, Move removes it.
    TransferService within;
    TEST_CHECK(within.start({{root.child("note café (1).txt")}, root.child("DCIM"), Copy}));
    wait(within);
    TEST_CHECK(within.results().front().outcome == Copied);
    TEST_CHECK(present(root.child("DCIM").child("note café (1).txt")) && present(root.child("note café (1).txt")));
    TransferService moved;
    TEST_CHECK(moved.start({{root.child("note café (1).txt")}, root.child("moved"), Move}));
    wait(moved);
    TEST_CHECK(moved.results().front().outcome == Moved);
    TEST_CHECK(present(root.child("moved").child("note café (1).txt")) && !present(root.child("note café (1).txt")));

    // Moving from the computer removes the local file once it has arrived; Undo of a copy removes it again.
    write(local / "to-phone.txt", "moving");
    TransferService cut;
    TEST_CHECK(cut.start({{Location::fromPath((local / "to-phone.txt").native())}, root, Move}));
    wait(cut);
    TEST_CHECK(cut.results().front().outcome == Moved && !fs::exists(local / "to-phone.txt"));
    TEST_CHECK(fetch(root.child("to-phone.txt")) == "moving");
    write(local / "undo.txt", "undo me");
    TransferService recorded;
    recorded.useOperationHistory();
    TEST_CHECK(recorded.start({{Location::fromPath((local / "undo.txt").native())}, root, Copy}));
    wait(recorded);
    TEST_CHECK(recorded.results().front().outcome == Copied && present(root.child("undo.txt")));
    TransferService undo;
    TEST_CHECK(undo.replayCopy(recorded.takeCopyReceipt()));
    wait(undo);
    TEST_CHECK(!present(root.child("undo.txt")) && fs::exists(local / "undo.txt"));

    TEST_CHECK(!staged(root) && !staged(root.child("DCIM")) && !staged(root.child("moved")));
  }
  // Copies and moves from an MTP or PTP device folder into a local folder. `device` is a writable scratch folder
  // on a connected device; fixtures are written there with GIO and removed afterwards.
  void deviceTransfers(const Location& device, const fs::path& local) {
    TEST_CHECK(transferDeviceSource(device));
    const luciole::GString uuid(g_uuid_string_random());
    const auto root = device.child(std::string("luciole-run-") + uuid.get());
    TEST_CHECK(g_file_make_directory(gioFile(root).get(), nullptr, nullptr));
    TEST_CHECK(g_file_make_directory(gioFile(root.child("DCIM")).get(), nullptr, nullptr));
    TEST_CHECK(g_file_make_directory(gioFile(root.child("DCIM").child("Camera")).get(), nullptr, nullptr));
    put(root.child("DCIM").child("IMG_0001.jpg"), "first photo");
    put(root.child("DCIM").child("Camera").child("IMG_0002.jpg"), "second photo");
    put(root.child("note café.txt"), "phone note");
    put(root.child("large.bin"), std::string(32U << 20U, 'X'));
    const auto destination = Location::fromPath(local.native());
    {
      TransferService copy;
      TEST_CHECK(copy.start({{root.child("DCIM"), root.child("note café.txt")}, destination, Copy}));
      wait(copy);
      TEST_CHECK(std::ranges::all_of(copy.results(), [](const auto& result) { return result.outcome == Copied; }));
      TEST_CHECK(read(local / "DCIM/IMG_0001.jpg") == "first photo");
      TEST_CHECK(read(local / "DCIM/Camera/IMG_0002.jpg") == "second photo");
      TEST_CHECK(read(local / "note café.txt") == "phone note");
      TEST_CHECK(present(root.child("DCIM").child("IMG_0001.jpg")));

      // Folders arrive file by file, yet Undo still removes the whole copied folder.
      fs::create_directory(local / "undo");
      TransferService recorded;
      recorded.useOperationHistory();
      TEST_CHECK(recorded.start({{root.child("DCIM")}, Location::fromPath((local / "undo").native()), Copy}));
      wait(recorded);
      TEST_CHECK(
          recorded.results().front().outcome == Copied
          && read(local / "undo/DCIM/Camera/IMG_0002.jpg") == "second photo"
      );
      TransferService undo;
      TEST_CHECK(undo.replayCopy(recorded.takeCopyReceipt()));
      wait(undo);
      TEST_CHECK(!fs::exists(local / "undo/DCIM") && present(root.child("DCIM").child("IMG_0001.jpg")));

      TransferService again;
      TEST_CHECK(again.start({{root.child("note café.txt")}, destination, Copy}));
      wait(again, [](const auto&) { return KeepBoth; });
      TEST_CHECK(read(local / "note café (1).txt") == "phone note");
      write(local / "note café.txt", "edited on the computer");
      TransferService replace;
      TEST_CHECK(replace.start({{root.child("note café.txt")}, destination, Copy}));
      wait(replace, [](const auto&) { return Replace; });
      TEST_CHECK(read(local / "note café.txt") == "phone note");

      fs::create_directory(local / "moved");
      TransferService move;
      TEST_CHECK(
          move.start({{root.child("DCIM").child("Camera")}, Location::fromPath((local / "moved").native()), Move})
      );
      wait(move);
      TEST_CHECK(move.results().front().outcome == Moved);
      TEST_CHECK(read(local / "moved/Camera/IMG_0002.jpg") == "second photo");
      TEST_CHECK(!present(root.child("DCIM").child("Camera")) && present(root.child("DCIM").child("IMG_0001.jpg")));

      TransferService cancelled;
      TEST_CHECK(cancelled.start({{root.child("large.bin")}, destination, Copy}));
      cancelled.cancel();
      wait(cancelled);
      TEST_CHECK(cancelled.results().front().outcome == Cancelled || cancelled.results().front().outcome == Copied);
      TEST_CHECK(present(root.child("large.bin")));

      if (transferDeviceDestination(root)) {
        phoneWrites(root, local);
      } else {
        // Cameras are read-only destinations.
        TransferService upload;
        TEST_CHECK(upload.start({{Location::fromPath((local / "moved").native())}, root, Copy}));
        wait(upload);
        TEST_CHECK(upload.results().front().outcome == Failed && !present(root.child("moved")));
      }
    }
    // Replace keeps the overwritten file for Undo until its history is dropped with the job.
    drainHistory();
    for (const auto& item : fs::recursive_directory_iterator(local)) {
      TEST_CHECK(!item.path().filename().native().starts_with(".luciole-"));
    }
    removeTree(root);
  }
} // namespace
int main(int argc, char** argv) {
  GlibPollSource glib;
  if (argc == 4 && std::string_view(argv[1]) == "--device") {
    const auto device = Location::fromUri(argv[2]);
    mount(device);
    deviceTransfers(device, argv[3]);
  } else if (argc == 7 && std::string_view(argv[1]) == "--portable-provider") {
    const auto remote = Location::fromUri(argv[2]), readOnly = Location::fromUri(argv[5]);
    TEST_CHECK(transferLocationSupported(remote) && transferLocationSupported(readOnly));
    mount(remote);
    mount(readOnly);
    const luciole::GString uuid(g_uuid_string_random());
    const auto name = std::string("run-") + uuid.get();
    const auto backend = fs::path(argv[3]) / name, local = fs::path(argv[4]) / name;
    fs::create_directories(backend);
    fs::create_directories(local);
    portable(local, remote.child(name), backend, readOnly, argv[6]);
    mount(remote);
    downloadHistory(local / "history", remote.child(name).child("history"), backend / "history");
  } else if (argc == 5 && std::string_view(argv[1]) == "--provider") {
    const auto remote = Location::fromUri(argv[2]);
    TEST_CHECK(!remote.isNative() && transferLocationSupported(remote));
    mount(remote);
    const luciole::GString uuid(g_uuid_string_random());
    const auto name = std::string("run-") + uuid.get();
    const auto backend = fs::path(argv[3]) / name, local = fs::path(argv[4]) / name;
    fs::create_directories(backend);
    fs::create_directories(local);
    const auto testRoot = remote.child(name);
    run(local, testRoot, backend, true);
    downloadHistory(local / "history", testRoot.child("history"), backend / "history");
    write(local / "destination/to-remote.txt", "destination survives disconnect");
    TransferService disconnected;
    TEST_CHECK(disconnected.start(
        {{testRoot.child("to-remote.txt")}, Location::fromPath((local / "destination").native()), TransferAction::Move}
    ));
    wait(disconnected, [&](const auto&) {
      disconnect(testRoot);
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        disconnected.results().front().outcome == Failed
        && read(local / "destination/to-remote.txt") == "destination survives disconnect"
        && read(backend / "to-remote.txt") == "move upload"
    );
    mount(remote);
  } else {
    char directory[] = "/tmp/luciole-provider-test-XXXXXX";
    TEST_CHECK(mkdtemp(directory));
    const fs::path root(directory);
    fs::create_directory(root / "remote");
    run(root, Location::fromPath((root / "remote").native()), root / "remote", false);
    measurableSources();
    downloadsCountFiles(root / "counting");
    gioEstimates(root / "gio-estimate");
    downloadHistory(root / "history", Location::fromPath((root / "remote-history").native()), root / "remote-history");
    downloadFailureProofs(root / "history-failures");
    providerWriteback(root / "writeback");
    moveSourceRemovalFailure(root / "move-removal-failure");
    for (const auto* uri : {"trash:///file", "https://example.invalid/a", "sftp://user:secret@example.invalid/a"}) {
      TEST_CHECK(!transferLocationSupported(Location::fromUri(uri)));
    }
    for (const auto* uri : {"smb://server/share", "dav://server/files", "davs://server/files"}) {
      TEST_CHECK(transferLocationSupported(Location::fromUri(uri)));
    }
    const auto photo = Location::fromUri("mtp://SAMSUNG_Android_R58M/Internal%20storage/DCIM/photo.jpg");
    const auto local = Location::fromPath(root.native());
    for (const auto* uri :
         {"mtp://SAMSUNG_Android_R58M/Internal%20storage/DCIM/photo.jpg", "gphoto2://Canon_EOS/store_00010001/DCIM"}) {
      const auto item = Location::fromUri(uri);
      // Device items copy into local folders, never onto network ones.
      TEST_CHECK(transferDeviceSource(item) && !transferLocationSupported(item));
      TEST_CHECK(transferSupported(item, local));
      TEST_CHECK(!transferSupported(item, Location::fromUri("sftp://example.invalid/upload")));
    }
    // Phones accept local files and their own, below the top level that lists their storages; cameras accept nothing.
    const auto phoneFolder = Location::fromUri("mtp://SAMSUNG_Android_R58M/Internal%20storage/Download");
    const auto camera = Location::fromUri("gphoto2://Canon_EOS/store_00010001/DCIM");
    TEST_CHECK(transferDeviceDestination(phoneFolder) && transferDestinationSupported(phoneFolder));
    TEST_CHECK(transferDeviceDestination(Location::fromUri("mtp://SAMSUNG_Android_R58M/Internal%20storage")));
    TEST_CHECK(!transferDestinationSupported(Location::fromUri("mtp://SAMSUNG_Android_R58M/")));
    TEST_CHECK(!transferDestinationSupported(camera) && !transferSupported(local, camera));
    TEST_CHECK(transferSupported(local, phoneFolder) && transferSupported(photo, phoneFolder));
    TEST_CHECK(!transferSupported(Location::fromUri("mtp://Pixel_10/Internal%20storage/a.jpg"), phoneFolder));
    TEST_CHECK(!transferSupported(Location::fromUri("sftp://example.invalid/a"), phoneFolder));
    TEST_CHECK(!transferSupported(camera, phoneFolder));
    for (const auto* uri :
         {"mtp://SAMSUNG_Android_R58M/", "mtp://user:secret@SAMSUNG_Android_R58M/DCIM", "mtp:///DCIM"}) {
      TEST_CHECK(!transferDeviceSource(Location::fromUri(uri)));
    }
    TEST_CHECK(transferSupported(Location::fromUri("sftp://example.invalid/a"), local));
    TransferService toServer;
    TEST_CHECK(toServer.start({{photo}, Location::fromUri("sftp://example.invalid/upload"), TransferAction::Copy}));
    wait(toServer);
    TEST_CHECK(toServer.results().front().outcome == Failed && toServer.report().contains("phones and cameras"));
    const auto smb = Location::fromUri("smb://server/share/CaseFolder");
    TEST_CHECK(transferLocationsEqual(smb, Location::fromUri("smb://SERVER/SHARE/casefolder")));
    TEST_CHECK(transferLocationInside(Location::fromUri("smb://server/share/casefolder/child"), smb));
    TEST_CHECK(!transferLocationInside(Location::fromUri("smb://server/share/casefolder-other"), smb));
    TEST_CHECK(!transferLocationsEqual(
        Location::fromUri("dav://server/files/Case"), Location::fromUri("dav://server/files/case")
    ));
    const luciole::GioPtr<GFileInfo> noFlags(g_file_info_new());
    TEST_CHECK(!readIsSymlink(noFlags.get()));
    g_file_info_set_file_type(noFlags.get(), G_FILE_TYPE_DIRECTORY);
    TEST_CHECK(!readIsSymlink(noFlags.get()));
    g_file_info_set_file_type(noFlags.get(), G_FILE_TYPE_SYMBOLIC_LINK);
    TEST_CHECK(readIsSymlink(noFlags.get()));
    g_file_info_set_file_type(noFlags.get(), G_FILE_TYPE_REGULAR);
    g_file_info_set_is_symlink(noFlags.get(), true);
    TEST_CHECK(readIsSymlink(noFlags.get()));
    TEST_CHECK(transferLocationsEqual(smb, Location::fromUri("smb://server:445/share/casefolder")));
    TransferResult change{Location::fromUri("dav://server/files/tree/nested/item.txt"), Partial};
    change.destination = Location::fromUri("smb://server/share/CaseFolder/item.txt");
    const std::vector<TransferResult> effects{change};
    TEST_CHECK(transferAffectsLocation(effects, Location::fromUri("dav://server/files/tree/nested")));
    TEST_CHECK(transferAffectsLocation(effects, Location::fromUri("smb://SERVER/share/casefolder")));
    TEST_CHECK(!transferAffectsLocation(effects, Location::fromUri("dav://server/files")));
    TEST_CHECK(transferAffectsLocation(effects, Location::fromUri("dav://server/files"), true));
    TEST_CHECK(!transferAffectsLocation(effects, Location::fromUri("dav://other/files"), true));
    TEST_CHECK(!transferAffectsLocation(effects, Location::fromUri("dav://server/files/tree-other"), true));
    TransferResult merged{Location::fromUri("dav://server/files/tree"), Partial};
    merged.destination = Location::fromUri("smb://server/share/CaseFolder");
    merged.children = effects;
    TEST_CHECK(transferAffectsLocation({merged}, smb));
    TransferResult folderMoved{Location::fromUri("dav://server/files/tree"), Moved};
    TEST_CHECK(transferAffectsLocation({folderMoved}, Location::fromUri("dav://server/files/tree/nested")));
    TransferService refused;
    TEST_CHECK(refused.start(
        {{Location::fromUri("sftp://user:secret@example.invalid/a")},
         Location::fromPath(root.native()),
         TransferAction::Copy}
    ));
    wait(refused);
    TEST_CHECK(refused.results().front().outcome == Failed && !refused.report().contains("secret"));
    fs::remove_all(root);
  }
  std::println("provider transfer checks passed");
}
