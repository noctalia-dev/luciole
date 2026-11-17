#pragma once

#include "fs/file_error.h"
#include "fs/location.h"
#include "fs/ws_discovery_protocol.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  struct WsServer {
    std::string endpoint;
    std::string name;
    wsd::Peer peer;
    Location location;
  };
  struct WsSnapshot {
    std::vector<WsServer> servers;
    bool searching = false;
    std::optional<FileError> error;
  };
  // Main-context observer API; sockets and metadata are cancellable worker jobs. No subprocesses.
  class WsDiscovery {
  public:
    WsDiscovery();
    ~WsDiscovery();
    WsDiscovery(const WsDiscovery&) = delete;
    WsDiscovery& operator=(const WsDiscovery&) = delete;
    void refresh();
    std::size_t subscribe(std::function<void()> callback);
    void unsubscribe(std::size_t id);
    const WsSnapshot& snapshot() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
