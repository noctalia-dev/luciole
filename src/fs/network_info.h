#pragma once

#include "fs/gio_ptr.h"
#include "fs/location.h"
#include "fs/native_listing.h"
#include "fs/network_process.h"
#include "fs/posix_fd.h"

#include <gio/gio.h>

namespace luciole {
  void isolatedChildren(
      const Location& parent, const char* attributes, GFileQueryInfoFlags flags, const CancelToken& token,
      const std::function<bool(GFileInfo*)>& visit
  );
  NetworkMessage encodeInfo(GFileInfo* info);
  IoLane ioLane(const Location& location);
  inline IoLane operationLane(const Location& location) {
    return ioLane(location) == IoLane::Network ? IoLane::NetworkOperation : IoLane::Operation;
  }
  void registerNetworkFolder(const Location& location);
  NetworkMessage filesystemRequest(std::string_view action, const Location& location);
  Fd openRequestMount(const NetworkMessage& request, const Location& location, const CancelToken& token);
  NativeListing isolatedListing(
      const Location& location, const char* fallbackAttributes, std::size_t limit, bool catalogue,
      const CancelToken& token
  );
  NetworkMessage
  networkNativeRequest(const NetworkMessage& request, const NetworkEvent& event, int descriptor, bool automount);
  GioPtr<GFileInfo> decodeInfo(const NetworkMessage& value);
  GioPtr<GFileInfo> isolatedInfo(
      const Location& location, const char* attributes, GFileQueryInfoFlags flags, const CancelToken& token,
      bool filesystem = false
  );
  NetworkMessage networkFilesystemRequest(const NetworkMessage& request, const NetworkEvent& event, int descriptor);
} // namespace luciole
