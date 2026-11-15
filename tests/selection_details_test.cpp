#include "browser/entry_details.h"
#include "browser/selection_details.h"
#include "fs/file_query.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  void wait(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  luciole::DirectoryEntry entry(const fs::path& path) {
    luciole::DirectoryEntry result{
        .place = luciole::Location::fromPath(path.native()), .name = {}, .size = {}, .modifiedSeconds = {}
    };
    const auto details = luciole::readFileDetails(result.location(), {});
    result.name = path.filename().native();
    result.setDisplayName(result.name);
    result.type = details.type;
    result.symlink = details.type == luciole::FileType::Symlink;
    result.size = details.size;
    result.setIdentity(details.identity);
    result.setEtag(details.etag);
    result.contentRevision = details.contentRevision;
    result.details = details.metadata;
    return result;
  }
  std::string value(const luciole::SelectionDetailsService& service, const std::string& label) {
    for (const auto& [key, text] : service.fields()) {
      if (key == label) {
        return text;
      }
    }
    return {};
  }
  void typedMetadataChecks() {
    const luciole::GioPtr<GFileInfo> info(g_file_info_new());
    g_file_info_set_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_CHANGED, 123);
    auto details = luciole::readEntryMetadata(info.get());
    TEST_CHECK(!details.createdSeconds && !details.accessedSeconds && !details.mode && !details.owner);
    g_file_info_set_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_ACCESS, 0);
    g_file_info_set_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_TIME_CREATED, 99);
    g_file_info_set_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_UNIX_MODE, 0100640);
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_OWNER_USER, "<literal> café");
    details = luciole::readEntryMetadata(info.get());
    TEST_CHECK(details.accessedSeconds == 0 && !details.createdSeconds && details.mode == 0640);
    TEST_CHECK(details.owner == "<literal> café" && !details.group);
    luciole::DirectoryEntry source{
        .place = luciole::Location::fromPath("/tmp/metadata-only"), .name = {}, .size = {}, .modifiedSeconds = {}
    };
    source.details = details;
    TEST_CHECK(luciole::entryColumnText(source, luciole::DetailsColumn::Created) == "Unknown");
    TEST_CHECK(luciole::entryColumnText(source, luciole::DetailsColumn::Permissions) == "0640");
    TEST_CHECK(luciole::entrySortNumber(source, luciole::SortField::Accessed) == 0);
    TEST_CHECK(!luciole::entrySortNumber(source, luciole::SortField::Created));
  }
  void selectionChecks(const fs::path& root) {
    fs::create_directory(root / "folder");
    std::ofstream(root / "folder" / "not-counted.bin") << std::string(200000, 'x');
    std::ofstream(root / "a.txt") << "hello";
    std::ofstream(root / "b-café <x>.txt") << "abc";
    TEST_CHECK(::chmod((root / "a.txt").c_str(), 0600) == 0);
    TEST_CHECK(::chmod((root / "b-café <x>.txt").c_str(), 0640) == 0);
    fs::create_symlink("a.txt", root / "link");
    TEST_CHECK(::mkfifo((root / "pipe").c_str(), 0600) == 0);
    const std::vector targets{
        entry(root / "a.txt"), entry(root / "b-café <x>.txt"), entry(root / "folder"), entry(root / "link"),
        entry(root / "pipe")
    };
    luciole::SelectionDetailsService service;
    service.show(targets);
    wait([&] { return !service.busy(); });
    TEST_CHECK(value(service, "Inspected") == "5 of 5 items");
    TEST_CHECK(value(service, "Files") == "2" && value(service, "Folders") == "1");
    TEST_CHECK(value(service, "Links") == "1" && value(service, "Other") == "1");
    TEST_CHECK(value(service, "File/link size") == "13 bytes");
    TEST_CHECK(value(service, "Folder contents") == "Not counted");
    TEST_CHECK(value(service, "Permissions") == "Mixed");
    TEST_CHECK(!value(service, "Owner").empty() && value(service, "Owner") != "Mixed");
    TEST_CHECK(!value(service, "Accessed").empty());
    auto stale = targets;
    stale[0].setIdentity(luciole::FileIdentity{"wrong", "identity"});
    fs::remove(root / "b-café <x>.txt");
    service.show(stale);
    wait([&] { return !service.busy(); });
    TEST_CHECK(value(service, "Inspected") == "3 of 5 items");
    TEST_CHECK(value(service, "Unavailable").starts_with("2 "));
    TEST_CHECK(value(service, "File/link size").starts_with("At least "));
    TEST_CHECK(value(service, "Owner").contains("some unknown"));
    fs::create_directory(root / "other");
    std::ofstream(root / "other" / "child.txt") << 'a';
    service.show(std::vector{entry(root / "a.txt"), entry(root / "other" / "child.txt")});
    wait([&] { return !service.busy(); });
    TEST_CHECK(value(service, "Location") == "Mixed");
  }
  void lifecycleChecks(const fs::path& root) {
    const auto source = entry(root / "a.txt");
    std::vector<luciole::DirectoryEntry> many(4100, source);
    luciole::SelectionDetailsService service;
    int changed = 0;
    service.setChangedCallback([&] { ++changed; });
    service.show(many);
    wait([&] { return !service.busy(); });
    TEST_CHECK(value(service, "Inspected") == "4096 of 4100 items");
    TEST_CHECK(!value(service, "Inspection limit").empty());
    service.show(many);
    service.clear();
    wait([&] { return !service.busy(); });
    TEST_CHECK(changed == 1 && service.fields().empty());
    for (int i = 0; i < 20; ++i) {
      service.show(many);
    }
    service.show(std::span(&source, 1));
    wait([&] { return !service.busy(); });
    TEST_CHECK(changed == 2 && value(service, "Inspected") == "1 of 1 items");
    {
      luciole::SelectionDetailsService retiring;
      retiring.setChangedCallback([] { TEST_CHECK(false); });
      retiring.show(many);
    }
    const auto drain = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    wait([&] { return std::chrono::steady_clock::now() >= drain; });
  }
} // namespace

int main() {
  const luciole::GString temporary(g_dir_make_tmp("luciole-selection-details-XXXXXX", nullptr));
  TEST_CHECK(temporary);
  const fs::path root(temporary.get());
  typedMetadataChecks();
  selectionChecks(root);
  lifecycleChecks(root);
  fs::remove_all(root);
  std::println("selection details checks passed");
}
