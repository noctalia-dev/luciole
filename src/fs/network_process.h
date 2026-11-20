#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace luciole {
  enum class IoLane : std::uint8_t {
    Native,
    Network,
    PreviewProbe,
    NetworkPreviewProbe,
    PreviewDecode,
    NetworkPreviewDecode,
    Operation,
    NetworkOperation
  };
  int workerLimit(IoLane lane);

  using NetworkMessage = nlohmann::json;
  using NetworkEvent = std::function<NetworkMessage(NetworkMessage)>;

  class NetworkProcess {
  public:
    ~NetworkProcess();
    NetworkProcess(const NetworkProcess&) = delete;
    NetworkProcess& operator=(const NetworkProcess&) = delete;
    static std::shared_ptr<NetworkProcess> acquire(IoLane lane = IoLane::Native);
    NetworkMessage call(
        NetworkMessage request, const CancelToken& cancel, NetworkEvent event = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(10), int descriptor = -1
    );
    void discard(std::uint64_t job, bool cleanup = false);
    bool alive() const;
    IoLane lane() const;

  private:
    friend bool networkCleanupPending();
    friend std::vector<FileError> takeNetworkCleanupErrors();
    struct State;
    explicit NetworkProcess(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
    std::atomic<bool> m_reserved{false};
  };

  std::vector<FileError> takeNetworkCleanupErrors();
  bool networkCleanupPending();
  bool inNetworkWorker();
  bool networkWorkerCancelled();
  // Call after a chunk or entry completes. A blocked syscall leaves the request deadline in force.
  void noteWorkerIoProgress();
  // The worker owns every descriptor and filesystem object returned by its adapters.
  using NetworkHandler = std::function<NetworkMessage(const NetworkMessage&, const NetworkEvent&, int)>;
  int runNetworkWorker(const NetworkHandler& handler);
} // namespace luciole
