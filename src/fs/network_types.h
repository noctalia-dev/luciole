#pragma once

#include "fs/directory_entries.h"
#include "fs/folder_totals.h"
#include "fs/network_process.h"
#include "fs/system_mounts.h"

namespace nlohmann {
  template <typename T> struct adl_serializer<std::optional<T>> {
    static void to_json(json& value, const std::optional<T>& source) { value = source ? json(*source) : json(nullptr); }
    static std::optional<T> from_json(const json& value) {
      return value.is_null() ? std::nullopt : std::optional(value.get<T>());
    }
    static void from_json(const json& value, std::optional<T>& result) { result = from_json(value); }
  };
  template <typename T> struct adl_serializer<std::shared_ptr<T>> {
    static void to_json(json& value, const std::shared_ptr<T>& source) {
      value = source ? json(*source) : json(nullptr);
    }
    static std::shared_ptr<T> from_json(const json& value) {
      return value.is_null() ? nullptr : std::make_shared<std::remove_const_t<T>>(value.get<std::remove_const_t<T>>());
    }
    static void from_json(const json& value, std::shared_ptr<T>& result) { result = from_json(value); }
  };
  template <> struct adl_serializer<luciole::Location> {
    static void to_json(json& value, const luciole::Location& source) { value = source.uri(); }
    static luciole::Location from_json(const json& value) {
      return luciole::Location::fromUri(value.get<std::string>());
    }
  };
  template <> struct adl_serializer<luciole::InternedText> {
    static void to_json(json& value, const luciole::InternedText& source) {
      value = source ? json(*source) : json(nullptr);
    }
    static luciole::InternedText from_json(const json& value) {
      return value.is_null() ? luciole::InternedText{} : luciole::InternedText(value.get<std::string>());
    }
    static void from_json(const json& value, luciole::InternedText& result) { result = from_json(value); }
  };
} // namespace nlohmann

namespace luciole {
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MountConfiguration, path, source, type, options, automount)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
      FolderTotalsSnapshot, phase, bytes, files, folders, links, other, unavailable, unsupported, changed, limited,
      overflow, cancelRequested, error
  )
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileError, kind, message)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileIdentity, filesystem, file)
  inline void to_json(NetworkMessage& value, const ChangeRevision& source) {
    value = NetworkMessage::array({source.seconds, source.nanoseconds});
  }
  inline void from_json(const NetworkMessage& value, ChangeRevision& result) {
    value.at(0).get_to(result.seconds);
    value.at(1).get_to(result.nanoseconds);
  }
  inline void to_json(NetworkMessage& value, const EntryMetadata& source) {
    value =
        NetworkMessage::array({source.accessedSeconds, source.createdSeconds, source.owner, source.group, source.mode});
  }
  inline void from_json(const NetworkMessage& value, EntryMetadata& result) {
    value.at(0).get_to(result.accessedSeconds);
    value.at(1).get_to(result.createdSeconds);
    value.at(2).get_to(result.owner);
    value.at(3).get_to(result.group);
    value.at(4).get_to(result.mode);
  }
  inline void to_json(NetworkMessage& value, const FileAccess& source) {
    value =
        NetworkMessage::array({source.read, source.write, source.execute, source.rename, source.trash, source.remove});
  }
  inline void from_json(const NetworkMessage& value, FileAccess& result) {
    value.at(0).get_to(result.read);
    value.at(1).get_to(result.write);
    value.at(2).get_to(result.execute);
    value.at(3).get_to(result.rename);
    value.at(4).get_to(result.trash);
    value.at(5).get_to(result.remove);
  }
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FilesystemInfo, readOnly, remote, type, freeBytes)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DirectoryMetadata, access, filesystem)
  inline void to_json(NetworkMessage& value, const FileIconSource& source) {
    value = NetworkMessage::array({source.names, source.filePath, source.desktopFile});
  }
  inline void from_json(const NetworkMessage& value, FileIconSource& result) {
    value.at(0).get_to(result.names);
    value.at(1).get_to(result.filePath);
    value.at(2).get_to(result.desktopFile);
  }
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TrashInfo, original, deletionDate)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EntryExtra, displayName, etag, identity, parentDisplayName, trash, target)
  inline void to_json(NetworkMessage& value, const LocalTags& source) {
    value = NetworkMessage::array(
        {source.etagSeconds, source.device, source.inode, source.etagNanoseconds, source.etag, source.identity}
    );
  }
  inline void from_json(const NetworkMessage& value, LocalTags& result) {
    value.at(0).get_to(result.etagSeconds);
    value.at(1).get_to(result.device);
    value.at(2).get_to(result.inode);
    value.at(3).get_to(result.etagNanoseconds);
    value.at(4).get_to(result.etag);
    value.at(5).get_to(result.identity);
  }

  inline NetworkMessage encodeEntry(const DirectoryEntry& entry, bool address = true) {
    return NetworkMessage::array(
        {address ? entry.location().uri() : std::string{}, entry.id, entry.name, entry.type, entry.hidden,
         entry.symlink, entry.metadataPending, entry.metadataFailed, entry.queriedDetails, entry.size,
         entry.modifiedSeconds, entry.access, entry.icon, entry.contentType, entry.details, entry.contentRevision,
         entry.tags, entry.extra}
    );
  }
  inline void to_json(NetworkMessage& value, const DirectoryEntry& entry) { value = encodeEntry(entry); }
  inline void from_json(const NetworkMessage& value, DirectoryEntry& entry) {
    const auto uri = value.at(0).get<std::string>();
    if (!uri.empty()) {
      entry.place = Location::fromUri(uri);
    }
    value.at(1).get_to(entry.id);
    value.at(2).get_to(entry.name);
    value.at(3).get_to(entry.type);
    value.at(4).get_to(entry.hidden);
    value.at(5).get_to(entry.symlink);
    value.at(6).get_to(entry.metadataPending);
    value.at(7).get_to(entry.metadataFailed);
    value.at(8).get_to(entry.queriedDetails);
    value.at(9).get_to(entry.size);
    value.at(10).get_to(entry.modifiedSeconds);
    value.at(11).get_to(entry.access);
    value.at(12).get_to(entry.icon);
    value.at(13).get_to(entry.contentType);
    value.at(14).get_to(entry.details);
    value.at(15).get_to(entry.contentRevision);
    value.at(16).get_to(entry.tags);
    value.at(17).get_to(entry.extra);
  }
} // namespace luciole
