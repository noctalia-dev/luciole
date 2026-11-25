#include "browser/directory_model.h"
#include "browser/entry_details.h"
#include "directory_fixture.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "model/details_columns.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <limits>
#include <print>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  using luciole::DirectoryEntry;
  using luciole::DirectoryModel;
  using luciole::DirectorySnapshot;
  using luciole::DirectoryStatus;
  using luciole::EntryId;
  using luciole::Location;
  using luciole::SelectionMode;
  using luciole::SortField;

  DirectoryEntry entry(EntryId id, const std::string& name, luciole::FileType type = luciole::FileType::Regular) {
    return {
        .id = id,
        .place = Location::fromPath("/tmp/luciole-model-fixture/" + name),
        .name = name,
        .type = type,
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt
    };
  }

  DirectorySnapshot snapshot(std::vector<DirectoryEntry> entries, std::uint64_t generation = 1) {
    DirectorySnapshot result;
    result.generation = generation;
    result.location = Location::fromPath("/tmp/luciole-model-fixture");
    result.status = DirectoryStatus::Ready;
    result.entries = std::move(entries);
    return result;
  }

  void rows(const DirectoryModel& model, const std::vector<EntryId>& expected) { TEST_CHECK(model.rows() == expected); }

  void selected(const DirectoryModel& model, const std::vector<EntryId>& expected) {
    TEST_CHECK(model.selectedIds() == expected);
  }

  void testBackupVisibility() {
    auto hidden = entry(3, ".hidden"), hiddenBackup = entry(4, ".hidden~");
    hidden.hidden = hiddenBackup.hidden = true;
    auto renamed = entry(2, "notes.txt~");
    renamed.setDisplayName("A normal label");
    auto ordinary = entry(10, "ordinary.txt");
    ordinary.setDisplayName("A label ending~");
    auto raw = entry(9, "raw-\xff~");
    raw.name = std::string("raw-\xff~");
    raw.setDisplayName("raw bytes");
    DirectoryModel model;
    model.applySnapshot(snapshot(
        {entry(1, "notes.txt"), renamed, hidden, hiddenBackup, entry(5, "directory~", luciole::FileType::Directory),
         entry(6, "notes.bak"), entry(7, "link~", luciole::FileType::Symlink), entry(8, "café-<literal>-文~"), raw,
         ordinary}
    ));
    TEST_CHECK(model.rows().size() == 8 && model.filter().showBackups);
    TEST_CHECK(model.select(2));
    model.setFilter({.showHidden = false, .text = {}, .showBackups = false});
    TEST_CHECK(model.rows().size() == 4 && model.selection().selected.empty());
    for (const auto id : {1, 5, 6, 10}) {
      TEST_CHECK(std::ranges::contains(model.rows(), id));
    }
    for (const auto id : {2, 3, 4, 7, 8, 9}) {
      TEST_CHECK(!std::ranges::contains(model.rows(), id));
    }
    TEST_CHECK(model.entry(2)->name == "notes.txt~" && model.entry(2)->displayName() == "A normal label");
    model.setFilter({.showHidden = true, .text = {}, .showBackups = false});
    TEST_CHECK(
        model.rows().size() == 5 && std::ranges::contains(model.rows(), 3) && !std::ranges::contains(model.rows(), 4)
    );
    model.setFilter({.showHidden = true, .text = {}, .showBackups = true});
    TEST_CHECK(model.rows().size() == 10 && model.selection().selected.empty());
  }

  void testPublishedEntryOwnership() {
    auto source = snapshot({entry(1, "folder", luciole::FileType::Directory), entry(2, "raw-\xff")});
    DirectoryModel model;
    model.applySnapshot(source);
    const auto retained = model.snapshot();
    const auto* folder = &source.entries[0];
    TEST_CHECK(model.entry(1) == folder && &retained.entries[0] == folder);
    luciole::DirectoryEntries children{entry(3, "café-文.txt"), entry(4, "other.txt")};
    const auto* child = &children[0];
    auto joined = luciole::DirectoryEntries::concatenate({source.entries, children});
    TEST_CHECK(&joined[0] == folder && &joined[2] == child);
    auto projection = luciole::DirectoryEntries::project({joined}, {{0, 0}, {0, 2}});
    TEST_CHECK(&projection[0] == folder && &projection[1] == child);
    source.entries = {};
    children = {};
    joined = {};
    auto data = snapshot({}, 2);
    data.entries = projection;
    model.applySnapshot(data, {{3, 1}});
    rows(model, {1, 3});
    TEST_CHECK(model.entry(3) == child && model.treeParent(3) == 1);
    model.setFilter({.text = "文"});
    rows(model, {1, 3});
    model.setFilter({});
    TEST_CHECK(model.select(3));
    const auto expanded = model.snapshot();
    model.applySnapshot(snapshot({entry(1, "replacement")}, 3));
    TEST_CHECK(model.selection().selected.empty() && !model.entry(3));
    TEST_CHECK(expanded.entries[1].displayName() == "café-文.txt" && &expanded.entries[1] == child);
    TEST_CHECK(retained.entries[1].name == "raw-\xff" && retained.entries[0].displayName() == "folder");
    auto appended = luciole::DirectoryEntries::concatenate({projection, luciole::DirectoryEntries{entry(5, "last")}});
    projection = {};
    TEST_CHECK(appended.size() == 3 && &appended[1] == child && appended.back().id == 5);
    for (const auto index : {luciole::DirectoryEntries::Index{1, 0}, luciole::DirectoryEntries::Index{0, 3}}) {
      bool rejected = false;
      try {
        (void)luciole::DirectoryEntries::project({appended}, {index});
      } catch (const std::out_of_range&) {
        rejected = true;
      }
      TEST_CHECK(rejected);
    }
  }

  void testCatalogueProjectionAndPendingFacts() {
    luciole::BrowseCatalogue catalogue;
    catalogue.folder = std::make_shared<const Location>(Location::fromPath("/tmp/luciole-model-fixture"));
    const std::vector<std::string> names{"item20", "item2", "Straße.txt", "a~", ".hidden", "folder~", "item1"};
    std::vector<DirectoryEntry> eager;
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto type = i == 5 ? luciole::FileType::Directory : luciole::FileType::Regular;
      catalogue.items.push_back(
          {.id = i + 1,
           .offset = static_cast<std::uint32_t>(catalogue.names.size()),
           .length = static_cast<std::uint16_t>(names[i].size()),
           .type = type,
           .hidden = i == 4,
           .symlink = false}
      );
      catalogue.names.append(names[i]);
      catalogue.names.push_back('\0');
      auto value = entry(i + 1, names[i], type);
      value.hidden = i == 4;
      eager.push_back(std::move(value));
    }
    auto data = snapshot({});
    data.entries = luciole::DirectoryEntries(std::move(catalogue));
    const auto retained = data.entries;
    DirectoryModel lazy, reference;
    std::vector<EntryId> requests;
    lazy.setMetadataRequestedCallback([&](std::size_t index, EntryId id, std::uint8_t) {
      TEST_CHECK(lazy.snapshot().entries.brief(index).id == id);
      requests.push_back(id);
    });
    TEST_CHECK(lazy.applySnapshot(data));
    reference.applySnapshot(snapshot(eager));
    TEST_CHECK(lazy.rows() == reference.rows() && requests.empty());
    for (const auto& filter :
         {luciole::FilterOptions{.showHidden = true, .text = {}, .showBackups = false},
          luciole::FilterOptions{.showHidden = false, .text = "STRASSE"}}) {
      lazy.setFilter(filter);
      reference.setFilter(filter);
      TEST_CHECK(lazy.rows() == reference.rows());
    }
    lazy.setFilter({});
    reference.setFilter({});
    TEST_CHECK(!lazy.selectPattern("item?"));
    TEST_CHECK(lazy.selectedIds().size() == 2);
    TEST_CHECK(lazy.selectionSummary().pendingSizes == 2 && lazy.selectionSummary().unknownSizes == 0);
    TEST_CHECK(lazy.entry(2)->metadataPending);
    auto known = eager[1];
    known.size = 0;
    known.contentType = "application/x-zerosize";
    data.generation = 2;
    data.entries = retained.withFacts({{1, known}});
    TEST_CHECK(lazy.applySnapshot(data));
    TEST_CHECK(lazy.entry(2)->size == 0 && !lazy.entry(2)->metadataPending);
    TEST_CHECK(lazy.selectionSummary().pendingSizes == 1 && lazy.selectionSummary().bytes == 0);
    TEST_CHECK(lazy.rows() == reference.rows() && retained[1].metadataPending);
    known.queriedDetails = 0;
    known.details = {};
    data.generation = 3;
    data.entries = retained.withFacts({{1, known}});
    lazy.applySnapshot(data);
    TEST_CHECK(luciole::entryColumnText(*lazy.entry(2), luciole::DetailsColumn::Owner) == "Reading…");
    known.queriedDetails = luciole::EntryDetails::Owner;
    data.generation = 4;
    data.entries = retained.withFacts({{1, known}});
    lazy.applySnapshot(data);
    TEST_CHECK(luciole::entryColumnText(*lazy.entry(2), luciole::DetailsColumn::Owner) == "Unknown");
    const auto old = *lazy.entry(2);
    known.size = 9;
    data.generation = 5;
    data.entries = retained.withFacts({{1, known}});
    lazy.applySnapshot(data);
    TEST_CHECK(old.size == 0 && lazy.entry(2)->size == 9);
    auto projection = luciole::DirectoryEntries::project({data.entries}, {{0, 1}, {0, 6}});
    data.generation = 6;
    data.entries = projection;
    lazy.applySnapshot(data);
    TEST_CHECK(lazy.entry(2)->size == 9 && lazy.entry(7)->metadataPending);
    // Pending projected rows must have a safe label, including when a tree mixes eager and catalogue blocks.
    TEST_CHECK(!lazy.typeLabel(7).empty());
    auto eagerProjection =
        luciole::DirectoryEntries::project({luciole::DirectoryEntries{entry(88, "eager")}}, {{0, 0}});
    const auto* eagerPointer = &eagerProjection.front();
    const auto mixed = luciole::DirectoryEntries::project({eagerProjection, projection}, {{0, 0}, {1, 0}, {1, 1}});
    TEST_CHECK(&mixed[0] == eagerPointer && mixed.known(1)->size == 9 && !mixed.known(2));
    TEST_CHECK(!mixed.catalogueRow(0) && mixed.catalogueRow(2)->catalogue->brief(mixed.catalogueRow(2)->index).id == 7);
    TEST_CHECK(mixed.brief(0).id == 88 && mixed.location(0) == eagerPointer->location());
    const auto joined = luciole::DirectoryEntries::concatenate({eagerProjection, mixed});
    TEST_CHECK(joined.size() == 4 && &joined[1] == eagerPointer && joined.brief(3).id == 7);
    data.generation = 7;
    data.entries = retained.withFacts({{1, known}});
    lazy.applySnapshot(data);
    TEST_CHECK(lazy.select(2));
    data.generation = 8;
    data.status = DirectoryStatus::Cancelled;
    data.entries = data.entries.withoutFacts();
    lazy.applySnapshot(data);
    TEST_CHECK(lazy.selection().selected.empty() && !lazy.selection().focused);
  }

  // A catalogue learns its links' targets after publication; the name-sorted path that skips sorting still re-sorts.
  void testCatalogueDirectoryLinks() {
    luciole::BrowseCatalogue catalogue;
    catalogue.folder = std::make_shared<const Location>(Location::fromPath("/tmp/luciole-model-fixture"));
    const std::vector<std::pair<std::string, luciole::FileType>> items{
        {"b-folder", luciole::FileType::Directory},
        {"a-link", luciole::FileType::Symlink},
        {"c-link", luciole::FileType::Symlink},
        {"a-file", luciole::FileType::Regular}
    };
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& [name, type] = items[i];
      catalogue.items.push_back(
          {.id = i + 1,
           .offset = static_cast<std::uint32_t>(catalogue.names.size()),
           .length = static_cast<std::uint16_t>(name.size()),
           .type = type,
           .hidden = false,
           .symlink = type == luciole::FileType::Symlink}
      );
      catalogue.names.append(name);
      catalogue.names.push_back('\0');
    }
    auto data = snapshot({});
    data.entries = luciole::DirectoryEntries(std::move(catalogue));
    const auto listed = data.entries;
    TEST_CHECK(listed[0].icon == luciole::localFolderIcon());
    TEST_CHECK(listed[1].icon == luciole::localLinkIcon() && listed[2].icon == luciole::localLinkIcon());
    for (const auto index : {0U, 1U, 2U}) {
      const auto& pending = listed[index];
      TEST_CHECK(pending.metadataPending && !listed.known(index) && pending.queriedDetails == 0);
      TEST_CHECK(!pending.size && !pending.identity() && !pending.access.read && !pending.access.write);
    }
    DirectoryModel model;
    model.applySnapshot(data);
    rows(model, {1, 4, 2, 3});
    data.generation = 2;
    // Only symlinks can lead to folders.
    data.entries = listed.withDirectoryLinks({0, 2});
    TEST_CHECK(data.entries.sameCatalogue(listed) && !data.entries.sameDirectoryLinks(listed));
    TEST_CHECK(!data.entries.brief(0).linksToDirectory && data.entries.brief(2).linksToDirectory);
    TEST_CHECK(data.entries[2].icon == luciole::localFolderIcon() && data.entries[2].metadataPending);
    TEST_CHECK(listed[2].icon == luciole::localLinkIcon() && !listed[2].linksToDirectory);
    model.applySnapshot(data);
    rows(model, {1, 3, 4, 2});
    // Detailed facts never follow links, so they keep the catalogue's target kind.
    auto fact = entry(3, "c-link", luciole::FileType::Symlink);
    fact.symlink = true;
    fact.icon = std::make_shared<const luciole::FileIconSource>(
        luciole::FileIconSource{.names = {"fixture-custom-folder"}, .filePath = {}, .desktopFile = {}}
    );
    const auto linked = data.entries;
    data.generation = 3;
    data.entries = linked.withFacts({{2, fact}});
    TEST_CHECK(data.entries.known(2)->linksToDirectory && data.entries.sameDirectoryLinks(linked));
    TEST_CHECK(data.entries.known(2)->icon == fact.icon && !data.entries.known(2)->metadataPending);
    TEST_CHECK(data.entries.withoutFacts().brief(2).linksToDirectory);
    TEST_CHECK(data.entries.withoutFacts()[2].icon == luciole::localFolderIcon());
    model.applySnapshot(data);
    rows(model, {1, 3, 4, 2});
    TEST_CHECK(!data.entries.withDirectoryLinks({}).known(2)->linksToDirectory);
  }

  void testSortAndMetadata() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(10, "item10"), entry(2, "item2"), entry(1, "item1"), entry(30, "z-folder", luciole::FileType::Directory),
         entry(20, "a-folder", luciole::FileType::Directory)}
    );
    model.applySnapshot(data);
    rows(model, {20, 30, 1, 2, 10});
    model.setSort({.descending = true});
    rows(model, {30, 20, 10, 2, 1});
    model.setSort({.directoriesFirst = false});
    rows(model, {20, 1, 2, 10, 30});

    // A link to a folder sorts with the folders; a file link stays with the files.
    data.entries = {
        entry(1, "b-folder", luciole::FileType::Directory), entry(2, "a-link", luciole::FileType::Symlink),
        entry(3, "c-link", luciole::FileType::Symlink), entry(4, "a-file")
    };
    test::editEntries(data, [&](auto& entries) {
      for (auto& item : entries) {
        item.symlink = item.type == luciole::FileType::Symlink;
      }
      entries[2].linksToDirectory = true;
    });
    model.setSort({});
    model.applySnapshot(data);
    rows(model, {1, 3, 4, 2});
    model.setSort({.directoriesFirst = false});
    rows(model, {4, 2, 1, 3});

    data.entries = {entry(1, "a"), entry(2, "b"), entry(3, "c")};
    test::editEntries(data, [&](auto& entries) {
      entries[0].size = 0;
      entries[0].modifiedSeconds = 0;
      entries[1].size = 5;
      entries[1].modifiedSeconds = 20;
    });
    model.applySnapshot(data);
    for (const auto field : {SortField::Size, SortField::Modified}) {
      model.setSort({.field = field});
      rows(model, {1, 2, 3});
      model.setSort({.field = field, .descending = true});
      rows(model, {2, 1, 3});
    }

    data.entries = {
        entry(1, "b.txt"), entry(2, "z-folder", luciole::FileType::Directory), entry(3, ".c.txt"),
        entry(4, ".a-folder", luciole::FileType::Directory)
    };
    test::editEntries(data, [&](auto& entries) {
      entries[0].size = 1;
      entries[2].size = 100;
      entries[2].hidden = entries[3].hidden = true;
    });
    model.applySnapshot(data);
    model.setFilter({.showHidden = true, .text = {}});
    model.setSort({.field = SortField::Size, .descending = true, .directoriesFirst = false});
    rows(model, {3, 1, 2, 4});
    model.setSort({.hiddenLast = true});
    rows(model, {2, 1, 4, 3});
    model.setSort({.descending = true, .directoriesFirst = false, .hiddenLast = true});
    rows(model, {2, 1, 3, 4});
    model.setSort({.field = SortField::Size, .descending = true, .directoriesFirst = false, .hiddenLast = true});
    rows(model, {1, 2, 3, 4});
    model.setFilter({.showHidden = false, .text = {}});

    // Collation ties must not let enumeration order determine action targets.
    data.entries = {entry(5, "same-b"), entry(4, "same-a")};
    test::editEntries(data, [&](auto& entries) {
      for (auto& item : entries) {
        item.setDisplayName("Same");
      }
    });
    model.setSort({});
    model.applySnapshot(data);
    const auto order = model.rows();
    test::editEntries(data, [&](auto& entries) { std::ranges::reverse(entries); });
    model.applySnapshot(data);
    TEST_CHECK(model.rows() == order);
    TEST_CHECK(model.entry(4)->name == "same-a");
  }

  void testExtendedMetadataSort() {
    auto a = entry(1, "a"), b = entry(2, "b"), unknown = entry(3, "unknown");
    a.details = {.accessedSeconds = 0, .createdSeconds = 5, .owner = "alpha", .group = "group-a", .mode = 0};
    b.details = {.accessedSeconds = 10, .createdSeconds = 20, .owner = "beta", .group = "group-b", .mode = 0644};
    DirectoryModel model;
    model.applySnapshot(snapshot({unknown, b, a}));
    model.select(1, SelectionMode::Replace);
    for (const auto field :
         {SortField::Accessed, SortField::Created, SortField::Owner, SortField::Group, SortField::Permissions}) {
      model.setSort({field, false, false});
      rows(model, {1, 2, 3});
      model.setSort({field, true, false});
      rows(model, {2, 1, 3});
      TEST_CHECK(model.selection().selected.contains(1));
    }
    a.place = Location::fromPath("/tmp/a/file");
    b.place = Location::fromPath("/tmp/b/file");
    model.applySnapshot(snapshot({b, a}));
    model.setSort({SortField::Location, false, false});
    rows(model, {1, 2});
    model.setSort({SortField::Location, true, false});
    rows(model, {2, 1});
    const auto columns = luciole::DetailsColumns::forWidth(300, luciole::allDetailsColumns());
    TEST_CHECK(columns.contentWidth > 1500 && columns.nameWidth == 160);
    for (const auto column : luciole::kDefaultDetailsColumnOrder) {
      TEST_CHECK(columns.visible(column) && columns.width(column) >= 56);
    }
    const auto reordered = luciole::insertDetailsColumnBefore(
        luciole::kDefaultDetailsColumnOrder, luciole::DetailsColumn::Owner, luciole::DetailsColumn::Type
    );
    TEST_CHECK(reordered && reordered->front() == luciole::DetailsColumn::Owner);
  }

  void testTextSortRefresh() {
    auto a = entry(1, "item2"), b = entry(2, "item10"), c = entry(4, "item1"), unknown = entry(3, "zzz");
    a.details.owner = "beta";
    b.details.owner = c.details.owner = "alpha";
    a.details.group = c.details.group = "group2";
    b.details.group = "group10";
    a.place = Location::fromPath("/tmp/z/item2");
    b.place = Location::fromPath("/tmp/a/item10");
    c.place = Location::fromPath("/tmp/a/item1");
    unknown.place = Location::fromPath("/");
    a.contentType = "text/plain";
    b.contentType = c.contentType = "image/png";
    DirectoryModel model;
    model.applySnapshot(snapshot({unknown, b, c, a}));
    TEST_CHECK(model.select(2) && model.setScrollAnchor(2));
    const auto check = [&](SortField field, const std::vector<EntryId>& ascending,
                           const std::vector<EntryId>& descending) {
      model.setSort({field, false, false});
      rows(model, ascending);
      model.setSort({field, true, false});
      rows(model, descending);
      // A different key pool may have been selected since the prior visit to this field.
      model.setSort({SortField::Size, false, false});
      model.setSort({field, false, false});
      rows(model, ascending);
      TEST_CHECK(model.selection().focused == 2 && model.selection().anchor == 2 && model.scrollAnchor() == 2);
      selected(model, {2});
    };
    check(SortField::Owner, {4, 2, 1, 3}, {1, 2, 4, 3});
    check(SortField::Group, {4, 1, 2, 3}, {2, 1, 4, 3});
    check(SortField::Type, {1, 4, 2, 3}, {2, 4, 1, 3});
    check(SortField::Location, {4, 2, 1, 3}, {1, 2, 4, 3});

    a.details.owner = {};
    b.details.owner = "zeta";
    unknown.details.owner = ""; // Empty is known, not missing metadata.
    b.details.group = "group1";
    c.details.group = {};
    unknown.details.group = "";
    b.setDisplayName("item0");
    b.place = Location::fromPath("/tmp/0/item10");
    a.contentType = unknown.contentType = "image/png";
    b.contentType = "application/octet-stream";
    c.contentType = "text/plain";
    // Refresh while Location is already selected, with different source indices and parent values.
    model.applySnapshot(snapshot({a, c, b, unknown}, 2));
    rows(model, {2, 4, 1, 3});
    check(SortField::Owner, {3, 4, 2, 1}, {2, 4, 3, 1});
    check(SortField::Group, {3, 2, 1, 4}, {1, 2, 3, 4});
    check(SortField::Type, {4, 1, 3, 2}, {3, 1, 4, 2});
    check(SortField::Name, {2, 4, 1, 3}, {3, 1, 4, 2});
    TEST_CHECK(model.entry(2)->name == "item10" && model.entry(2)->displayName() == "item0");
    // Removing IDs and adding a new row must not retain an old presentation by source index.
    auto replacement = entry(9, "item3");
    replacement.details.owner = "alpha";
    model.applySnapshot(snapshot({replacement, b}, 3));
    TEST_CHECK(model.entry(1) == nullptr && model.entry(3) == nullptr && model.entry(4) == nullptr);
    rows(model, {2, 9});
    model.setSort({SortField::Owner, false, false});
    rows(model, {9, 2});
  }

  void testTextSortKeyPoolLifetime() {
    constexpr EntryId count = 512;
    std::vector<DirectoryEntry> entries;
    entries.reserve(count);
    for (EntryId id = 1; id <= count; ++id) {
      auto item = entry(id, "café-" + std::to_string(id));
      item.details.owner = "owner" + std::to_string(id);
      item.details.group = "group" + std::to_string(count + 1 - id);
      item.place = Location::fromPath("/tmp/folder" + std::to_string(id) + "/item");
      entries.push_back(std::move(item));
    }
    std::ranges::reverse(entries);
    DirectoryModel model;
    model.applySnapshot(snapshot(std::move(entries)));
    for (int pass = 0; pass < 2; ++pass) {
      for (const auto field : {SortField::Owner, SortField::Group, SortField::Location}) {
        for (const bool descending : {false, true}) {
          model.setSort({field, descending, false});
          const bool reverse = descending != (field == SortField::Group);
          for (std::size_t i = 0; i < count; ++i) {
            TEST_CHECK(model.rows()[i] == (reverse ? count - i : i + 1));
          }
          model.setFilter({.showHidden = false, .text = "CAFÉ"});
          TEST_CHECK(model.rows().size() == count);
          model.setFilter({});
        }
      }
      auto refreshed = model.snapshot();
      ++refreshed.generation;
      test::editEntries(refreshed, [&](auto& entries) { std::ranges::reverse(entries); });
      model.applySnapshot(std::move(refreshed));
    }
  }

  void testDetailsLayout() {
    const auto wide = luciole::DetailsColumns::forWidth(1000);
    TEST_CHECK(wide.contentWidth == 1000 && wide.nameWidth == 566 && wide.showType);
    const auto ultrawide = luciole::DetailsColumns::forWidth(4000);
    TEST_CHECK(ultrawide.nameWidth == 3566 && ultrawide.contentWidth == 4000);
    const auto bounded = luciole::DetailsColumns::forWidth(100000);
    TEST_CHECK(luciole::validDetailsColumnWidth(0, static_cast<int>(bounded.nameWidth)));
    const luciole::DetailsColumnWidths requested{400, 200, 96, 180, 156, 156, 128, 128, 112, 260};
    const auto narrow = luciole::DetailsColumns::forWidth(300, {}, requested);
    const auto expanded = luciole::DetailsColumns::forWidth(1600, {}, requested);
    TEST_CHECK(
        narrow == expanded && narrow.contentWidth == 884 && narrow.showType && narrow.showSize && narrow.showModified
    );
    const auto hidden = luciole::DetailsColumns::forWidth(300, {false, false, false}, requested);
    TEST_CHECK(hidden.contentWidth == 408 && hidden.nameWidth == 400);
    const auto automatic = luciole::DetailsColumns::forWidth(300);
    TEST_CHECK(automatic.nameWidth == 160 && automatic.contentWidth == 594 && automatic.showType);
  }

  void testDetailsColumnMoves() {
    using Column = luciole::DetailsColumn;
    const auto defaults = luciole::kDefaultDetailsColumnOrder;
    TEST_CHECK(
        !luciole::moveDetailsColumn(defaults, Column::Type, -1, {})
        && !luciole::moveDetailsColumn(defaults, Column::Modified, 1, {})
    );
    TEST_CHECK(
        (luciole::moveDetailsColumn(defaults, Column::Type, 1, {})
         == luciole::DetailsColumnOrder{
             Column::Size, Column::Type, Column::Modified, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        (luciole::moveDetailsColumn(defaults, Column::Modified, -1, {})
         == luciole::DetailsColumnOrder{
             Column::Type, Column::Modified, Column::Size, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        (luciole::moveDetailsColumn(defaults, Column::Type, 1, {.size = false})
         == luciole::DetailsColumnOrder{
             Column::Modified, Column::Size, Column::Type, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        !luciole::moveDetailsColumn(defaults, Column::Type, -1, {.type = false})
        && !luciole::moveDetailsColumn(defaults, Column::Type, 1, {.size = false, .modified = false})
        && !luciole::moveDetailsColumn(defaults, Column::Type, 0, {})
        && !luciole::moveDetailsColumn({Column::Type, Column::Type, Column::Size}, Column::Type, 1, {})
    );
  }

  void testDetailsColumnInsertion() {
    using Column = luciole::DetailsColumn;
    const auto defaults = luciole::kDefaultDetailsColumnOrder;
    TEST_CHECK(
        (luciole::insertDetailsColumnBefore(defaults, Column::Type, std::nullopt)
         == luciole::DetailsColumnOrder{
             Column::Size, Column::Modified, Column::Accessed, Column::Created, Column::Owner, Column::Group,
             Column::Permissions, Column::Location, Column::Type
         })
    );
    TEST_CHECK(
        (luciole::insertDetailsColumnBefore(defaults, Column::Modified, Column::Type)
         == luciole::DetailsColumnOrder{
             Column::Modified, Column::Type, Column::Size, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        luciole::insertDetailsColumnBefore(defaults, Column::Type, Column::Type) == defaults
        && luciole::insertDetailsColumnBefore(defaults, Column::Size, Column::Modified) == defaults
    );
    TEST_CHECK(
        !luciole::insertDetailsColumnBefore(defaults, static_cast<Column>(99), std::nullopt)
        && !luciole::insertDetailsColumnBefore(defaults, Column::Type, static_cast<Column>(99))
        && !luciole::insertDetailsColumnBefore({Column::Type, Column::Type, Column::Size}, Column::Type, std::nullopt)
    );
  }

  void testTypeSort() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(10, "z-folder", luciole::FileType::Directory), entry(2, "link"), entry(3, "note10.txt"),
         entry(4, "note2.txt"), entry(5, "z.png"), entry(6, "a.bin"), entry(7, "b"),
         entry(8, "special", luciole::FileType::Special), entry(9, "c", luciole::FileType::Unknown)}
    );
    test::editEntries(data, [&](auto& entries) {
      entries[1].symlink = true;
      entries[1].contentType = "image/png";
      entries[2].contentType = "text/plain";
      entries[3].contentType = "text/plain";
      entries[4].contentType = "image/png";
      entries[5].contentType = "application/octet-stream";
    });
    model.applySnapshot(data);
    model.select(3);
    model.select(5, SelectionMode::Toggle);
    model.setScrollAnchor(3);
    model.setSort({.field = SortField::Type, .directoriesFirst = false});
    rows(model, {10, 2, 4, 3, 5, 8, 6, 7, 9});
    TEST_CHECK(
        model.typeLabel(10) == "Folder"
        && model.typeLabel(2) == "Link"
        && model.typeLabel(3) == model.typeLabel(4)
        && model.typeLabel(3) != model.typeLabel(5)
        && model.typeLabel(6) == "File"
        && model.typeLabel(9) == "Unknown"
    );
    model.setSort({.field = SortField::Type, .descending = true, .directoriesFirst = false});
    rows(model, {8, 5, 3, 4, 2, 10, 9, 7, 6});
    selected(model, {5, 3});
    TEST_CHECK(model.selection().focused == 5 && model.selection().anchor == 5 && model.scrollAnchor() == 3);
    test::editEntries(data, [&](auto& entries) { std::ranges::reverse(entries); });
    model.applySnapshot(data);
    rows(model, {8, 5, 3, 4, 2, 10, 9, 7, 6});
    model.setSort({.field = SortField::Type, .descending = true});
    rows(model, {10, 8, 5, 3, 4, 2, 9, 7, 6});
    test::editEntries(data, [&](auto& entries) {
      for (auto& item : entries) {
        if (item.id == 3) {
          item.contentType = "image/png";
        }
      }
    });
    model.applySnapshot(data);
    TEST_CHECK(model.typeLabel(3) == model.typeLabel(5));
    selected(model, {5, 3});
  }

  void testSelectionSummary() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(1, "empty"), entry(2, "known"), entry(3, "unknown"), entry(4, "folder", luciole::FileType::Directory),
         entry(5, "link"), entry(6, "special", luciole::FileType::Special), entry(7, ".hidden")}
    );
    test::editEntries(data, [&](auto& entries) {
      entries[0].size = 0;
      entries[1].size = 5;
      entries[3].size = 4096;
      entries[4].symlink = true;
      entries[4].size = 900;
      entries[5].size = 777;
      entries[6].hidden = true;
      entries[6].size = 12;
    });
    model.applySnapshot(data);
    model.selectAll();
    TEST_CHECK(!model.selection().focused);
    const luciole::SelectionSummary expected{.files = 3, .folders = 1, .others = 2, .bytes = 5, .unknownSizes = 1};
    TEST_CHECK(model.selectionSummary() == expected);
    model.setFilter({.showHidden = true, .text = {}});
    model.selectAll();
    TEST_CHECK(model.selectionSummary().bytes == 17 && model.selectionSummary().files == 4);
    model.setFilter({});
    TEST_CHECK(model.selectionSummary() == expected);
    test::editEntries(data, [&](auto& entries) { entries[1].size = 10; });
    model.applySnapshot(data);
    TEST_CHECK(model.selectionSummary().bytes == 10);
    TEST_CHECK(model.replaceSelection(std::array<EntryId, 2>{1, 2}));
    TEST_CHECK((model.selectionSummary() == luciole::SelectionSummary{.files = 2, .bytes = 10}));
    data.status = DirectoryStatus::Cancelled;
    model.applySnapshot(data);
    TEST_CHECK(model.selectionSummary() == luciole::SelectionSummary{});
    auto huge = snapshot({entry(10, "large"), entry(11, "one")}, 2);
    test::editEntries(huge, [&](auto& entries) {
      entries[0].size = std::numeric_limits<std::uint64_t>::max();
      entries[1].size = 1;
    });
    model.applySnapshot(huge);
    model.selectAll();
    TEST_CHECK(
        (model.selectionSummary()
         == luciole::SelectionSummary{.files = 2, .bytes = std::numeric_limits<std::uint64_t>::max(), .overflow = true})
    );
    model.clearSelection();
    TEST_CHECK(model.selectionSummary() == luciole::SelectionSummary{});
  }

  void testFiltering() {
    DirectoryModel model;
    auto data = snapshot({entry(1, "Straße"), entry(2, "café"), entry(3, ".hidden"), entry(4, "*.txt")});
    test::editEntries(data, [&](auto& entries) { entries[2].hidden = true; });
    model.applySnapshot(data);
    TEST_CHECK(model.rows().size() == 3);
    model.setFilter({.text = "STRASSE"});
    rows(model, {1});
    model.setFilter({.text = "CAFE\u0301"});
    rows(model, {2});
    model.setFilter({.text = "*"});
    rows(model, {4});
    model.setFilter({.showHidden = true, .text = "hidden"});
    rows(model, {3});
    TEST_CHECK(model.entry(3)->name == ".hidden");
    TEST_CHECK(model.snapshot().entries.size() == 4);
    model.setFilter({.text = "absent"});
    rows(model, {});
    TEST_CHECK(model.snapshot().status == DirectoryStatus::Ready);
    model.setSort({.descending = true});
    model.setFilter({});
    rows(model, {1, 2, 4});
    model.setEntryFilter([](const auto& brief, const auto*) { return brief.id != 2; });
    rows(model, {1, 4});
    model.setFilter({.text = "STRASSE"});
    rows(model, {1});
    model.setEntryFilter({});
    model.setFilter({});
    rows(model, {1, 2, 4});
    model.applySnapshot(snapshot({entry(5, "z"), entry(6, "a"), entry(7, "m")}, 2));
    rows(model, {5, 7, 6});
    model.setFilter({.text = "m"});
    rows(model, {7});
    model.setSort({});
    model.setFilter({});
    rows(model, {6, 7, 5});
    model.setFilter({.text = "missing"});
    model.applySnapshot(snapshot({entry(8, "alpha"), entry(9, "alpine"), entry(10, "beta")}, 3));
    rows(model, {});
    model.setFilter({.text = "alpha"});
    rows(model, {8});
    model.setFilter({.text = "alp"});
    rows(model, {8, 9});
    model.setFilter({});
    rows(model, {8, 9, 10});
  }

  void testSelectionAndAnchors() {
    DirectoryModel model;
    model.applySnapshot(snapshot({entry(1, "a"), entry(2, "b"), entry(3, "c"), entry(4, "d")}));
    model.select(2);
    model.setScrollAnchor(2);
    model.setSort({.descending = true});
    selected(model, {2});
    TEST_CHECK(model.selection().focused == 2 && model.selection().anchor == 2 && model.scrollAnchor() == 2);
    model.select(4, SelectionMode::Range);
    selected(model, {4, 3, 2});
    TEST_CHECK(model.selection().anchor == 2 && model.selection().focused == 4);
    model.select(3, SelectionMode::Range);
    selected(model, {3, 2});
    model.select(1, SelectionMode::Toggle);
    selected(model, {3, 2, 1});
    model.select(1, SelectionMode::Toggle);
    selected(model, {3, 2});
    TEST_CHECK(model.selection().anchor == 1 && model.selection().focused == 1);
    model.select(4, SelectionMode::AddRange);
    selected(model, {4, 3, 2, 1});
    model.clearSelection();
    TEST_CHECK(model.selection().focused == 4 && !model.selection().anchor);
    model.setFocus(3);
    model.select(1, SelectionMode::Range);
    selected(model, {3, 2, 1});
    TEST_CHECK(model.selection().anchor == 3);
    TEST_CHECK(!model.select(999) && !model.setFocus(999) && !model.setScrollAnchor(999));
    selected(model, {3, 2, 1});
    model.setFilter({.text = "b"});
    selected(model, {2});
    TEST_CHECK(!model.selection().focused && !model.selection().anchor);
    TEST_CHECK(model.scrollAnchor() == 2);
    TEST_CHECK(!model.select(1));
    model.selectAll();
    selected(model, {2});
    model.setFilter({});
    selected(model, {2});
  }

  void testBulkSelection() {
    DirectoryModel model;
    model.applySnapshot(snapshot({entry(1, "a"), entry(2, "b"), entry(3, "c")}));
    int publications = 0;
    model.setChangedCallback([&] { ++publications; });
    const std::array initial{luciole::EntryId{1}, luciole::EntryId{3}, luciole::EntryId{1}};
    TEST_CHECK(model.replaceSelection(initial));
    selected(model, {1, 3});
    TEST_CHECK(publications == 1 && model.selection().anchor == 1 && model.selection().focused == 1);
    const std::array invalid{luciole::EntryId{2}, luciole::EntryId{999}};
    TEST_CHECK(!model.replaceSelection(invalid));
    selected(model, {1, 3});
    TEST_CHECK(publications == 1);
    const std::array reduced{luciole::EntryId{3}};
    TEST_CHECK(model.replaceSelection(reduced));
    selected(model, {3});
    model.setSort({.descending = true});
    TEST_CHECK(model.replaceSelection(initial));
    selected(model, {3, 1});
    model.setFilter({.text = "c"});
    TEST_CHECK(!model.replaceSelection(initial));
    selected(model, {3});
    TEST_CHECK(model.replaceSelection({}) && !model.selection().anchor);
    selected(model, {});
    auto stopped = model.snapshot();
    stopped.status = DirectoryStatus::Cancelled;
    model.applySnapshot(stopped);
    TEST_CHECK(!model.replaceSelection(reduced));
  }

  void testSingleSelectionAndEntryFilter() {
    DirectoryModel model;
    model.applySnapshot(snapshot({entry(1, "a.txt"), entry(2, "b.png"), entry(3, "c.txt")}));
    TEST_CHECK(model.select(1) && model.select(3, SelectionMode::Toggle));
    selected(model, {1, 3});
    model.setSingleSelection(true);
    TEST_CHECK(model.singleSelection());
    selected(model, {1});
    TEST_CHECK(model.select(3, SelectionMode::Toggle));
    selected(model, {3});
    TEST_CHECK(model.select(3, SelectionMode::Toggle) && model.selection().focused == 3);
    selected(model, {});
    TEST_CHECK(model.select(1, SelectionMode::Range));
    selected(model, {1});
    model.selectAll();
    selected(model, {1});
    TEST_CHECK(!model.invertSelection());
    TEST_CHECK(model.selectPattern("*") == "Only one item can be chosen.");
    const std::array many{luciole::EntryId{3}, luciole::EntryId{1}};
    TEST_CHECK(model.replaceSelection(many));
    selected(model, {3});

    int publications = 0;
    model.setChangedCallback([&] { ++publications; });
    model.setEntryFilter([](const luciole::EntryBrief& brief, const DirectoryEntry*) {
      return !brief.name.ends_with(".png");
    });
    rows(model, {1, 3});
    TEST_CHECK(publications == 1);
    model.applySnapshot(snapshot({entry(1, "a.txt"), entry(2, "b.png"), entry(4, "d.png")}, 2));
    rows(model, {1});
    model.setEntryFilter({});
    rows(model, {1, 2, 4});
  }

  void testSnapshotReconciliation() {
    DirectoryModel model;
    auto data = snapshot({entry(1, "a"), entry(2, "b")}, 10);
    model.applySnapshot(data);
    model.select(1);
    model.setScrollAnchor(1);
    auto loading = data;
    loading.generation = 11;
    loading.status = DirectoryStatus::Loading;
    model.applySnapshot(loading);
    selected(model, {1});
    TEST_CHECK(!model.applySnapshot(data));
    TEST_CHECK(model.snapshot().status == DirectoryStatus::Loading);
    data.generation = 11;
    test::editEntries(data, [&](auto& entries) { entries[0] = entry(1, "z-renamed"); });
    model.applySnapshot(data);
    selected(model, {1});
    TEST_CHECK(model.entry(1)->name == "z-renamed" && model.scrollAnchor() == 1);
    data.generation = 12;
    test::editEntries(data, [&](auto& entries) { entries[0] = entry(3, "z-renamed"); });
    model.applySnapshot(data);
    selected(model, {2});
    TEST_CHECK(model.selection().focused == 2 && model.selection().anchor == 2 && !model.scrollAnchor());
    model.select(2);
    auto cancelled = data;
    cancelled.generation = 13;
    cancelled.status = DirectoryStatus::Cancelled;
    model.applySnapshot(cancelled);
    selected(model, {});
    TEST_CHECK(!model.select(2));
    cancelled.generation = 14;
    cancelled.status = DirectoryStatus::Loading;
    model.applySnapshot(cancelled);
    TEST_CHECK(!model.select(2));
    cancelled.status = DirectoryStatus::Error;
    cancelled.error = luciole::FileError{.kind = luciole::FileErrorKind::Denied, .message = "partial fixture"};
    model.applySnapshot(cancelled);
    TEST_CHECK(model.snapshot().error && model.select(2));
    auto other = snapshot({entry(2, "other")}, 15);
    other.location = Location::fromPath("/tmp/another-model-location");
    model.applySnapshot(other);
    selected(model, {});
    TEST_CHECK(!model.selection().focused);
    auto dying = std::make_unique<DirectoryModel>();
    dying->setChangedCallback([&dying] { dying.reset(); });
    dying->applySnapshot(other);
  }

  void testSelectionAfterRemoval() {
    const auto initial = snapshot({entry(1, "a"), entry(2, "b"), entry(3, "c"), entry(4, "d")});
    DirectoryModel model;
    model.applySnapshot(initial);
    TEST_CHECK(model.select(2));
    model.applySnapshot(snapshot({entry(1, "a"), entry(3, "c"), entry(4, "d")}, 2));
    selected(model, {3});
    TEST_CHECK(model.selection().focused == 3 && model.selection().anchor == 3);
    model.applySnapshot(snapshot({entry(1, "a"), entry(4, "d")}, 3));
    selected(model, {4});
    model.applySnapshot(snapshot({entry(1, "a")}, 4));
    selected(model, {1});
    model.applySnapshot(snapshot({}, 5));
    selected(model, {});
    TEST_CHECK(!model.selection().focused && !model.selection().anchor);

    DirectoryModel descending;
    descending.setSort({.descending = true});
    descending.applySnapshot(initial);
    TEST_CHECK(descending.select(3));
    descending.applySnapshot(snapshot({entry(1, "a"), entry(2, "b"), entry(4, "d")}, 2));
    selected(descending, {2});

    DirectoryModel batch;
    batch.applySnapshot(initial);
    TEST_CHECK(batch.select(2) && batch.select(3, SelectionMode::Range));
    batch.applySnapshot(snapshot({entry(1, "a"), entry(2, "b"), entry(4, "d")}, 2));
    selected(batch, {2});
    TEST_CHECK(batch.selection().focused == 2);
    batch.applySnapshot(snapshot({entry(1, "a"), entry(4, "d")}, 3));
    selected(batch, {4});

    DirectoryModel together;
    together.applySnapshot(initial);
    TEST_CHECK(together.select(2) && together.select(3, SelectionMode::Range));
    together.applySnapshot(snapshot({entry(1, "a"), entry(4, "d")}, 2));
    selected(together, {4});

    DirectoryModel inserted;
    inserted.applySnapshot(initial);
    TEST_CHECK(inserted.select(2));
    inserted.applySnapshot(snapshot({entry(1, "a"), entry(99, "bb"), entry(3, "c"), entry(4, "d")}, 2));
    selected(inserted, {3});

    DirectoryModel filtered;
    filtered.applySnapshot(snapshot({entry(1, "match-a"), entry(2, "match-b"), entry(3, "other")}));
    filtered.setFilter({.text = "match"});
    TEST_CHECK(filtered.select(2));
    filtered.applySnapshot(snapshot({entry(1, "match-a"), entry(3, "other")}, 2));
    selected(filtered, {1});
    filtered.setFilter({.text = "other"});
    selected(filtered, {});
    TEST_CHECK(!filtered.selection().focused);

    DirectoryModel hidden;
    hidden.applySnapshot(initial);
    TEST_CHECK(hidden.select(2));
    auto hiddenEntry = entry(2, ".hidden");
    hiddenEntry.hidden = true;
    hidden.applySnapshot(snapshot({entry(1, "a"), hiddenEntry, entry(3, "c"), entry(4, "d")}, 2));
    selected(hidden, {});
    TEST_CHECK(!hidden.selection().focused);

    DirectoryModel unselected;
    unselected.applySnapshot(initial);
    TEST_CHECK(unselected.select(2));
    unselected.clearSelection();
    unselected.applySnapshot(snapshot({entry(1, "a"), entry(3, "c"), entry(4, "d")}, 2));
    selected(unselected, {});
    TEST_CHECK(unselected.selection().focused == 3);

    DirectoryModel changedChoice;
    changedChoice.applySnapshot(initial);
    TEST_CHECK(changedChoice.select(2) && changedChoice.select(4));
    changedChoice.applySnapshot(snapshot({entry(1, "a"), entry(3, "c"), entry(4, "d")}, 2));
    selected(changedChoice, {4});

    DirectoryModel loading;
    loading.applySnapshot(initial);
    TEST_CHECK(loading.select(2));
    auto refresh = initial;
    refresh.status = DirectoryStatus::Loading;
    refresh.generation = 2;
    loading.applySnapshot(refresh);
    selected(loading, {2});
    loading.applySnapshot(snapshot({entry(1, "a"), entry(3, "c"), entry(4, "d")}, 2));
    selected(loading, {3});
    auto partial = snapshot({entry(1, "a"), entry(4, "d")}, 3);
    partial.status = DirectoryStatus::Error;
    partial.error = luciole::FileError{.kind = luciole::FileErrorKind::Denied, .message = "partial fixture"};
    loading.applySnapshot(partial);
    selected(loading, {});
    TEST_CHECK(!loading.selection().focused);

    DirectoryModel incremental;
    incremental.applySnapshot(initial);
    TEST_CHECK(incremental.select(2));
    auto fresh = snapshot({entry(1, "a")}, 2);
    fresh.status = DirectoryStatus::Loading;
    fresh.incrementalResults = true;
    incremental.applySnapshot(fresh);
    selected(incremental, {});
    incremental.applySnapshot(snapshot({entry(1, "a"), entry(3, "c"), entry(4, "d")}, 2));
    selected(incremental, {});

    DirectoryModel tree;
    tree.applySnapshot(
        snapshot({entry(1, "folder", luciole::FileType::Directory), entry(2, "a"), entry(3, "b"), entry(4, "other")}),
        {{2, 1}, {3, 1}}
    );
    TEST_CHECK(tree.select(2));
    tree.applySnapshot(
        snapshot({entry(1, "folder", luciole::FileType::Directory), entry(3, "b"), entry(4, "other")}, 2), {{3, 1}}
    );
    selected(tree, {3});
    tree.applySnapshot(snapshot({entry(1, "folder", luciole::FileType::Directory), entry(4, "other")}, 3));
    selected(tree, {});
    TEST_CHECK(!tree.selection().focused);
  }

  struct Fixture {
    std::filesystem::path path;
    Fixture() {
      const luciole::GString directory(g_dir_make_tmp("luciole-model-test-XXXXXX", nullptr));
      TEST_CHECK(directory != nullptr);
      path = directory.get();
    }
    ~Fixture() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    void write(const std::string& name) const {
      std::ofstream file(path / name);
      file << "fixture";
      TEST_CHECK(file.good());
    }
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        const int timeout = requested < 0 ? 20 : std::min(requested, 20);
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };

  void testDirectoryServiceIntegration() {
    Fixture fixture;
    const std::string name = "item2#%\n" + std::string(1, static_cast<char>(0xFF));
    fixture.write(name);
    fixture.write("item10");
    Pump pump;
    DirectoryModel model;
    luciole::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(Location::fromPath(fixture.path.native()));
    pump.until([&] { return model.snapshot().status == DirectoryStatus::Ready; });
    TEST_CHECK(model.rows().size() == 2);
    const auto id = model.rows().front();
    const auto otherId = model.rows().back();
    TEST_CHECK(model.entry(id)->name == name);
    model.select(id);
    model.setScrollAnchor(id);
    std::filesystem::rename(fixture.path / name, fixture.path / "item20");
    pump.until([&] { return model.entry(id) && model.entry(id)->name == "item20"; });
    rows(model, {otherId, id});
    selected(model, {id});
    TEST_CHECK(model.selection().focused == id && model.scrollAnchor() == id);
    session.refresh();
    selected(model, {id});
    pump.until([&] { return model.snapshot().status == DirectoryStatus::Ready; });
    selected(model, {id});
    std::filesystem::remove(fixture.path / "item20");
    pump.until([&] { return model.entry(id) == nullptr; });
    selected(model, {otherId});
    fixture.write("item20");
    pump.until([&] { return model.rows().size() == 2; });
    TEST_CHECK(model.entry(id) == nullptr && model.selection().selected.contains(otherId));
  }
} // namespace

int main() {
  try {
    // MIME descriptions consult language environment variables independently of collation.
    g_setenv("LC_ALL", "C", true);
    g_setenv("LANGUAGE", "C", true);
    std::setlocale(LC_ALL, "C");
    testPublishedEntryOwnership();
    testCatalogueProjectionAndPendingFacts();
    testCatalogueDirectoryLinks();
    testSortAndMetadata();
    testExtendedMetadataSort();
    testTextSortRefresh();
    testTextSortKeyPoolLifetime();
    testDetailsLayout();
    testDetailsColumnMoves();
    testDetailsColumnInsertion();
    testTypeSort();
    testSelectionSummary();
    testFiltering();
    testBackupVisibility();
    testSelectionAndAnchors();
    testBulkSelection();
    testSingleSelectionAndEntryFilter();
    testSnapshotReconciliation();
    testSelectionAfterRemoval();
    testDirectoryServiceIntegration();
    std::println("directory model checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "directory model test: {}", error.what());
    return 1;
  }
}
