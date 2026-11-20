#pragma once

#include "fs/network_process.h"
#include "fs/posix_fd.h"

#include <span>

namespace luciole::network_wire {
  inline constexpr std::size_t maxFrame = 64U * 1024U * 1024U;
  // A frame consists of a network-order length and one bounded CBOR value. Optional fd ownership is transferred.
  void send(int socket, const NetworkMessage& value, const CancelToken& token, int descriptor = -1);
  // The polling callback may extend the deadline when the worker reports completed I/O.
  NetworkMessage receive(
      int socket, const CancelToken& token, const std::chrono::steady_clock::time_point& deadline,
      Fd* descriptor = nullptr, const std::function<void()>& tick = {}
  );
} // namespace luciole::network_wire
