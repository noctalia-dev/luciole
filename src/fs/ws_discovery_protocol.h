#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::wsd {
  inline constexpr std::size_t kPacketLimit = 65536;
  inline constexpr std::size_t kMetadataLimit = 262144;
  enum class Kind : std::uint8_t { Match, Resolve, Hello, Bye };
  struct Message {
    Kind kind;
    std::string endpoint, messageId, relatesTo;
    std::vector<std::string> addresses;
    bool modern = false;
    bool computer = false;
    std::optional<std::uint64_t> instance, sequence;
  };
  struct Peer {
    std::string address;
    std::uint32_t scope = 0;
    bool operator==(const Peer&) const = default;
  };
  struct MetadataTarget {
    std::string uri;
    std::string host;
  };
  std::vector<Message> parse(std::string_view xml);
  std::string probe(const std::string& id, bool modern);
  std::string resolve(const std::string& id, const std::string& endpoint, bool modern);
  std::string metadataRequest(const std::string& id, const std::string& endpoint, bool modern = false);
  std::string metadataName(std::string_view xml);
  // Metadata connects only to the numeric discovery sender, with the advertised HTTP host preserved.
  std::optional<MetadataTarget> metadataTarget(const std::string& uri, const Peer& peer);
  std::string smbUri(const Peer& peer);
} // namespace luciole::wsd
