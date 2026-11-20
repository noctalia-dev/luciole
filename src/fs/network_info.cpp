#include "fs/network_info.h"

#include "fs/folder_totals_walk.h"
#include "fs/gio_interop.h"
#include "fs/network_types.h"
#include "fs/system_mounts.h"

#include <deque>
#include <mutex>

namespace {
  struct Classification {
    std::mutex aliasesMutex, rootsMutex;
    std::deque<luciole::Location> aliases;
    std::vector<luciole::Location> roots;
    std::chrono::steady_clock::time_point updated;
  };
  Classification& classification() {
    // Cancelled IPC jobs can still consult these facts during process teardown.
    static auto* value = new Classification;
    return *value;
  }
} // namespace

void luciole::registerNetworkFolder(const Location& location) {
  auto& cache = classification();
  const std::scoped_lock lock(cache.aliasesMutex);
  std::erase(cache.aliases, location);
  cache.aliases.push_back(location);
  if (cache.aliases.size() > 256) {
    cache.aliases.pop_front();
  }
}

luciole::IoLane luciole::ioLane(const Location& location) {
  auto& cache = classification();
  if (!location.isNative()) {
    return IoLane::Network;
  }
  {
    const std::scoped_lock lock(cache.aliasesMutex);
    if (std::ranges::any_of(cache.aliases, [&](const auto& root) {
          return location == root || location.isInside(root);
        })) {
      return IoLane::Network;
    }
  }
  const std::scoped_lock lock(cache.rootsMutex);
  const auto now = std::chrono::steady_clock::now();
  if (now - cache.updated >= std::chrono::milliseconds(500)) {
    cache.roots.clear();
    for (const auto& config : configuredSystemMounts()) {
      cache.roots.push_back(Location::fromPath(config.path));
    }
    for (const auto& mount : readMountedFilesystems()) {
      if (networkMountType(mount.type) || mount.type == "autofs") {
        cache.roots.push_back(Location::fromPath(mount.path));
      }
    }
    cache.updated = now;
  }
  if (std::ranges::any_of(cache.roots, [&](const auto& root) { return location == root || location.isInside(root); })) {
    return IoLane::Network;
  }
  return IoLane::Native;
}

luciole::NetworkMessage luciole::filesystemRequest(std::string_view action, const Location& location) {
  NetworkMessage result{{"action", action}, {"uri", location.uri()}};
  if (!location.isNative()) {
    return result;
  }
  bool known = false;
  if (const auto expected = automountFor(location, configuredSystemMounts(&known))) {
    result["expected"] = *expected;
  } else if (known) {
    result["expected"] = nullptr;
  }
  return result;
}

luciole::NetworkMessage luciole::encodeInfo(GFileInfo* info) {
  NetworkMessage result = NetworkMessage::array();
  char** attributes = g_file_info_list_attributes(info, nullptr);
  for (std::size_t i = 0; attributes[i]; ++i) {
    GFileAttributeType type = G_FILE_ATTRIBUTE_TYPE_INVALID;
    gpointer value = nullptr;
    if (!g_file_info_get_attribute_data(info, attributes[i], &type, &value, nullptr)) {
      continue;
    }
    NetworkMessage item{{"name", attributes[i]}, {"type", type}};
    switch (type) {
    case G_FILE_ATTRIBUTE_TYPE_STRING:
    case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
      item["value"] = std::string(static_cast<const char*>(value));
      break;
    case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
      item["value"] = *static_cast<gboolean*>(value) != FALSE;
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT32:
      item["value"] = *static_cast<guint32*>(value);
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT32:
      item["value"] = *static_cast<gint32*>(value);
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT64:
      item["value"] = *static_cast<guint64*>(value);
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT64:
      item["value"] = *static_cast<gint64*>(value);
      break;
    case G_FILE_ATTRIBUTE_TYPE_OBJECT:
      if (G_IS_ICON(value)) {
        const GString icon(g_icon_to_string(G_ICON(value)));
        if (icon) {
          item["value"] = icon.get();
        }
      }
      break;
    default:
      break;
    }
    if (item.contains("value")) {
      result.push_back(std::move(item));
    }
  }
  g_strfreev(attributes);
  return result;
}

luciole::GioPtr<GFileInfo> luciole::decodeInfo(const NetworkMessage& value) {
  GioPtr<GFileInfo> result(g_file_info_new());
  if (!value.is_array() || value.size() > 256) {
    throwFileError(FileErrorKind::Unavailable, "Invalid filesystem metadata response.");
  }
  for (const auto& item : value) {
    const auto name = item.at("name").get<std::string>();
    const auto& field = item.at("value");
    switch (item.at("type").get<int>()) {
    case G_FILE_ATTRIBUTE_TYPE_STRING:
      g_file_info_set_attribute_string(result.get(), name.c_str(), field.get<std::string>().c_str());
      break;
    case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
      g_file_info_set_attribute_byte_string(result.get(), name.c_str(), field.get<std::string>().c_str());
      break;
    case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
      g_file_info_set_attribute_boolean(result.get(), name.c_str(), field.get<bool>());
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT32:
      g_file_info_set_attribute_uint32(result.get(), name.c_str(), field.get<std::uint32_t>());
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT32:
      g_file_info_set_attribute_int32(result.get(), name.c_str(), field.get<std::int32_t>());
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT64:
      g_file_info_set_attribute_uint64(result.get(), name.c_str(), field.get<std::uint64_t>());
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT64:
      g_file_info_set_attribute_int64(result.get(), name.c_str(), field.get<std::int64_t>());
      break;
    case G_FILE_ATTRIBUTE_TYPE_OBJECT: {
      const GioPtr<GIcon> icon(g_icon_new_for_string(field.get<std::string>().c_str(), nullptr));
      if (icon) {
        g_file_info_set_attribute_object(result.get(), name.c_str(), G_OBJECT(icon.get()));
      }
      break;
    }
    default:
      throwFileError(FileErrorKind::Unavailable, "Unsupported filesystem metadata response.");
    }
  }
  return result;
}

luciole::GioPtr<GFileInfo> luciole::isolatedInfo(
    const Location& location, const char* attributes, GFileQueryInfoFlags flags, const CancelToken& token,
    bool filesystem
) {
  if (inNetworkWorker()) {
    GError* raw = nullptr;
    GioPtr<GFileInfo> result(
        filesystem ? g_file_query_filesystem_info(gioFile(location).get(), attributes, gioCancellable(token), &raw)
                   : g_file_query_info(gioFile(location).get(), attributes, flags, gioCancellable(token), &raw)
    );
    const GErrorPtr error(raw);
    if (!result) {
      throw FileErrorException(
          error ? fileError(*error) : FileError{FileErrorKind::Unavailable, "Could not query the file."}
      );
    }
    return result;
  }
  auto request = filesystemRequest("info", location);
  request.update({{"attributes", attributes}, {"flags", flags}, {"filesystem", filesystem}});
  const auto reply = NetworkProcess::acquire(ioLane(location))->call(std::move(request), token);
  return decodeInfo(reply);
}

void luciole::isolatedChildren(
    const Location& parent, const char* attributes, GFileQueryInfoFlags flags, const CancelToken& token,
    const std::function<bool(GFileInfo*)>& visit
) {
  auto request = filesystemRequest("list", parent);
  request.update({{"attributes", attributes}, {"flags", flags}, {"interactive", true}});
  (void)NetworkProcess::acquire(ioLane(parent))->call(std::move(request), token, [&](const NetworkMessage& event) {
    bool proceed = true;
    for (const auto& value : event.at("entries")) {
      const auto info = decodeInfo(value);
      if (!visit(info.get())) {
        proceed = false;
        break;
      }
    }
    return NetworkMessage{{"continue", proceed}};
  });
}

luciole::NetworkMessage
luciole::networkFilesystemRequest(const NetworkMessage& request, const NetworkEvent& event, int descriptor) {
  const auto action = request.at("action").get<std::string>();
  if (action == "noop") {
    return {};
  }
  if (action == "totals") {
    return networkFolderTotals(request, event);
  }
  const auto location = Location::fromUri(request.at("uri").get<std::string>());
  const auto file = gioFile(location);
  const CancelToken token;
  std::optional<MountConfiguration> expected;
  const auto expectation = request.find("expected");
  if (expectation != request.end() && !expectation->is_null()) {
    expected = expectation->get<MountConfiguration>();
  }
  const auto mount = location.isNative() && (expectation == request.end() || expected)
      ? openSystemMount(location, token, std::move(expected))
      : Fd{};
  if (action == "native" || action == "validate" || action == "directoryLinks" || action == "directoryMetadata") {
    return networkNativeRequest(request, event, descriptor, mount.valid());
  }
  if (action == "info") {
    const auto attributes = request.at("attributes").get<std::string>();
    GError* raw = nullptr;
    const GioPtr<GFileInfo> info(
        request.value("filesystem", false)
            ? g_file_query_filesystem_info(file.get(), attributes.c_str(), nullptr, &raw)
            : g_file_query_info(
                  file.get(), attributes.c_str(), static_cast<GFileQueryInfoFlags>(request.at("flags").get<int>()),
                  nullptr, &raw
              )
    );
    const GErrorPtr error(raw);
    if (!info) {
      throw FileErrorException(
          error ? fileError(*error) : FileError{FileErrorKind::Unavailable, "Could not query the file."}
      );
    }
    return encodeInfo(info.get());
  }
  if (action == "list") {
    const auto attributes = request.at("attributes").get<std::string>();
    GError* raw = nullptr;
    const GioPtr<GFileEnumerator> listing(g_file_enumerate_children(
        file.get(), attributes.c_str(),
        static_cast<GFileQueryInfoFlags>(request.value("flags", static_cast<int>(G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))),
        nullptr, &raw
    ));
    const GErrorPtr error(raw);
    if (!listing) {
      throw FileErrorException(
          error ? fileError(*error) : FileError{FileErrorKind::Unavailable, "Could not read the folder."}
      );
    }
    std::size_t count = 0;
    const auto limit = request.value("limit", static_cast<std::size_t>(0));
    while (true) {
      NetworkMessage batch = NetworkMessage::array();
      for (int i = 0; i < 64 && (!limit || count < limit); ++i) {
        GError* failure = nullptr;
        const GioPtr<GFileInfo> info(g_file_enumerator_next_file(listing.get(), nullptr, &failure));
        const GErrorPtr held(failure);
        if (held) {
          throw FileErrorException(fileError(*held));
        }
        if (!info) {
          if (!batch.empty()) {
            NetworkMessage message{{"entries", std::move(batch)}};
            if (request.value("interactive", false)) {
              message["question"] = true;
            }
            (void)event(std::move(message));
          }
          return {{"limited", false}};
        }
        batch.push_back(encodeInfo(info.get()));
        ++count;
      }
      NetworkMessage message{{"entries", std::move(batch)}};
      if (request.value("interactive", false)) {
        message["question"] = true;
      }
      const auto answer = event(std::move(message));
      if (!answer.is_null() && !answer.value("continue", true)) {
        return {{"limited", true}};
      }
      if (limit && count >= limit) {
        return {{"limited", true}};
      }
    }
  }
  throwFileError(FileErrorKind::Unsupported, "Unknown filesystem request.");
}
