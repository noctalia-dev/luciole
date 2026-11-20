#include "fs/gio_interop.h"
#include "fs/network_info.h"
#include "fs/network_types.h"

luciole::NetworkMessage luciole::networkNativeRequest(
    const NetworkMessage& request, const NetworkEvent& event, int descriptor, bool automount
) {
  const auto location = Location::fromUri(request.at("uri").get<std::string>());
  const auto path = location.nativePath();
  if (!path) {
    throwFileError(FileErrorKind::InvalidArgument, "Expected a native folder.");
  }
  if (request.at("action") == "directoryMetadata") {
    const auto file = gioFile(location);
    GError* error = nullptr;
    const GioPtr<GFileInfo> filesystem(g_file_query_filesystem_info(
        file.get(), request.at("filesystem").get_ref<const std::string&>().c_str(), nullptr, &error
    ));
    const GErrorPtr filesystemError(std::exchange(error, nullptr));
    const GioPtr<GFileInfo> access(g_file_query_info(
        file.get(), request.at("access").get_ref<const std::string&>().c_str(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        nullptr, &error
    ));
    const GErrorPtr accessError(error);
    return {
        {"automount", automount},
        {"filesystem", filesystem ? readFilesystemInfo(filesystem.get()) : FilesystemInfo{}},
        {"filesystemError", filesystemError ? std::optional(fileError(*filesystemError)) : std::nullopt},
        {"access", access ? readFileAccess(access.get()) : FileAccess{}},
        {"accessError", accessError ? std::optional(fileError(*accessError)) : std::nullopt}
    };
  }
  if (request.at("action") == "directoryLinks") {
    NetworkMessage directories = NetworkMessage::array();
    for (const auto& link : request.at("links")) {
      const auto& name = link.at(1).get_ref<const std::string&>();
      if (linksToDirectory(*path + "/" + name)) {
        directories.push_back(link.at(0));
      }
    }
    return directories;
  }
  if (request.at("action") == "validate") {
    BrowseCatalogue catalogue;
    catalogue.folder = std::make_shared<const Location>(location);
    catalogue.device = request.at("device").get<std::uint64_t>();
    catalogue.inode = request.at("inode").get<std::uint64_t>();
    std::vector<std::size_t> rows;
    for (const auto& value : request.at("rows")) {
      const auto name = value.at(0).get<std::string>();
      if (name.size() > UINT16_MAX) {
        throwFileError(FileErrorKind::InvalidArgument, "Invalid catalogue name size.");
      }
      rows.push_back(catalogue.items.size());
      catalogue.items.push_back(
          {.id = 0,
           .inodeHint = value.at(1).get<std::uint64_t>(),
           .offset = static_cast<std::uint32_t>(catalogue.names.size()),
           .length = static_cast<std::uint16_t>(name.size())}
      );
      catalogue.names += name;
      catalogue.names += '\0';
    }
    const auto result = validateCatalogueRows(catalogue, rows, nullptr);
    return {{"changed", result.changed}, {"parentChanged", result.parentChanged}, {"unavailable", result.unavailable}};
  }
  auto listing = listNativeFolder(
      *path, request.value("limit", static_cast<std::size_t>(0)), nullptr, request.value("catalogue", false)
  );
  if (listing.unsupported) {
    if (!request.contains("attributes")) {
      return {{"limited", false}, {"unsupported", true}};
    }
    auto fallback = request;
    fallback["action"] = "list";
    return networkFilesystemRequest(fallback, event, descriptor);
  }
  if (listing.catalogue) {
    const auto& catalogue = *listing.catalogue;
    (void)event({{"catalogue", {{"device", catalogue.device}, {"inode", catalogue.inode}}}});
    for (std::size_t first = 0; first < catalogue.items.size(); first += 2048) {
      const auto last = std::min(first + 2048, catalogue.items.size());
      const auto begin = catalogue.items[first].offset;
      const auto end = catalogue.items[last - 1].offset + catalogue.items[last - 1].length + 1;
      NetworkMessage items = NetworkMessage::array(), displays = NetworkMessage::array();
      for (auto i = first; i < last; ++i) {
        const auto& item = catalogue.items[i];
        items.push_back(
            NetworkMessage::array(
                {item.inodeHint, item.offset - begin, item.length, item.type, bool{item.hidden}, bool{item.symlink}}
            )
        );
        if (const auto display = catalogue.displayNames.find(i); display != catalogue.displayNames.end()) {
          displays.push_back(NetworkMessage::array({i - first, display->second}));
        }
      }
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(catalogue.names.data());
      (void)event(
          {{"names", NetworkMessage::binary(std::vector<std::uint8_t>(bytes + begin, bytes + end))},
           {"items", std::move(items)},
           {"displays", std::move(displays)}}
      );
    }
  } else {
    for (std::size_t first = 0; first < listing.entries.size(); first += 64) {
      NetworkMessage batch = NetworkMessage::array();
      for (auto i = first; i < std::min(first + 64, listing.entries.size()); ++i) {
        batch.push_back(encodeEntry(listing.entries[i], false));
      }
      (void)event({{"nativeEntries", std::move(batch)}});
    }
  }
  return {{"limited", listing.limited}, {"uncertain", listing.uncertain}, {"native", true}};
}

luciole::NativeListing luciole::isolatedListing(
    const Location& location, const char* fallbackAttributes, std::size_t limit, bool catalogue,
    const CancelToken& token
) {
  NativeListing result;
  FileIconPool icons;
  const auto path = location.nativePath();
  icons.setLocalFolder(path ? path->c_str() : nullptr);
  const auto folder = std::make_shared<const Location>(location);
  auto request = filesystemRequest("native", location);
  request.update({{"limit", limit}, {"catalogue", catalogue}});
  if (fallbackAttributes) {
    request["attributes"] = fallbackAttributes;
  }
  const auto reply =
      NetworkProcess::acquire(ioLane(location))->call(std::move(request), token, [&](const NetworkMessage& event) {
        if (event.contains("catalogue")) {
          result.catalogue.emplace();
          result.catalogue->folder = folder;
          result.catalogue->device = event.at("catalogue").at("device").get<std::uint64_t>();
          result.catalogue->inode = event.at("catalogue").at("inode").get<std::uint64_t>();
        } else if (event.contains("items")) {
          if (!result.catalogue) {
            throwFileError(FileErrorKind::Unavailable, "Missing folder catalogue header.");
          }
          auto& target = *result.catalogue;
          const auto base = target.names.size();
          const auto first = target.items.size();
          const auto& names = event.at("names").get_binary();
          if (base + names.size() > UINT32_MAX) {
            throwFileError(FileErrorKind::Unavailable, "The folder catalogue exceeds its name limit.");
          }
          target.names.append(reinterpret_cast<const char*>(names.data()), names.size());
          for (const auto& encoded : event.at("items")) {
            const auto offset = encoded.at(1).get<std::uint32_t>();
            const auto length = encoded.at(2).get<std::uint16_t>();
            if (offset + static_cast<std::size_t>(length) >= names.size() || names[offset + length] != 0) {
              throwFileError(FileErrorKind::Unavailable, "Invalid folder catalogue name.");
            }
            target.items.push_back(
                {.id = 0,
                 .inodeHint = encoded.at(0).get<std::uint64_t>(),
                 .offset = static_cast<std::uint32_t>(base + offset),
                 .length = length,
                 .type = encoded.at(3).get<FileType>(),
                 .hidden = encoded.at(4).get<bool>(),
                 .symlink = encoded.at(5).get<bool>()}
            );
          }
          for (const auto& display : event.at("displays")) {
            const auto index = display.at(0).get<std::size_t>();
            if (index >= target.items.size() - first) {
              throwFileError(FileErrorKind::Unavailable, "Invalid folder catalogue display name.");
            }
            target.displayNames.emplace(first + index, display.at(1).get<std::string>());
          }
        } else if (event.contains("nativeEntries")) {
          for (const auto& value : event.at("nativeEntries")) {
            auto entry = value.get<DirectoryEntry>();
            entry.place = EntryPlace::inFolder(folder);
            entry.icon = icons.getLocal(
                entry.name.c_str(), entry.type, entry.contentType ? entry.contentType->c_str() : nullptr, *folder
            );
            result.entries.push_back(std::move(entry));
          }
        } else {
          for (const auto& value : event.at("entries")) {
            const auto info = decodeInfo(value);
            result.entries.push_back(readEntry(info.get(), folder, icons, true, false));
          }
        }
        return NetworkMessage{};
      });
  result.limited = reply.at("limited").get<bool>();
  result.unsupported = reply.value("unsupported", false);
  if (reply.contains("uncertain")) {
    result.uncertain = reply.at("uncertain").get<std::vector<std::uint32_t>>();
    if (std::ranges::any_of(result.uncertain, [&](const auto index) { return index >= result.entries.size(); })) {
      throwFileError(FileErrorKind::Unavailable, "Invalid content-type row in the folder listing.");
    }
  }
  return result;
}
