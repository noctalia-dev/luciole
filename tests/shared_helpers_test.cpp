#include "fs/background_job.h"
#include "fs/file_error.h"
#include "fs/location.h"
#include "fs/posix_fd.h"
#include "operations/conflict_gate.h"
#include "test_check.h"

#include <cerrno>
#include <fcntl.h>
#include <glib.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
  void locationBoundaries() {
    const auto root = luciole::Location::fromPath("/");
    TEST_CHECK(!root.parent());
    const auto folder = luciole::Location::fromPath("/tmp/a b");
    const auto child = folder.child("c");
    TEST_CHECK(child == luciole::Location::fromPath("/tmp/a b/c"));
    TEST_CHECK(child.parent() == folder);
    TEST_CHECK(child.isInside(folder));
    TEST_CHECK(child.isInside(root));
    TEST_CHECK(!folder.isInside(folder));
    TEST_CHECK(!folder.isInside(child));
    TEST_CHECK(!luciole::Location::fromPath("/tmp/a bc").isInside(folder));
    TEST_CHECK(folder.resolve("c") == child);
    TEST_CHECK(folder.resolve("/etc") == luciole::Location::fromPath("/etc"));
    TEST_CHECK(folder.basename() == "a b");
    TEST_CHECK(folder.isNative());
    TEST_CHECK(!luciole::Location::trash().nativePath());

    const auto raw = folder.child(std::string("raw-") + static_cast<char>(0xff));
    TEST_CHECK(raw.basename() == std::string("raw-") + static_cast<char>(0xff));
    const auto display = raw.displayName();
    TEST_CHECK(display && g_utf8_validate(display->c_str(), static_cast<gssize>(display->size()), nullptr));
  }

  void errorMapping() {
    TEST_CHECK(luciole::errnoError(ENOENT).kind == luciole::FileErrorKind::Missing);
    TEST_CHECK(luciole::errnoError(EPERM).kind == luciole::FileErrorKind::Denied);
    TEST_CHECK(luciole::errnoError(ENOTEMPTY).kind == luciole::FileErrorKind::NotEmpty);
    TEST_CHECK(luciole::errnoError(ETIMEDOUT).kind == luciole::FileErrorKind::Unavailable);
    TEST_CHECK(luciole::errnoError(EIO).kind == luciole::FileErrorKind::Other);
    try {
      throw std::runtime_error("plain");
    } catch (...) {
      TEST_CHECK((luciole::currentFileError() == luciole::FileError{luciole::FileErrorKind::Other, "plain"}));
    }
    try {
      luciole::throwFileError(luciole::FileErrorKind::Changed, "moved");
    } catch (const std::runtime_error& error) {
      TEST_CHECK(std::string(error.what()) == "moved");
      TEST_CHECK(luciole::currentFileError().kind == luciole::FileErrorKind::Changed);
    }
  }

  void descriptors() {
    bool threw = false;
    try {
      (void)luciole::checkedFd(::open("/nonexistent-luciole", O_RDONLY | O_CLOEXEC));
    } catch (const luciole::FileErrorException& error) {
      threw = error.error().kind == luciole::FileErrorKind::Missing;
    }
    TEST_CHECK(threw);
    auto first = luciole::checkedFd(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const int raw = first.get();
    luciole::Fd second(std::move(first));
    TEST_CHECK(!first.valid() && second.get() == raw); // NOLINT(bugprone-use-after-move)
    TEST_CHECK(luciole::sameIdentity(luciole::statFd(second.get()), luciole::statFd(raw)));
    TEST_CHECK(second.close() == 0 && !second.valid());
  }

  void jobs() {
    std::optional<luciole::JobResult<int>> delivered;
    luciole::runJob<int>([] { return 7; }, [&](luciole::JobResult<int> result) { delivered = std::move(result); });
    luciole::CancelToken cancelled;
    cancelled.cancel();
    bool ran = false;
    std::optional<luciole::JobResult<int>> skipped;
    luciole::runJob<int>(
        cancelled,
        [&] {
          ran = true;
          return 1;
        },
        [&](luciole::JobResult<int> result) { skipped = std::move(result); }
    );
    while (!delivered || !skipped) {
      g_main_context_iteration(nullptr, TRUE);
    }
    TEST_CHECK(delivered->value() == 7);
    TEST_CHECK(!ran && !skipped->has_value() && skipped->error().kind == luciole::FileErrorKind::Cancelled);
  }

  struct Question {
    std::uint64_t id = 0;
  };

  void conflictGate() {
    luciole::ConflictGate<Question, int> gate;
    luciole::CancelToken token;
    std::optional<int> answer = 0;
    std::thread worker([&] { answer = gate.ask({}, token, [] {}); });
    while (!gate.pending()) {
      std::this_thread::yield();
    }
    const auto id = gate.pending()->id;
    TEST_CHECK(!gate.resolve(id + 1, 3, [](const Question&) { return true; }));
    TEST_CHECK(!gate.resolve(id, 3, [](const Question&) { return false; }));
    TEST_CHECK(gate.resolve(id, 3, [](const Question&) { return true; }));
    worker.join();
    TEST_CHECK(answer == 3 && !gate.pending());

    answer = gate.ask({}, token, [&] {
      TEST_CHECK(gate.resolve(gate.pending()->id, 4, [](const Question&) { return true; }));
      TEST_CHECK(!gate.pending());
    });
    TEST_CHECK(answer == 4);

    std::thread cancelled([&] { answer = gate.ask({}, token, [] {}); });
    while (!gate.pending()) {
      std::this_thread::yield();
    }
    token.cancel();
    gate.wake();
    cancelled.join();
    TEST_CHECK(!answer);
  }
} // namespace

int main() {
  locationBoundaries();
  errorMapping();
  descriptors();
  jobs();
  conflictGate();
  return 0;
}
