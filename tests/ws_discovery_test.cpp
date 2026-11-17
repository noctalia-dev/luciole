#include "fs/ws_discovery_metadata.h"
#include "fs/ws_discovery_protocol.h"
#include "test_check.h"

#include <chrono>
#include <future>
#include <string>

namespace {
  constexpr const char* kSoap = "http://www.w3.org/2003/05/soap-envelope";
  constexpr const char* kOldAddress = "http://schemas.xmlsoap.org/ws/2004/08/addressing";
  constexpr const char* kAddress = "http://www.w3.org/2005/08/addressing";
  constexpr const char* kOldDiscovery = "http://schemas.xmlsoap.org/ws/2005/04/discovery";
  constexpr const char* kDiscovery = "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01";
  constexpr const char* kPublication = "http://schemas.microsoft.com/windows/pub/2005/07";
  std::string
  message(const std::string& kind = "ProbeMatches", bool modern = false, const std::string& types = "n:Computer") {
    const std::string discovery = modern ? kDiscovery : kOldDiscovery;
    const std::string addressing = modern ? kAddress : kOldAddress;
    const std::string item = kind == "ProbeMatches" ? "ProbeMatch" : kind == "ResolveMatches" ? "ResolveMatch" : kind;
    const auto inner = "<a:EndpointReference><a:Address>urn:uuid:server</a:Address></a:EndpointReference>"
                       "<d:Types>"
        + types
        + "</d:Types><d:XAddrs>http://192.0.2.2:5357/device</d:XAddrs>";
    const auto body = kind == item ? "<d:" + kind + '>' + inner + "</d:" + kind + '>'
                                   : "<d:" + kind + "><d:" + item + '>' + inner + "</d:" + item + "></d:" + kind + '>';
    return "<e:Envelope xmlns:e=\""
        + std::string(kSoap)
        + "\" xmlns:a=\""
        + addressing
        + "\" xmlns:d=\""
        + discovery
        + "\" xmlns:n=\""
        + kPublication
        + "\"><e:Header><a:Action>"
        + discovery
        + '/'
        + kind
        + "</a:Action><a:MessageID>urn:uuid:response</a:MessageID><a:RelatesTo>urn:uuid:request</a:RelatesTo>"
          "<d:AppSequence InstanceId=\"7\" MessageNumber=\"42\"/></e:Header><e:Body>"
        + body
        + "</e:Body></e:Envelope>";
  }
  void replace(std::string& text, const std::string& from, const std::string& to) {
    const auto start = text.find(from);
    TEST_CHECK(start != std::string::npos);
    text.replace(start, from.size(), to);
  }
  void packets() {
    using namespace luciole::wsd;
    for (const bool modern : {false, true}) {
      TEST_CHECK(probe("urn:uuid:test", modern).contains("<d:Types>wsdp:Device</d:Types>"));
      TEST_CHECK(metadataRequest("urn:uuid:test", "urn:uuid:server", modern).contains(modern ? kAddress : kOldAddress));
      const auto values = parse(message("ProbeMatches", modern));
      TEST_CHECK(values.size() == 1);
      const auto& entry = values.front();
      TEST_CHECK(entry.kind == Kind::Match && entry.computer && entry.modern == modern);
      TEST_CHECK(entry.endpoint == "urn:uuid:server" && entry.messageId == "urn:uuid:response");
      TEST_CHECK(entry.relatesTo == "urn:uuid:request");
      TEST_CHECK(entry.instance == 7 && entry.sequence == 42);
      TEST_CHECK(entry.addresses.size() == 1 && entry.addresses.front() == "http://192.0.2.2:5357/device");
      const auto resolveReply = parse(message("ResolveMatches", modern));
      TEST_CHECK(resolveReply.size() == 1 && resolveReply.front().kind == Kind::Resolve);
      const auto hello = parse(message("Hello", modern));
      const auto bye = parse(message("Bye", modern));
      TEST_CHECK(hello.size() == 1 && hello.front().kind == Kind::Hello);
      TEST_CHECK(bye.size() == 1 && bye.front().kind == Kind::Bye);
      TEST_CHECK(parse(probe("urn:uuid:test", modern)).empty());
      TEST_CHECK(resolve("urn:uuid:test", "urn:uuid:a&b", modern).contains("a&amp;b"));
    }
    TEST_CHECK(!parse(message("ProbeMatches", false, "n:Printer")).front().computer);
    auto wrong = message();
    replace(wrong, kPublication, "urn:wrong-publication");
    TEST_CHECK(!parse(wrong).front().computer);
    auto namespaceAlias = message();
    replace(namespaceAlias, "xmlns:n=", "xmlns:other=");
    replace(namespaceAlias, "n:Computer", "other:Computer");
    TEST_CHECK(parse(namespaceAlias).front().computer);
    auto defaultNamespace = message();
    replace(defaultNamespace, "<d:Types>", "<d:Types xmlns=\"" + std::string(kPublication) + "\">");
    replace(defaultNamespace, "n:Computer", "Computer");
    TEST_CHECK(parse(defaultNamespace).front().computer);
    auto badEnvelope = message();
    replace(badEnvelope, kSoap, "urn:fake-soap");
    TEST_CHECK(parse(badEnvelope).empty());
    auto spoof = message();
    replace(spoof, "/ProbeMatches</a:Action>", "/Hello</a:Action>");
    TEST_CHECK(parse(spoof).empty());
    auto overflow = message();
    replace(overflow, "InstanceId=\"7\"", "InstanceId=\"18446744073709551616\"");
    TEST_CHECK(!parse(overflow).front().instance);
    auto badEndpoint = message();
    replace(badEndpoint, "urn:uuid:server", "relative-address");
    TEST_CHECK(parse(badEndpoint).empty());
    auto multiple = message();
    const auto start = multiple.find("<d:ProbeMatch>");
    const auto end = multiple.find("</d:ProbeMatch>") + std::string("</d:ProbeMatch>").size();
    const auto extra = multiple.substr(start, end - start);
    multiple.insert(end, extra);
    TEST_CHECK(parse(multiple).size() == 2);
  }
  void xmlBoundaries() {
    using namespace luciole::wsd;
    TEST_CHECK(parse({}).empty());
    TEST_CHECK(parse(std::string(kPacketLimit + 1, 'x')).empty());
    auto nul = message();
    nul.insert(20, 1, '\0');
    TEST_CHECK(parse(nul).empty());
    const auto complete = message();
    for (std::size_t i = 0; i < complete.size(); ++i) {
      TEST_CHECK(parse(std::string_view(complete).substr(0, i)).empty());
    }
    TEST_CHECK(parse("<!DOCTYPE e:Envelope [<!ENTITY x SYSTEM 'file:///nonexistent-luciole'>]>" + complete).empty());
    auto deep = complete;
    const auto insertion = deep.find("</e:Body>");
    std::string nested;
    for (int i = 0; i < 40; ++i) {
      nested += "<x>";
    }
    for (int i = 0; i < 40; ++i) {
      nested += "</x>";
    }
    deep.insert(insertion, nested);
    TEST_CHECK(parse(deep).empty());
    auto count = complete;
    std::string nodes;
    for (int i = 0; i < 4200; ++i) {
      nodes += "<x/>";
    }
    count.insert(insertion, nodes);
    TEST_CHECK(parse(count).empty());
  }
  void metadata() {
    using namespace luciole::wsd;
    const auto wrap = [](const std::string& text) {
      return "<s:Envelope xmlns:s=\""
          + std::string(kSoap)
          + "\" xmlns:p=\""
          + kPublication
          + "\"><s:Body><p:Computer>"
          + text
          + "</p:Computer></s:Body></s:Envelope>";
    };
    TEST_CHECK(metadataName(wrap("NAS/Workgroup:WORKGROUP")) == "NAS");
    TEST_CHECK(metadataName(wrap("NAS\\Domain:example")) == "NAS");
    TEST_CHECK(metadataName(wrap("  café &amp; 家 /NotJoined  ")) == "café & 家");
    TEST_CHECK(metadataName(wrap("bad&#10;name")).empty());
    TEST_CHECK(metadataName(wrap(std::string(257, 'a'))).empty());
    TEST_CHECK(metadataName(std::string(kMetadataLimit + 1, 'a')).empty());
    auto wrong = wrap("NAS");
    replace(wrong, kPublication, "urn:not-publication");
    TEST_CHECK(metadataName(wrong).empty());
    TEST_CHECK(metadataName("<!DOCTYPE s:Envelope [<!ENTITY x 'data'>]>" + wrap("&x;")).empty());
  }
  void addresses() {
    using namespace luciole::wsd;
    const Peer peer{"192.0.2.2", 0};
    const auto target = metadataTarget("http://192.0.2.2:5357/device?q=1", peer);
    TEST_CHECK(target && target->uri == "http://192.0.2.2:5357/device?q=1" && target->host == "192.0.2.2:5357");
    const auto named = metadataTarget("http://nas.local:5357/device", peer);
    TEST_CHECK(named && named->uri == "http://192.0.2.2:5357/device" && named->host == "nas.local:5357");
    TEST_CHECK(!metadataTarget("http://198.51.100.2:5357/device", peer));
    TEST_CHECK(!metadataTarget("http://127.0.0.1:5357/device", peer));
    TEST_CHECK(!metadataTarget("file:///etc/passwd", peer));
    TEST_CHECK(!metadataTarget("http://user:password@192.0.2.2/device", peer));
    TEST_CHECK(!metadataTarget("http://192.0.2.2/device#fragment", peer));
    TEST_CHECK(!metadataTarget("http://192.0.2.2:0/device", peer));
    TEST_CHECK(!metadataTarget("http://bad%0d%0aHost/device", peer));
    TEST_CHECK(!metadataTarget("http://192.0.2.2/device", {"0.0.0.0", 0}));
    TEST_CHECK(!metadataTarget("http://192.0.2.2/device", {"239.255.255.250", 0}));
    TEST_CHECK(smbUri(peer) == "smb://192.0.2.2/");
    TEST_CHECK(smbUri({"fe80::1234", 7}) == "smb://[fe80::1234%257]/");
    const auto scoped = metadataTarget("http://[fe80::1234]:5357/device", {"fe80::1234", 7});
    TEST_CHECK(scoped && scoped->uri.contains("[fe80::1234%257]"));
  }
} // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string_view(argv[1]) == "--metadata") {
    const luciole::CancelToken token;
    const int cancel = std::stoi(argv[4]);
    if (cancel < 0) {
      token.cancel();
    }
    const auto start = std::chrono::steady_clock::now();
    auto result = std::async(std::launch::async, [&] {
      return luciole::wsd::metadata(
          {argv[2], argv[3]}, "urn:uuid:fixture", std::string_view(argv[6]) == "modern", token
      );
    });
    if (cancel > 0) {
      TEST_CHECK(result.wait_for(std::chrono::milliseconds(cancel)) == std::future_status::timeout);
      token.cancel();
    }
    TEST_CHECK(result.get() == argv[5]);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (cancel != 0) {
      TEST_CHECK(elapsed < std::chrono::seconds(2));
    } else if (std::string_view(argv[2]).ends_with("/drip")) {
      TEST_CHECK(elapsed >= std::chrono::seconds(7) && elapsed < std::chrono::seconds(10));
    }
    return 0;
  }
  TEST_CHECK(argc == 1);
  packets();
  xmlBoundaries();
  metadata();
  addresses();
}
