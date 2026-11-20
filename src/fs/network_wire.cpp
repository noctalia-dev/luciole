#include "fs/network_wire.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

namespace {
  void waitSocket(
      int socket, short events, const luciole::CancelToken& token,
      const std::chrono::steady_clock::time_point& deadline, const std::function<void()>& tick = {}
  ) {
    while (true) {
      if (tick) {
        tick();
      }
      if (token.cancelled()) {
        luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Request cancelled.");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        luciole::throwFileError(
            luciole::FileErrorKind::Unavailable, "The filesystem did not respond. Refresh to retry."
        );
      }
      pollfd descriptor{socket, events, 0};
      const int ready = ::poll(&descriptor, 1, deadline == std::chrono::steady_clock::time_point::max() ? -1 : 25);
      if (ready > 0) {
        if (descriptor.revents & events) {
          return;
        }
        luciole::throwFileError(luciole::FileErrorKind::Unavailable, "The filesystem worker stopped responding.");
      }
      if (ready < 0 && errno != EINTR) {
        luciole::throwErrno(errno);
      }
    }
  }
} // namespace

void luciole::network_wire::send(int socket, const NetworkMessage& value, const CancelToken& token, int descriptor) {
  const auto bytes = NetworkMessage::to_cbor(value);
  if (bytes.size() > maxFrame) {
    throwFileError(FileErrorKind::Unavailable, "The filesystem response exceeds its limit.");
  }
  const auto length = htonl(static_cast<std::uint32_t>(bytes.size()));
  alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
  iovec header{const_cast<std::uint32_t*>(&length), sizeof(length)};
  msghdr message{};
  message.msg_iov = &header;
  message.msg_iovlen = 1;
  if (descriptor >= 0) {
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &descriptor, sizeof(descriptor));
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::size_t offset = 0;
  while (offset < sizeof(length)) {
    waitSocket(socket, POLLOUT, token, deadline);
    header.iov_base = reinterpret_cast<char*>(const_cast<std::uint32_t*>(&length)) + offset;
    header.iov_len = sizeof(length) - offset;
    const auto written = ::sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (written < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (written <= 0) {
      throwFileError(FileErrorKind::Unavailable, "Could not contact the filesystem worker.");
    }
    offset += static_cast<std::size_t>(written);
    message.msg_control = nullptr;
    message.msg_controllen = 0;
  }
  offset = 0;
  while (offset < bytes.size()) {
    waitSocket(socket, POLLOUT, token, deadline);
    const auto written = ::send(socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (written < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (written <= 0) {
      throwFileError(FileErrorKind::Unavailable, "Could not contact the filesystem worker.");
    }
    offset += static_cast<std::size_t>(written);
  }
}

luciole::NetworkMessage luciole::network_wire::receive(
    int socket, const CancelToken& token, const std::chrono::steady_clock::time_point& deadline, Fd* descriptor,
    const std::function<void()>& tick
) {
  std::uint32_t length = 0;
  std::size_t offset = 0;
  while (offset < sizeof(length)) {
    waitSocket(socket, POLLIN, token, deadline, tick);
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
    iovec header{reinterpret_cast<char*>(&length) + offset, sizeof(length) - offset};
    msghdr message{};
    message.msg_iov = &header;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const auto read = ::recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (read < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (read <= 0) {
      throwFileError(FileErrorKind::Unavailable, "The filesystem worker disconnected.");
    }
    bool invalid = (message.msg_flags & MSG_CTRUNC) != 0;
    for (auto* cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0)) {
        invalid = true;
        continue;
      }
      const auto bytes = cmsg->cmsg_len - CMSG_LEN(0);
      const bool expected = !invalid && bytes == sizeof(int) && descriptor && !descriptor->valid();
      for (std::size_t position = 0; position + sizeof(int) <= bytes; position += sizeof(int)) {
        int received = -1;
        std::memcpy(&received, CMSG_DATA(cmsg) + position, sizeof(received));
        Fd owned(received);
        if (expected && position == 0) {
          *descriptor = std::move(owned);
        }
      }
      invalid |= !expected;
    }
    if (invalid) {
      throwFileError(FileErrorKind::Unavailable, "Invalid filesystem descriptor response.");
    }
    offset += static_cast<std::size_t>(read);
  }
  const auto size = ntohl(length);
  if (size == 0 || size > maxFrame) {
    throwFileError(FileErrorKind::Unavailable, "Invalid filesystem response size.");
  }
  std::vector<std::uint8_t> bytes(size);
  offset = 0;
  while (offset < bytes.size()) {
    waitSocket(socket, POLLIN, token, deadline, tick);
    const auto read = ::recv(socket, bytes.data() + offset, bytes.size() - offset, MSG_DONTWAIT);
    if (read < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (read <= 0) {
      throwFileError(FileErrorKind::Unavailable, "The filesystem worker disconnected.");
    }
    offset += static_cast<std::size_t>(read);
  }
  return NetworkMessage::from_cbor(bytes, true, true);
}
