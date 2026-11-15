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
struct luciole::DirectoryEntries::Storage {
  struct Block {
    std::shared_ptr<const std::vector<DirectoryEntry>> entries;
    std::size_t end;
  };
  std::vector<Block> blocks;
  std::vector<const DirectoryEntry*> rows;
  std::size_t size = 0;
  bool projected = false;
};

luciole::DirectoryEntries::DirectoryEntries(std::vector<DirectoryEntry> entries) {
  if (entries.empty()) {
    return;
  }
  auto storage = std::make_shared<Storage>();
  storage->size = entries.size();
  storage->blocks.push_back({std::make_shared<const std::vector<DirectoryEntry>>(std::move(entries)), storage->size});
  m_storage = std::move(storage);
}

luciole::DirectoryEntries::DirectoryEntries(std::initializer_list<DirectoryEntry> entries)
    : DirectoryEntries(std::vector<DirectoryEntry>(entries)) {}

std::size_t luciole::DirectoryEntries::size() const { return m_storage ? m_storage->size : 0; }

const luciole::DirectoryEntry& luciole::DirectoryEntries::operator[](std::size_t index) const {
  if (m_storage->projected) {
    return *m_storage->rows[index];
  }
  const auto& blocks = m_storage->blocks;
  if (blocks.size() == 1) {
    return (*blocks.front().entries)[index];
  }
  const auto block = std::ranges::upper_bound(blocks, index, {}, &Storage::Block::end);
  return (*block->entries)[index - (block->end - block->entries->size())];
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
      storage->size += block.entries->size();
      storage->blocks.push_back({block.entries, storage->size});
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
  std::unordered_set<const std::vector<DirectoryEntry>*> retained;
  for (const auto& source : sources) {
    if (!source.m_storage) {
      continue;
    }
    for (const auto& block : source.m_storage->blocks) {
      if (retained.insert(block.entries.get()).second) {
        storage->blocks.push_back(block);
      }
    }
  }
  storage->rows.reserve(rows.size());
  for (const auto& row : rows) {
    storage->rows.push_back(&sources.at(row.source).at(row.entry));
  }
  return DirectoryEntries(std::move(storage));
}
