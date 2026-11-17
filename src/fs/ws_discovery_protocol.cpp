#include "fs/ws_discovery_protocol.h"

#include "fs/gio_ptr.h"

#include <algorithm>
#include <charconv>
#include <gio/gio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <memory>
#include <utility>

namespace {
  constexpr const char* kSoap = "http://www.w3.org/2003/05/soap-envelope";
  constexpr const char* kOldDiscovery = "http://schemas.xmlsoap.org/ws/2005/04/discovery";
  constexpr const char* kDiscovery = "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01";
  constexpr const char* kOldAddressing = "http://schemas.xmlsoap.org/ws/2004/08/addressing";
  constexpr const char* kAddressing = "http://www.w3.org/2005/08/addressing";
  constexpr const char* kPublication = "http://schemas.microsoft.com/windows/pub/2005/07";
  using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
  std::string value(const xmlChar* text) { return text ? reinterpret_cast<const char*>(text) : ""; }
  bool is(const xmlNode* node, const char* name, const char* space) {
    return node
        && node->type == XML_ELEMENT_NODE
        && value(node->name) == name
        && node->ns
        && value(node->ns->href) == space;
  }
  xmlNode* child(xmlNode* parent, const char* name, const char* space) {
    for (auto* node = parent ? parent->children : nullptr; node; node = node->next) {
      if (is(node, name, space)) {
        return node;
      }
    }
    return nullptr;
  }
  std::string content(xmlNode* node) {
    if (!node) {
      return {};
    }
    auto* raw = xmlNodeGetContent(node);
    auto result = value(raw);
    xmlFree(raw);
    return result;
  }
  std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  }
  std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> result;
    std::size_t offset = 0;
    while ((offset = text.find_first_not_of(" \t\r\n", offset)) != std::string::npos && result.size() < 32) {
      const auto end = text.find_first_of(" \t\r\n", offset);
      result.push_back(text.substr(offset, end == std::string::npos ? end : end - offset));
      if (end == std::string::npos) {
        break;
      }
      offset = end;
    }
    return result;
  }
  bool bounded(const xmlNode* node, unsigned depth, std::size_t& count) {
    for (; node; node = node->next) {
      if (++count > 4096 || depth > 32 || !bounded(node->children, depth + 1, count)) {
        return false;
      }
    }
    return true;
  }
  Document document(std::string_view text, std::size_t limit) {
    Document result(nullptr, xmlFreeDoc);
    if (text.empty() || text.size() > limit || text.contains('\0')) {
      return result;
    }
    // No entity substitution, DTD loading, recovery or external resources. Reject DTDs before reading content.
    result.reset(xmlReadMemory(
        text.data(), static_cast<int>(text.size()), nullptr, nullptr,
        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING
    ));
    std::size_t count = 0;
    if (result && (result->intSubset || result->extSubset || !bounded(xmlDocGetRootElement(result.get()), 0, count))) {
      result.reset();
    }
    return result;
  }
  std::optional<std::uint64_t> number(xmlNode* node, const char* name) {
    auto* raw = xmlGetProp(node, reinterpret_cast<const xmlChar*>(name));
    const auto text = value(raw);
    xmlFree(raw);
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional(result)
                                                                                                : std::nullopt;
  }
  bool computer(xmlNode* types) {
    return std::ranges::any_of(words(content(types)), [types](const auto& type) {
      const auto colon = type.find(':');
      const auto local = colon == std::string::npos ? type : type.substr(colon + 1);
      if (local != "Computer") {
        return false;
      }
      const auto prefix = colon == std::string::npos ? "" : type.substr(0, colon);
      const auto* space =
          xmlSearchNs(types->doc, types, prefix.empty() ? nullptr : reinterpret_cast<const xmlChar*>(prefix.c_str()));
      return space && value(space->href) == kPublication;
    });
  }

  std::string escape(const std::string& text) {
    const luciole::GString escaped(g_markup_escape_text(text.c_str(), static_cast<gssize>(text.size())));
    return escaped.get();
  }
  std::string envelope(
      const std::string& id, const std::string& action, const std::string& to, const std::string& body, bool modern
  ) {
    const auto* addressing = modern ? kAddressing : kOldAddressing;
    const std::string anonymous = std::string(addressing) + (modern ? "/anonymous" : "/role/anonymous");
    return R"(<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s=")"
        + std::string(kSoap)
        + "\" xmlns:a=\""
        + addressing
        + "\"><s:Header><a:Action>"
        + action
        + "</a:Action><a:MessageID>"
        + escape(id)
        + "</a:MessageID><a:To>"
        + escape(to)
        + "</a:To><a:ReplyTo><a:Address>"
        + anonymous
        + "</a:Address></a:ReplyTo></s:Header><s:Body>"
        + body
        + "</s:Body></s:Envelope>";
  }
  xmlNode* find(xmlNode* node, const char* name, const char* space) {
    for (; node; node = node->next) {
      if (is(node, name, space)) {
        return node;
      }
      if (auto* found = find(node->children, name, space)) {
        return found;
      }
    }
    return nullptr;
  }
  std::string host(const luciole::wsd::Peer& peer, bool uri) {
    if (!peer.address.contains(':')) {
      return peer.address;
    }
    return peer.address + (peer.scope ? (uri ? "%25" : "%") + std::to_string(peer.scope) : "");
  }
} // namespace

std::vector<luciole::wsd::Message> luciole::wsd::parse(std::string_view xml) {
  std::vector<Message> result;
  const auto doc = document(xml, kPacketLimit);
  auto* root = doc ? xmlDocGetRootElement(doc.get()) : nullptr;
  if (!is(root, "Envelope", kSoap)) {
    return result;
  }
  auto* header = child(root, "Header", kSoap);
  auto* body = child(root, "Body", kSoap);
  if (!header || !body) {
    return result;
  }
  const char* addressing = child(header, "Action", kOldAddressing) ? kOldAddressing : kAddressing;
  const auto action = trim(content(child(header, "Action", addressing)));
  const auto id = trim(content(child(header, "MessageID", addressing)));
  const auto relates = trim(content(child(header, "RelatesTo", addressing)));
  if (id.empty() || id.size() > 2048 || relates.size() > 2048) {
    return result;
  }
  for (const bool modern : {false, true}) {
    const auto* space = modern ? kDiscovery : kOldDiscovery;
    for (const auto& [name, kind] :
         {std::pair{"ProbeMatches", Kind::Match},
          {"ResolveMatches", Kind::Resolve},
          {"Hello", Kind::Hello},
          {"Bye", Kind::Bye}}) {
      if (action != std::string(space) + '/' + name) {
        continue;
      }
      auto* group = child(body, name, space);
      if (!group) {
        continue;
      }
      auto* first = kind == Kind::Match ? group->children
          : kind == Kind::Resolve       ? child(group, "ResolveMatch", space)
                                        : group;
      for (auto* node = first; node; node = kind == Kind::Match ? node->next : nullptr) {
        if (kind == Kind::Match && !is(node, "ProbeMatch", space)) {
          continue;
        }
        auto* epr = child(node, "EndpointReference", addressing);
        auto endpoint = trim(content(child(epr, "Address", addressing)));
        if (endpoint.empty()
            || endpoint.size() > 2048
            || !g_uri_is_valid(endpoint.c_str(), G_URI_FLAGS_NONE, nullptr)) {
          continue;
        }
        auto* sequence = child(header, "AppSequence", space);
        result.push_back(
            {.kind = kind,
             .endpoint = std::move(endpoint),
             .messageId = id,
             .relatesTo = relates,
             .addresses = words(content(child(node, "XAddrs", space))),
             .modern = modern,
             .computer = computer(child(node, "Types", space)),
             .instance = sequence ? number(sequence, "InstanceId") : std::nullopt,
             .sequence = sequence ? number(sequence, "MessageNumber") : std::nullopt}
        );
      }
    }
  }
  return result;
}

std::string luciole::wsd::probe(const std::string& id, bool modern) {
  const auto* space = modern ? kDiscovery : kOldDiscovery;
  const auto* to =
      modern ? "urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01" : "urn:schemas-xmlsoap-org:ws:2005:04:discovery";
  const auto* profile =
      modern ? "http://docs.oasis-open.org/ws-dd/ns/dpws/2009/01" : "http://schemas.xmlsoap.org/ws/2006/02/devprof";
  return envelope(
      id, std::string(space) + "/Probe", to,
      "<d:Probe xmlns:d=\""
          + std::string(space)
          + "\" xmlns:wsdp=\""
          + profile
          + "\"><d:Types>wsdp:Device</d:Types></d:Probe>",
      modern
  );
}
std::string luciole::wsd::resolve(const std::string& id, const std::string& endpoint, bool modern) {
  const auto* space = modern ? kDiscovery : kOldDiscovery;
  const auto* to =
      modern ? "urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01" : "urn:schemas-xmlsoap-org:ws:2005:04:discovery";
  return envelope(
      id, std::string(space) + "/Resolve", to,
      "<d:Resolve xmlns:d=\""
          + std::string(space)
          + "\"><a:EndpointReference><a:Address>"
          + escape(endpoint)
          + "</a:Address></a:EndpointReference></d:Resolve>",
      modern
  );
}
std::string luciole::wsd::metadataRequest(const std::string& id, const std::string& endpoint, bool modern) {
  return envelope(id, "http://schemas.xmlsoap.org/ws/2004/09/transfer/Get", endpoint, "", modern);
}
std::string luciole::wsd::metadataName(std::string_view xml) {
  const auto doc = document(xml, kMetadataLimit);
  auto* root = doc ? xmlDocGetRootElement(doc.get()) : nullptr;
  auto* node = is(root, "Envelope", kSoap) ? find(child(root, "Body", kSoap), "Computer", kPublication) : nullptr;
  auto result = trim(content(node));
  result = trim(result.substr(0, result.find_first_of("/\\")));
  if (result.empty()
      || result.size() > 256
      || !g_utf8_validate(result.c_str(), static_cast<gssize>(result.size()), nullptr)) {
    return {};
  }
  for (const auto ch : result) {
    if (static_cast<unsigned char>(ch) < 32 || ch == 127) {
      return {};
    }
  }
  return result;
}
std::optional<luciole::wsd::MetadataTarget> luciole::wsd::metadataTarget(const std::string& uri, const Peer& peer) {
  const GioPtr<GInetAddress> address(g_inet_address_new_from_string(peer.address.c_str()));
  if (!address
      || g_inet_address_get_is_any(address.get())
      || g_inet_address_get_is_multicast(address.get())
      || g_inet_address_get_is_loopback(address.get())) {
    return {};
  }
  GUri* parsed = g_uri_parse(uri.c_str(), G_URI_FLAGS_ENCODED, nullptr);
  if (!parsed) {
    return {};
  }
  const std::unique_ptr<GUri, decltype(&g_uri_unref)> owned(parsed, g_uri_unref);
  const std::string scheme = g_uri_get_scheme(parsed) ? g_uri_get_scheme(parsed) : "";
  const char* advertised = g_uri_get_host(parsed);
  if ((scheme != "http" && scheme != "https")
      || !advertised
      || !*advertised
      || g_uri_get_userinfo(parsed)
      || g_uri_get_fragment(parsed)
      || uri.size() > 4096
      || g_uri_get_port(parsed) == 0) {
    return {};
  }
  for (const char ch : std::string_view(advertised)) {
    if (static_cast<unsigned char>(ch) <= 32 || static_cast<unsigned char>(ch) >= 127) {
      return {};
    }
  }
  const GioPtr<GInetAddress> numeric(g_inet_address_new_from_string(advertised));
  if (numeric && !g_inet_address_equal(numeric.get(), address.get())) {
    return {};
  }
  const std::string numericHost = host(peer, true);
  GUri* pinned = g_uri_build(
      G_URI_FLAGS_ENCODED, scheme.c_str(), nullptr, numericHost.c_str(), g_uri_get_port(parsed), g_uri_get_path(parsed),
      g_uri_get_query(parsed), nullptr
  );
  if (!pinned) {
    return {};
  }
  const GString text(g_uri_to_string(pinned));
  g_uri_unref(pinned);
  std::string authority = advertised;
  if (authority.contains(':')) {
    authority = '[' + authority + ']';
  }
  if (g_uri_get_port(parsed) != -1) {
    authority += ':' + std::to_string(g_uri_get_port(parsed));
  }
  return MetadataTarget{text.get(), std::move(authority)};
}
std::string luciole::wsd::smbUri(const Peer& peer) {
  const std::string address = host(peer, true);
  return "smb://" + (peer.address.contains(':') ? '[' + address + ']' : address) + '/';
}
