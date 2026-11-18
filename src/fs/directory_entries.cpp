#include "fs/directory_entries.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {
  // Reads one decimal number up to `end` (or the text's end when end is '\0'), advancing `text`.
  bool readNumber(std::string_view& text, char end, std::uint64_t& value) {
    const auto stop = end == '\0' ? text.size() : text.find(end);
    if (stop == std::string_view::npos || stop == 0) {
      return false;
    }
    const auto [rest, error] = std::from_chars(text.data(), text.data() + stop, value);
    if (error != std::errc{} || rest != text.data() + stop) {
      return false;
    }
    text.remove_prefix(end == '\0' ? stop : stop + 1);
    return true;
  }

  std::string localEtag(std::uint64_t seconds, std::uint32_t nanoseconds) {
    return std::format("{}:{}:{}", seconds, nanoseconds / 1000, nanoseconds);
  }

  std::string localFilesystem(std::uint64_t device) { return std::format("l{}", device); }
  std::string localFile(std::uint64_t device, std::uint64_t inode) { return std::format("l{}:{}", device, inode); }
} // namespace

luciole::EntryPlace::EntryPlace(Location location)
    : m_location(std::make_shared<const Location>(std::move(location))) {}

luciole::EntryPlace luciole::EntryPlace::inFolder(std::shared_ptr<const Location> folder) {
  EntryPlace place;
  place.m_location = std::move(folder);
  place.m_inFolder = true;
  return place;
}

luciole::Location luciole::EntryPlace::resolve(const std::string& name) const {
  if (!m_location) {
    throw std::logic_error("directory entry without a location");
  }
  return m_inFolder ? m_location->child(name) : *m_location;
}

const std::string& luciole::DirectoryEntry::displayName() const {
  return extra && extra->displayName ? *extra->displayName : name;
}

const luciole::Location* luciole::DirectoryEntry::shortcutTarget() const {
  return extra && extra->target ? &*extra->target : nullptr;
}
bool luciole::DirectoryEntry::isFolderLink() const {
  return type == FileType::Directory || (type == FileType::Shortcut && shortcutTarget() != nullptr);
}
luciole::Location luciole::DirectoryEntry::navigationLocation() const {
  const auto* target = type == FileType::Shortcut ? shortcutTarget() : nullptr;
  return target ? *target : location();
}
void luciole::DirectoryEntry::setShortcutTarget(std::optional<Location> target) {
  if (target || (extra && extra->target)) {
    editExtra().target = std::move(target);
  }
}

std::optional<std::string> luciole::DirectoryEntry::etag() const {
  if (tags.etag) {
    return localEtag(tags.etagSeconds, tags.etagNanoseconds);
  }
  return extra ? extra->etag : std::nullopt;
}

std::optional<luciole::FileIdentity> luciole::DirectoryEntry::identity() const {
  if (tags.identity) {
    return FileIdentity{.filesystem = localFilesystem(tags.device), .file = localFile(tags.device, tags.inode)};
  }
  return extra ? extra->identity : std::nullopt;
}

const std::string& luciole::DirectoryEntry::parentDisplayName() const {
  static const std::string none;
  return extra ? extra->parentDisplayName : none;
}

const luciole::TrashInfo* luciole::DirectoryEntry::trash() const {
  return extra && extra->trash ? &*extra->trash : nullptr;
}

const luciole::TrashInfo& luciole::DirectoryEntry::trashInfo() const {
  static const TrashInfo none;
  const auto* info = trash();
  return info != nullptr ? *info : none;
}

luciole::EntryExtra& luciole::DirectoryEntry::editExtra() {
  // Extra facts are shared between copies; edits copy them first.
  auto copy = extra ? std::make_shared<EntryExtra>(*extra) : std::make_shared<EntryExtra>();
  auto& result = *copy;
  extra = std::move(copy);
  return result;
}

void luciole::DirectoryEntry::setDisplayName(std::string displayName) {
  if (displayName == name) {
    if (extra && extra->displayName) {
      editExtra().displayName.reset();
    }
    return;
  }
  editExtra().displayName = std::move(displayName);
}

void luciole::DirectoryEntry::setEtag(std::optional<std::string> etag) {
  tags.etag = false;
  if (etag) {
    std::string_view text = *etag;
    std::uint64_t seconds = 0, micro = 0, nano = 0;
    if (readNumber(text, ':', seconds)
        && readNumber(text, ':', micro)
        && readNumber(text, '\0', nano)
        && nano < 1'000'000'000
        && localEtag(seconds, static_cast<std::uint32_t>(nano)) == *etag) {
      tags.etag = true;
      tags.etagSeconds = seconds;
      tags.etagNanoseconds = static_cast<std::uint32_t>(nano);
      etag.reset();
    }
  }
  if (etag || (extra && extra->etag)) {
    editExtra().etag = std::move(etag);
  }
}

void luciole::DirectoryEntry::setIdentity(std::optional<FileIdentity> identity) {
  tags.identity = false;
  if (identity && identity->filesystem.starts_with('l') && identity->file.starts_with('l')) {
    std::string_view filesystem = std::string_view(identity->filesystem).substr(1);
    std::string_view file = std::string_view(identity->file).substr(1);
    std::uint64_t device = 0, fileDevice = 0, inode = 0;
    if (readNumber(filesystem, '\0', device)
        && readNumber(file, ':', fileDevice)
        && readNumber(file, '\0', inode)
        && device == fileDevice
        && localFilesystem(device) == identity->filesystem
        && localFile(device, inode) == identity->file) {
      tags.identity = true;
      tags.device = device;
      tags.inode = inode;
      identity.reset();
    }
  }
  if (identity || (extra && extra->identity)) {
    editExtra().identity = std::move(identity);
  }
}

void luciole::DirectoryEntry::setParentDisplayName(std::string text) {
  if (!text.empty() || (extra && !extra->parentDisplayName.empty())) {
    editExtra().parentDisplayName = std::move(text);
  }
}

void luciole::DirectoryEntry::setTrash(std::optional<TrashInfo> trash) {
  if (trash || (extra && extra->trash)) {
    editExtra().trash = std::move(trash);
  }
}
luciole::EntryBrief luciole::BrowseCatalogue::brief(std::size_t index) const {
  const auto& item = items[index];
  const auto name = std::string_view(names).substr(item.offset, item.length);
  const auto display = displayNames.find(index);
  return {item.id,      name, display == displayNames.end() ? name : display->second, item.type, item.hidden,
          item.symlink, false};
}

struct luciole::DirectoryEntries::Storage {
  struct Payload {
    std::vector<DirectoryEntry> entries;
    std::shared_ptr<const BrowseCatalogue> catalogue;
    std::unordered_map<std::size_t, DirectoryEntry> facts;
    // Catalogue indices of the symlinks that lead to folders.
    std::shared_ptr<const std::unordered_set<std::size_t>> directoryLinks;
    // Materializing a pending value never changes it. References live as long as this payload.
    mutable std::unordered_map<std::size_t, DirectoryEntry> pending;
    std::size_t size() const { return catalogue ? catalogue->items.size() : entries.size(); }
    const DirectoryEntry* known(std::size_t index) const {
      if (!catalogue) {
        return &entries[index];
      }
      const auto found = facts.find(index);
      return found == facts.end() ? nullptr : &found->second;
    }
    EntryBrief brief(std::size_t index) const {
      if (catalogue) {
        auto brief = catalogue->brief(index);
        brief.linksToDirectory = brief.symlink && directoryLinks && directoryLinks->contains(index);
        return brief;
      }
      const auto& entry = entries[index];
      return {entry.id,     entry.name,    entry.displayName(),   entry.type,
              entry.hidden, entry.symlink, entry.linksToDirectory};
    }
    const DirectoryEntry& entry(std::size_t index) const {
      if (const auto* entry = known(index)) {
        return *entry;
      }
      auto [found, inserted] = pending.try_emplace(index);
      if (inserted) {
        const auto item = brief(index);
        auto& entry = found->second;
        entry.id = item.id;
        entry.place = EntryPlace::inFolder(catalogue->folder);
        entry.name = item.name;
        entry.setDisplayName(std::string(item.displayName));
        entry.type = item.type;
        entry.hidden = item.hidden;
        entry.symlink = item.symlink;
        entry.linksToDirectory = item.linksToDirectory;
        entry.metadataPending = true;
        entry.queriedDetails = 0;
      }
      return found->second;
    }
  };
  struct Block {
    std::shared_ptr<const Payload> payload;
    std::size_t end;
  };
  struct Row {
    const Payload* payload;
    union {
      std::size_t index;
      const DirectoryEntry* direct;
    };
    const DirectoryEntry* known() const { return payload ? payload->known(index) : direct; }
    const DirectoryEntry& entry() const { return payload ? payload->entry(index) : *direct; }
    EntryBrief brief() const {
      return payload ? payload->brief(index)
                     : EntryBrief{direct->id,     direct->name,    direct->displayName(),   direct->type,
                                  direct->hidden, direct->symlink, direct->linksToDirectory};
    }
    Location location() const {
      return payload && payload->catalogue ? payload->catalogue->folder->child(std::string(payload->brief(index).name))
                                           : entry().location();
    }
  };
  std::vector<Block> blocks;
  std::vector<Row> rows;
  // Eager tree/search projections retain the original single-pointer-per-row representation.
  std::vector<const DirectoryEntry*> directRows;
  std::size_t size = 0;
  bool projected = false;
  Row row(std::size_t index) const {
    if (projected) {
      if (!directRows.empty()) {
        return {.payload = nullptr, .direct = directRows[index]};
      }
      return rows[index];
    }
    const auto block = blocks.size() == 1 ? blocks.begin() : std::ranges::upper_bound(blocks, index, {}, &Block::end);
    return {.payload = block->payload.get(), .index = index - (block->end - block->payload->size())};
  }
};

luciole::DirectoryEntries::DirectoryEntries(std::vector<DirectoryEntry> entries) {
  if (entries.empty()) {
    return;
  }
  auto storage = std::make_shared<Storage>();
  storage->size = entries.size();
  auto payload = std::make_shared<Storage::Payload>();
  payload->entries = std::move(entries);
  storage->blocks.push_back({std::move(payload), storage->size});
  m_storage = std::move(storage);
}

luciole::DirectoryEntries::DirectoryEntries(std::initializer_list<DirectoryEntry> entries)
    : DirectoryEntries(std::vector<DirectoryEntry>(entries)) {}

luciole::DirectoryEntries::DirectoryEntries(BrowseCatalogue catalogue) {
  auto storage = std::make_shared<Storage>();
  auto payload = std::make_shared<Storage::Payload>();
  storage->size = catalogue.items.size();
  payload->catalogue = std::make_shared<const BrowseCatalogue>(std::move(catalogue));
  storage->blocks.push_back({std::move(payload), storage->size});
  m_storage = std::move(storage);
}

std::size_t luciole::DirectoryEntries::size() const { return m_storage ? m_storage->size : 0; }
luciole::EntryBrief luciole::DirectoryEntries::brief(std::size_t index) const {
  if (!m_storage->directRows.empty()) {
    const auto& entry = *m_storage->directRows[index];
    return {entry.id, entry.name, entry.displayName(), entry.type, entry.hidden, entry.symlink, entry.linksToDirectory};
  }
  const auto row = m_storage->row(index);
  return row.brief();
}
luciole::Location luciole::DirectoryEntries::location(std::size_t index) const {
  if (!m_storage->directRows.empty()) {
    return m_storage->directRows[index]->location();
  }
  const auto row = m_storage->row(index);
  return row.location();
}
const luciole::DirectoryEntry* luciole::DirectoryEntries::known(std::size_t index) const {
  if (!m_storage->directRows.empty()) {
    return m_storage->directRows[index];
  }
  const auto row = m_storage->row(index);
  return row.known();
}
bool luciole::DirectoryEntries::isCatalogue() const {
  return m_storage
      && !m_storage->projected
      && m_storage->blocks.size() == 1
      && m_storage->blocks.front().payload->catalogue;
}
bool luciole::DirectoryEntries::sameCatalogue(const DirectoryEntries& other) const {
  return isCatalogue()
      && other.isCatalogue()
      && m_storage->blocks.front().payload->catalogue == other.m_storage->blocks.front().payload->catalogue;
}
bool luciole::DirectoryEntries::sameDirectoryLinks(const DirectoryEntries& other) const {
  return sameCatalogue(other)
      && m_storage->blocks.front().payload->directoryLinks == other.m_storage->blocks.front().payload->directoryLinks;
}
const luciole::BrowseCatalogue* luciole::DirectoryEntries::catalogue() const {
  return isCatalogue() ? m_storage->blocks.front().payload->catalogue.get() : nullptr;
}
std::optional<luciole::DirectoryEntries::CatalogueRow>
luciole::DirectoryEntries::catalogueRow(std::size_t index) const {
  if (!m_storage->directRows.empty()) {
    return std::nullopt;
  }
  const auto row = m_storage->row(index);
  return row.payload && row.payload->catalogue ? std::optional(CatalogueRow{row.payload->catalogue.get(), row.index})
                                               : std::nullopt;
}
luciole::DirectoryEntries
luciole::DirectoryEntries::withFacts(std::vector<std::pair<std::size_t, DirectoryEntry>> facts) const {
  if (!isCatalogue()) {
    throw std::logic_error("facts require a flat catalogue");
  }
  const auto& previous = *m_storage->blocks.front().payload;
  auto payload = std::make_shared<Storage::Payload>();
  payload->catalogue = previous.catalogue;
  payload->directoryLinks = previous.directoryLinks;
  // The session supplies the complete bounded working set, rather than retaining older revisions.
  for (auto& [index, entry] : facts) {
    if (index >= size() || entry.id != brief(index).id || entry.name != brief(index).name || entry.metadataPending) {
      throw std::logic_error("mismatched catalogue facts");
    }
    // Detailed facts never follow links, so the target kind stays the catalogue's.
    entry.linksToDirectory = brief(index).linksToDirectory;
    payload->facts.emplace(index, std::move(entry));
  }
  auto storage = std::make_shared<Storage>();
  storage->size = size();
  storage->blocks.push_back({std::move(payload), size()});
  return DirectoryEntries(std::move(storage));
}
luciole::DirectoryEntries luciole::DirectoryEntries::withoutFacts() const { return withFacts({}); }
luciole::DirectoryEntries
luciole::DirectoryEntries::withDirectoryLinks(const std::unordered_set<std::size_t>& links) const {
  if (empty()) {
    return *this;
  }
  if (m_storage->projected || m_storage->blocks.size() != 1) {
    throw std::logic_error("directory links require one unprojected listing");
  }
  const auto& previous = *m_storage->blocks.front().payload;
  if (!previous.catalogue) {
    auto entries = previous.entries;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      entries[index].linksToDirectory = entries[index].symlink && links.contains(index);
    }
    return DirectoryEntries(std::move(entries));
  }
  auto payload = std::make_shared<Storage::Payload>();
  payload->catalogue = previous.catalogue;
  payload->facts = previous.facts;
  payload->directoryLinks = std::make_shared<const std::unordered_set<std::size_t>>(links);
  for (auto& [index, entry] : payload->facts) {
    entry.linksToDirectory = entry.symlink && links.contains(index);
  }
  auto storage = std::make_shared<Storage>();
  storage->size = size();
  storage->blocks.push_back({std::move(payload), size()});
  return DirectoryEntries(std::move(storage));
}

const luciole::DirectoryEntry& luciole::DirectoryEntries::operator[](std::size_t index) const {
  if (!m_storage->directRows.empty()) {
    return *m_storage->directRows[index];
  }
  const auto row = m_storage->row(index);
  return row.entry();
}
const luciole::DirectoryEntry& luciole::DirectoryEntries::at(std::size_t index) const {
  if (index >= size()) {
    throw std::out_of_range("directory entry index");
  }
  return (*this)[index];
}

luciole::DirectoryEntries luciole::DirectoryEntries::concatenate(std::vector<DirectoryEntries> sources) {
  std::erase_if(sources, [](const auto& source) { return source.empty(); });
  if (sources.empty()) {
    return {};
  }
  if (sources.size() == 1) {
    return sources.front();
  }
  if (std::ranges::any_of(sources, [](const auto& source) { return source.m_storage->projected; })) {
    std::vector<Index> rows;
    for (std::size_t source = 0; source < sources.size(); ++source) {
      for (std::size_t entry = 0; entry < sources[source].size(); ++entry) {
        rows.push_back({source, entry});
      }
    }
    return project(std::move(sources), rows);
  }
  auto storage = std::make_shared<Storage>();
  for (const auto& source : sources) {
    for (const auto& block : source.m_storage->blocks) {
      storage->size += block.payload->size();
      storage->blocks.push_back({block.payload, storage->size});
    }
  }
  return DirectoryEntries(std::move(storage));
}

luciole::DirectoryEntries
luciole::DirectoryEntries::project(std::vector<DirectoryEntries> sources, const std::vector<Index>& rows) {
  if (rows.empty()) {
    return {};
  }
  auto storage = std::make_shared<Storage>();
  storage->projected = true;
  storage->size = rows.size();
  std::unordered_set<const Storage::Payload*> retained;
  for (const auto& source : sources) {
    if (!source.m_storage) {
      continue;
    }
    for (const auto& block : source.m_storage->blocks) {
      if (retained.insert(block.payload.get()).second) {
        storage->blocks.push_back(block);
      }
    }
  }
  const bool indexed = std::ranges::any_of(storage->blocks, [](const Storage::Block& block) {
    return block.payload->catalogue != nullptr;
  });
  if (indexed) {
    storage->rows.reserve(rows.size());
  } else {
    storage->directRows.reserve(rows.size());
  }
  for (const auto& row : rows) {
    const auto& source = sources.at(row.source);
    if (row.entry >= source.size()) {
      throw std::out_of_range("directory entry index");
    }
    if (indexed) {
      storage->rows.push_back(source.m_storage->row(row.entry));
    } else {
      storage->directRows.push_back(&source[row.entry]);
    }
  }
  return DirectoryEntries(std::move(storage));
}
