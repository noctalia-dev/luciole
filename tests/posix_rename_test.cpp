#include "fs/posix_fd.h"
#include "test_check.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;

  struct Fixture {
    fs::path root;
    Fixture() {
      auto pattern = (fs::temp_directory_path() / "luciole-rename-test-XXXXXX").native();
      const char* created = ::mkdtemp(pattern.data());
      TEST_CHECK(created != nullptr);
      root = created;
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    luciole::Fd open(const fs::path& relative = {}) const {
      return luciole::checkedFd(::open((root / relative).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    }
  };

  void write(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  bool noExchangeTemporaries(const fs::path& directory) {
    return std::ranges::none_of(fs::recursive_directory_iterator(directory), [](const auto& entry) {
      return entry.path().filename().native().starts_with(".luciole-exchange-");
    });
  }

  void testNoReplace() {
    Fixture f;
    fs::create_directory(f.root / "other");
    write(f.root / "item", "A");
    write(f.root / "taken", "T");
    const auto root = f.open();
    const auto other = f.open("other");
    TEST_CHECK(luciole::renameNoReplace(root.get(), "item", other.get(), "moved") == 0);
    TEST_CHECK(!fs::exists(f.root / "item") && read(f.root / "other/moved") == "A");
    TEST_CHECK(luciole::renameNoReplace(other.get(), "moved", root.get(), "taken") == -1);
    TEST_CHECK(errno == EEXIST && read(f.root / "other/moved") == "A" && read(f.root / "taken") == "T");
    fs::create_directory(f.root / "folder");
    TEST_CHECK(luciole::renameNoReplace(root.get(), "folder", other.get(), "folder") == 0);
    TEST_CHECK(fs::is_directory(f.root / "other/folder"));
    TEST_CHECK(luciole::renameNoReplace(root.get(), "missing", other.get(), "missing") == -1 && errno == ENOENT);
  }

  void testExchange() {
    Fixture f;
    fs::create_directory(f.root / "stage");
    write(f.root / "stage/item", "new");
    write(f.root / "target", "old");
    const auto root = f.open();
    const auto stage = f.open("stage");
    TEST_CHECK(luciole::renameExchange(stage.get(), "item", root.get(), "target") == 0);
    TEST_CHECK(read(f.root / "target") == "new" && read(f.root / "stage/item") == "old");
    fs::create_directory(f.root / "folder");
    write(f.root / "folder/inside", "F");
    TEST_CHECK(luciole::renameExchange(stage.get(), "item", root.get(), "folder") == 0);
    TEST_CHECK(read(f.root / "stage/item/inside") == "F" && read(f.root / "folder") == "old");
    TEST_CHECK(noExchangeTemporaries(f.root));
  }

  void testExchangeFailuresRestoreNames() {
    Fixture f;
    write(f.root / "item", "I");
    const auto root = f.open();
    TEST_CHECK(luciole::renameExchange(root.get(), "item", root.get(), "missing") == -1 && errno == ENOENT);
    TEST_CHECK(read(f.root / "item") == "I");
    // A folder cannot trade places with an item inside it; both directions fail partway through a fallback swap.
    fs::create_directories(f.root / "folder/sub");
    write(f.root / "folder/sub/inner", "N");
    const auto sub = f.open("folder/sub");
    TEST_CHECK(luciole::renameExchange(sub.get(), "inner", root.get(), "folder") == -1 && errno == EINVAL);
    TEST_CHECK(read(f.root / "folder/sub/inner") == "N");
    TEST_CHECK(luciole::renameExchange(root.get(), "folder", sub.get(), "inner") == -1 && errno == EINVAL);
    TEST_CHECK(read(f.root / "folder/sub/inner") == "N" && fs::is_directory(f.root / "folder"));
    TEST_CHECK(noExchangeTemporaries(f.root));
  }

  bool sameCtime(const struct stat& a, const struct stat& b) {
    return a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
  }

  void testExchangeIntoReadOnlyLeavesItemsUntouched() {
    if (::geteuid() == 0) {
      return; // Permission-denial fixture needs an ordinary process.
    }
    Fixture f;
    fs::create_directory(f.root / "locked");
    write(f.root / "item", "I");
    write(f.root / "locked/target", "T");
    struct stat item{}, target{};
    TEST_CHECK(::lstat((f.root / "item").c_str(), &item) == 0);
    TEST_CHECK(::lstat((f.root / "locked/target").c_str(), &target) == 0);
    TEST_CHECK(::chmod((f.root / "locked").c_str(), 0555) == 0);
    const auto root = f.open();
    const auto locked = f.open("locked");
    TEST_CHECK(luciole::renameExchange(root.get(), "item", locked.get(), "target") == -1 && errno == EACCES);
    TEST_CHECK(luciole::renameExchange(locked.get(), "target", root.get(), "item") == -1 && errno == EACCES);
    struct stat itemAfter{}, targetAfter{};
    TEST_CHECK(::lstat((f.root / "item").c_str(), &itemAfter) == 0 && sameCtime(item, itemAfter));
    TEST_CHECK(::lstat((f.root / "locked/target").c_str(), &targetAfter) == 0 && sameCtime(target, targetAfter));
    TEST_CHECK(::chmod((f.root / "locked").c_str(), 0755) == 0);
    TEST_CHECK(noExchangeTemporaries(f.root));
  }
} // namespace

int main() {
  try {
    testNoReplace();
    testExchange();
    testExchangeFailuresRestoreNames();
    testExchangeIntoReadOnlyLeavesItemsUntouched();
    std::println("posix rename checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "posix rename test: {}", error.what());
    return 1;
  }
}
