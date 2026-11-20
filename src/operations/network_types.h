#pragma once

#include "fs/network_types.h"
#include "operations/archive_service.h"
#include "operations/batch_rename_service.h"
#include "operations/create_receipt.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/permission_service.h"
#include "operations/rename_service.h"
#include "operations/restore_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"

namespace luciole {
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AclEntry, tag, qualifier, permissions)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AclInfo, access, defaults, supported, editable, directory, message)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AclEdit, access, defaults)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AclEntryPatch, tag, qualifier, permissions, mask)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AclListPatch, entries, maskPolicy)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AclPatchEdit, access, defaults, defaultPolicy)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
      PermissionInfo, mode, owner, group, ownerName, groupName, canMode, canOwner, canGroup, symlink, multipleLinks,
      current, directory, acl, reason
  )
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PermissionEdit, mode, owner, group, mask)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RecursivePermissionEdit, files, directoryMode, directoryMask)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
      TemplateRevision, device, inode, size, modifiedSeconds, modifiedNanos, changedSeconds, changedNanos
  )
  inline void to_json(NetworkMessage& value, const ArchiveLimits& limits) {
    value = {
        {"maxArchiveBytes", limits.maxArchiveBytes},
        {"maxExpandedBytes", limits.maxExpandedBytes},
        {"maxEntries", limits.maxEntries},
        {"maxMetadataBytes", limits.maxMetadataBytes},
        {"maxDepth", limits.maxDepth},
        {"maxTime", limits.maxTime.count()}
    };
  }
  inline void from_json(const NetworkMessage& value, ArchiveLimits& limits) {
    value.at("maxArchiveBytes").get_to(limits.maxArchiveBytes);
    value.at("maxExpandedBytes").get_to(limits.maxExpandedBytes);
    value.at("maxEntries").get_to(limits.maxEntries);
    value.at("maxMetadataBytes").get_to(limits.maxMetadataBytes);
    value.at("maxDepth").get_to(limits.maxDepth);
    limits.maxTime = std::chrono::seconds(value.at("maxTime").get<std::chrono::seconds::rep>());
  }
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
      ArchiveEntry, path, parent, name, displayName, kind, size, link, mode, icon, encrypted
  )
} // namespace luciole

namespace nlohmann {
  template <> struct adl_serializer<luciole::TransferResult> {
    static void to_json(json& value, const luciole::TransferResult& source) {
      value = {{"source", source.source},     {"outcome", source.outcome},     {"error", source.error},
               {"children", source.children}, {"retryable", source.retryable}, {"destination", source.destination}};
    }
    static luciole::TransferResult from_json(const json& value) {
      using namespace luciole;
      TransferResult result(
          value.at("source").get<Location>(), value.at("outcome").get<TransferOutcome>(),
          value.at("error").get<std::optional<FileError>>()
      );
      value.at("children").get_to(result.children);
      value.at("retryable").get_to(result.retryable);
      value.at("destination").get_to(result.destination);
      return result;
    }
  };
  template <> struct adl_serializer<luciole::TransferConflict> {
    static void to_json(json& value, const luciole::TransferConflict& source) {
      value = {
          {"id", source.id},
          {"source", source.source},
          {"destination", source.destination},
          {"name", source.name},
          {"sourceDirectory", source.sourceDirectory},
          {"destinationDirectory", source.destinationDirectory},
          {"sourceSize", source.sourceSize},
          {"destinationSize", source.destinationSize},
          {"sourceModified", source.sourceModified},
          {"destinationModified", source.destinationModified},
          {"canMerge", source.canMerge},
          {"canApplyToAll", source.canApplyToAll},
          {"scopeLabel", source.scopeLabel}
      };
    }
    static luciole::TransferConflict from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("id").get<std::uint64_t>(),
          value.at("source").get<Location>(),
          value.at("destination").get<Location>(),
          value.at("name").get<std::string>(),
          value.at("sourceDirectory").get<bool>(),
          value.at("destinationDirectory").get<bool>(),
          value.at("sourceSize").get<std::optional<std::uint64_t>>(),
          value.at("destinationSize").get<std::optional<std::uint64_t>>(),
          value.at("sourceModified").get<std::optional<std::uint64_t>>(),
          value.at("destinationModified").get<std::optional<std::uint64_t>>(),
          value.at("canMerge").get<bool>(),
          value.at("canApplyToAll").get<bool>(),
          value.at("scopeLabel").get<std::string>()
      };
    }
  };
  template <> struct adl_serializer<luciole::TrashResult> {
    static void to_json(json& value, const luciole::TrashResult& source) {
      value = {{"source", source.source}, {"outcome", source.outcome}, {"error", source.error}};
    }
    static luciole::TrashResult from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("source").get<Location>(), value.at("outcome").get<TrashOutcome>(),
          value.at("error").get<std::optional<FileError>>()
      };
    }
  };
  template <> struct adl_serializer<luciole::EmptyTrashResult> {
    static void to_json(json& value, const luciole::EmptyTrashResult& source) {
      value = {
          {"location", source.location},
          {"displayName", source.displayName},
          {"outcome", source.outcome},
          {"error", source.error}
      };
    }
    static luciole::EmptyTrashResult from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("location").get<Location>(), value.at("displayName").get<std::string>(),
          value.at("outcome").get<EmptyTrashOutcome>(), value.at("error").get<std::optional<FileError>>()
      };
    }
  };
  template <> struct adl_serializer<luciole::BatchRenameResult> {
    static void to_json(json& value, const luciole::BatchRenameResult& source) {
      value = {
          {"source", source.source}, {"actual", source.actual}, {"outcome", source.outcome}, {"error", source.error}
      };
    }
    static luciole::BatchRenameResult from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("source").get<Location>(), value.at("actual").get<std::optional<Location>>(),
          value.at("outcome").get<BatchRenameOutcome>(), value.at("error").get<std::optional<FileError>>()
      };
    }
  };
  template <> struct adl_serializer<luciole::PermissionResult> {
    static void to_json(json& value, const luciole::PermissionResult& source) {
      value = {{"location", source.location}, {"name", source.name},   {"info", source.info},
               {"outcome", source.outcome},   {"error", source.error}, {"message", source.message}};
    }
    static luciole::PermissionResult from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("location").get<Location>(),
          value.at("name").get<std::string>(),
          value.at("info").get<std::optional<PermissionInfo>>(),
          value.at("outcome").get<PermissionOutcome>(),
          value.at("error").get<std::optional<FileError>>(),
          value.at("message").get<std::string>()
      };
    }
  };
  template <> struct adl_serializer<luciole::RestoreResult> {
    static void to_json(json& value, const luciole::RestoreResult& source) {
      value = {
          {"source", source.source},
          {"destination", source.destination},
          {"displayName", source.displayName},
          {"outcome", source.outcome},
          {"error", source.error}
      };
    }
    static luciole::RestoreResult from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("source").get<Location>(), value.at("destination").get<std::optional<Location>>(),
          value.at("displayName").get<std::string>(), value.at("outcome").get<RestoreOutcome>(),
          value.at("error").get<std::optional<FileError>>()
      };
    }
  };
  template <> struct adl_serializer<luciole::ArchiveCatalog> {
    static void to_json(json& value, const luciole::ArchiveCatalog& source) {
      value = {
          {"source", source.source},
          {"stamp", source.stamp},
          {"format", source.format},
          {"entries", source.entries},
          {"expandedBytes", source.expandedBytes},
          {"encrypted", source.encrypted}
      };
    }
    static luciole::ArchiveCatalog from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("source").get<Location>(),
          value.at("stamp").get<std::string>(),
          value.at("format").get<std::string>(),
          value.at("entries").get<std::vector<ArchiveEntry>>(),
          value.at("expandedBytes").get<std::uint64_t>(),
          value.at("encrypted").get<bool>()
      };
    }
  };
  template <> struct adl_serializer<luciole::ArchivePasswordRequest> {
    static void to_json(json& value, const luciole::ArchivePasswordRequest& source) {
      value = {{"id", source.id}, {"source", source.source}, {"retry", source.retry}, {"format", source.format}};
    }
    static luciole::ArchivePasswordRequest from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("id").get<std::uint64_t>(), value.at("source").get<Location>(), value.at("retry").get<bool>(),
          value.at("format").get<ArchiveFormat>()
      };
    }
  };
  template <> struct adl_serializer<luciole::DocumentTemplate> {
    static void to_json(json& value, const luciole::DocumentTemplate& source) {
      value = {
          {"location", source.location}, {"name", source.name}, {"label", source.label}, {"revision", source.revision}
      };
    }
    static luciole::DocumentTemplate from_json(const json& value) {
      using namespace luciole;
      return {
          value.at("location").get<Location>(), value.at("name").get<std::string>(),
          value.at("label").get<std::string>(), value.at("revision").get<TemplateRevision>()
      };
    }
  };
  template <> struct adl_serializer<luciole::TransferRequest> {
    static void to_json(json& value, const luciole::TransferRequest& source) {
      value = {
          {"sources", source.sources},
          {"destination", source.destination},
          {"action", source.action},
          {"allowMerge", source.allowMerge}
      };
    }
    static luciole::TransferRequest from_json(const json& value) {
      return {
          value.at("sources").get<std::vector<luciole::Location>>(), value.at("destination").get<luciole::Location>(),
          value.at("action").get<luciole::TransferAction>(), value.at("allowMerge").get<bool>()
      };
    }
  };
} // namespace nlohmann
namespace luciole {
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RenameSnapshot, status, source, destination, error)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CreateSnapshot, status, kind, parent, destination, error, removed, retireHistory)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TrashSnapshot, status, results, error)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
      EmptyTrashSnapshot, status, results, error, size, sizeIncomplete, selection, outsideTrash
  )
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
      BatchRenameOptions, mode, pattern, find, replacement, start, digits, keepExtensions
  )
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BatchRenameRow, before, after, name, error, changed)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BatchRenameSnapshot, phase, rows, results, parent, error, report, canApply)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
      PermissionSnapshot, phase, info, message, error, itemsFailed, applied, multiple, finished, cancelRequested,
      recursive, aclOperation, mixedMode, mixedOwner, mixedGroup, unknownItems, results, report
  )
} // namespace luciole
