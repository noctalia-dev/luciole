#pragma once

#include "fs/background_job.h"
#include "fs/network_process.h"

namespace luciole {
  // Service adapter. Decode runs on an IPC worker; delivery runs on the initiating main context.
  class NetworkOperation {
  public:
    explicit NetworkOperation(std::string service);
    ~NetworkOperation();
    bool busy() const;
    void useProcess(std::shared_ptr<NetworkProcess> process);
    void cancel();
    std::optional<NetworkMessage> question() const;
    bool answer(std::uint64_t id, NetworkMessage value);

    template <typename Reply, typename Decode, typename Deliver>
    bool
    invoke(std::string method, NetworkMessage arguments, IoLane lane, bool mutating, Decode decode, Deliver deliver) {
      auto delivery = std::make_shared<Deliver>(std::move(deliver));
      return start(
          std::move(method), std::move(arguments), lane, mutating,
          [decode = std::move(decode), delivery](
              const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process, bool final
          ) mutable -> Delivery {
            auto reply = decode(value, process);
            return [delivery, reply = std::move(reply), final] mutable {
              (*delivery)(JobResult<Reply>(std::move(reply)), final);
            };
          },
          [delivery](FileError error) -> Delivery {
            return [delivery, error = std::move(error)] mutable {
              (*delivery)(JobResult<Reply>(std::unexpected(std::move(error))), true);
            };
          }
      );
    }

  private:
    using Delivery = std::move_only_function<void()>;
    using DecodeDelivery =
        std::move_only_function<Delivery(const NetworkMessage&, const std::shared_ptr<NetworkProcess>&, bool)>;
    using ErrorDelivery = std::move_only_function<Delivery(FileError)>;
    bool start(
        std::string method, NetworkMessage arguments, IoLane lane, bool mutating, DecodeDelivery decode,
        ErrorDelivery error
    );
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
