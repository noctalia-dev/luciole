#pragma once

#include "fs/file_icon_source.h"
#include "fs/file_metadata.h"
#include "fs/location.h"
#include "fs/trash_info.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace luciole {
  // Where an entry lives. A listing shares one folder between its entries, and each entry's location is that
  // folder's child named by the entry; other entries hold their whole location.
  class EntryPlace {
  public:
    EntryPlace() = default;
    EntryPlace(Location location); // NOLINT(google-explicit-constructor): a whole location is a place
    static EntryPlace inFolder(std::shared_ptr<const Location> folder);

    Location resolve(const std::string& name) const;
    // The shared folder, or null when this place holds a whole location.
    const Location* folder() const { return m_inFolder ? m_location.get() : nullptr; }

  private:
    std::shared_ptr<const Location> m_location;
    bool m_inFolder = false;
  };

  // Facts few entries have; ordinary local entries share none.
  struct EntryExtra {
    std::optional<std::string> displayName; // Only when it differs from the name.
    std::optional<std::string> etag;        // Only when it is not a local modification tag.
    std::optional<FileIdentity> identity;   // Only when it is not a local device and inode pair.
    std::string parentDisplayName;
    std::optional<TrashInfo> trash;
  };

  // Local providers' entity tag and identity as numbers; the text forms are rebuilt exactly on request.
  struct LocalTags {
    std::uint64_t etagSeconds = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint32_t etagNanoseconds = 0;
    bool etag = false;
    bool identity = false;
  };

  struct DirectoryEntry {
    std::uint64_t id = 0;
    EntryPlace place;
    std::string name; // Original filename bytes, including non-UTF-8 names on Unix.
    FileType type = FileType::Unknown;
    bool hidden = false;
    bool symlink = false;
    // Local listings leave access read/write/execute unknown; entryAccess() asks the filesystem when they matter.
    bool accessDeferred = false;
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> modifiedSeconds;
    FileAccess access{};
    SharedFileIcon icon = nullptr; // Shared between entries with equal artwork; null means none.
    // Provider's regular MIME type, with its fast type as a fallback.
    InternedText contentType = nullptr;
    EntryMetadata details{};
    std::optional<ChangeRevision> contentRevision = std::nullopt;
    LocalTags tags{};
    std::shared_ptr<const EntryExtra> extra = nullptr;

    Location location() const { return place.resolve(name); }
    // Presentation text; equals the name unless the provider reports another.
    const std::string& displayName() const;
    std::optional<std::string> etag() const;
    std::optional<FileIdentity> identity() const;
    // Search result context; never an operation address.
    const std::string& parentDisplayName() const;
    // Set only for Trash items.
    const TrashInfo* trash() const;
    // The Trash facts, empty outside the Trash.
    const TrashInfo& trashInfo() const;

    // Setters keep local forms compact and move everything else into the shared extra facts.
    void setDisplayName(std::string displayName);
    void setEtag(std::optional<std::string> etag);
    void setIdentity(std::optional<FileIdentity> identity);
    void setParentDisplayName(std::string text);
    void setTrash(std::optional<TrashInfo> trash);

  private:
    EntryExtra& editExtra();
  };

  // Published entries are immutable. Copies retain payloads; producers publish a new collection.
  class DirectoryEntries {
  public:
    DirectoryEntries() = default;
    DirectoryEntries(std::vector<DirectoryEntry> entries);
    DirectoryEntries(std::initializer_list<DirectoryEntry> entries);

    struct Index {
      std::size_t source;
      std::size_t entry;
    };
    // Retains source payloads, without retaining intermediate projection/index tables.
    static DirectoryEntries concatenate(std::vector<DirectoryEntries> sources);
    static DirectoryEntries project(std::vector<DirectoryEntries> sources, const std::vector<Index>& rows);

    std::size_t size() const;
    bool empty() const { return size() == 0; }
    const DirectoryEntry& operator[](std::size_t index) const;
    const DirectoryEntry& at(std::size_t index) const;
    const DirectoryEntry& front() const { return (*this)[0]; }
    const DirectoryEntry& back() const { return (*this)[size() - 1]; }

    class Iterator {
    public:
      using value_type = DirectoryEntry;
      using difference_type = std::ptrdiff_t;
      using reference = const DirectoryEntry&;
      using pointer = const DirectoryEntry*;
      using iterator_category = std::forward_iterator_tag;
      using iterator_concept = std::forward_iterator_tag;
      Iterator() = default;
      reference operator*() const { return (*m_owner)[m_index]; }
      pointer operator->() const { return &**this; }
      Iterator& operator++() {
        ++m_index;
        return *this;
      }
      Iterator operator++(int) {
        const auto previous = *this;
        ++*this;
        return previous;
      }
      bool operator==(const Iterator&) const = default;

    private:
      friend DirectoryEntries;
      Iterator(const DirectoryEntries* owner, std::size_t index) : m_owner(owner), m_index(index) {}
      const DirectoryEntries* m_owner = nullptr;
      std::size_t m_index = 0;
    };
    Iterator begin() const { return Iterator(this, 0); }
    Iterator end() const { return Iterator(this, size()); }

  private:
    struct Storage;
    explicit DirectoryEntries(std::shared_ptr<const Storage> storage) : m_storage(std::move(storage)) {}
    std::shared_ptr<const Storage> m_storage;
  };
} // namespace luciole
