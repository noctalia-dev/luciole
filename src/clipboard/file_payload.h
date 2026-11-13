#pragma once

#include "fs/location.h"
#include "fs/transfer_types.h"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  inline constexpr std::string_view kUriListMime = "text/uri-list";
  inline constexpr std::string_view kGnomeFilesMime = "x-special/gnome-copied-files";
  // Luciole-specific drag-start intent; URI data and Wayland action negotiation stay standard.
  inline constexpr std::string_view kAskDropMime = "application/x-luciole-ask";
  inline constexpr std::string_view kExtractionServiceMime = "application/x-kde-ark-dndextract-service";
  inline constexpr std::string_view kExtractionPathMime = "application/x-kde-ark-dndextract-path";
  // An established deferred-extraction contract, selected by MIME offers, not process identity.
  struct ExtractionOffer {
    std::string service;
    std::string path;
  };
  inline constexpr std::string_view kKdeCutMime = "application/x-kde-cutselection";
  inline constexpr std::size_t kFilePayloadLimit = 1024 * 1024;
  inline constexpr std::size_t kFilePayloadCount = 4096;

  struct FilePayload {
    std::vector<Location> locations;
    bool cut = false;
  };

  // Parse the whole immutable offer or reject it. Never silently drop bad sources.
  std::expected<FilePayload, std::string> parseFilePayload(std::string_view mime, std::string_view bytes);
  std::expected<std::string, std::string> encodeFilePayload(const FilePayload& payload, std::string_view mime);
  FilePayload remainingCutFiles(FilePayload payload, const std::vector<TransferResult>& results);
} // namespace luciole
