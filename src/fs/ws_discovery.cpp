#include "fs/ws_discovery.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/ws_discovery_metadata.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <utility>

namespace {
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  constexpr std::size_t kMaxServers = 512;
  constexpr std::size_t kMaxSockets = 64;
  constexpr std::size_t kMaxMetadataJobs = 4;
  struct Event {
    std::optional<luciole::wsd::Message> message = std::nullopt;
    luciole::wsd::Peer peer{};
    std::optional<luciole::Location> location = std::nullopt;
    std::optional<luciole::wsd::MetadataTarget> metadata = std::nullopt;
    bool finished = false;
    bool restart = false;
    std::optional<luciole::FileError> error = std::nullopt;
  };
  struct Mailbox {
    std::mutex mutex;
    std::deque<Event> events;
    bool open = true;
    guint source = 0;
  };
  struct Interface {
    std::string name;
    unsigned index;
    int family;
    std::array<unsigned char, 16> address{}, mask{};
  };
  struct Socket {
    luciole::GioPtr<GSocket> socket;
    Interface interface;
    luciole::GioPtr<GSocketAddress> group;
    bool sends = false;
  };
  std::string uuid() {
    const luciole::GString id(g_uuid_string_random());
    return "urn:uuid:" + std::string(id.get());
  }
  std::vector<Interface> interfaces() {
    std::vector<Interface> result;
    ifaddrs* raw = nullptr;
    if (getifaddrs(&raw) != 0) {
      return result;
    }
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owned(raw, freeifaddrs);
    std::set<std::pair<unsigned, int>> added;
    for (auto* it = raw; it && result.size() < kMaxSockets / 2; it = it->ifa_next) {
      if (!it->ifa_addr
          || !it->ifa_netmask
          || !(it->ifa_flags & IFF_UP)
          || !(it->ifa_flags & IFF_MULTICAST)
          || (it->ifa_flags & IFF_LOOPBACK)) {
        continue;
      }
      const int family = it->ifa_addr->sa_family;
      if (family != AF_INET && family != AF_INET6) {
        continue;
      }
      const unsigned index = if_nametoindex(it->ifa_name);
      if (!index || added.contains({index, family})) {
        continue;
      }
      Interface entry{.name = it->ifa_name, .index = index, .family = family};
      if (family == AF_INET) {
        std::memcpy(entry.address.data(), &reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr, 4);
        std::memcpy(entry.mask.data(), &reinterpret_cast<sockaddr_in*>(it->ifa_netmask)->sin_addr, 4);
      } else {
        const auto* address = reinterpret_cast<sockaddr_in6*>(it->ifa_addr);
        if (!IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) {
          continue;
        }
        std::memcpy(entry.address.data(), &address->sin6_addr, 16);
        std::memcpy(entry.mask.data(), &reinterpret_cast<sockaddr_in6*>(it->ifa_netmask)->sin6_addr, 16);
      }
      added.emplace(index, family);
      result.push_back(std::move(entry));
    }
    return result;
  }
  std::optional<Socket> socket(const Interface& interface, bool listener) {
    const auto family = interface.family == AF_INET ? G_SOCKET_FAMILY_IPV4 : G_SOCKET_FAMILY_IPV6;
    luciole::GioPtr<GSocket> sock(g_socket_new(family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, nullptr));
    if (!sock) {
      return {};
    }
    g_socket_set_blocking(sock.get(), FALSE);
    g_socket_set_multicast_ttl(sock.get(), 1);
    const auto local = luciole::GioPtr<GInetAddress>(
        listener ? g_inet_address_new_any(family) : g_inet_address_new_from_bytes(interface.address.data(), family)
    );
    luciole::GioPtr<GSocketAddress> bind(g_inet_socket_address_new(local.get(), listener ? 3702 : 0));
    if (family == G_SOCKET_FAMILY_IPV6) {
      bind.reset(G_SOCKET_ADDRESS(g_object_new(
          G_TYPE_INET_SOCKET_ADDRESS, "address", local.get(), "port", listener ? 3702 : 0, "scope-id", interface.index,
          nullptr
      )));
      if (!g_socket_set_option(sock.get(), IPPROTO_IPV6, IPV6_V6ONLY, 1, nullptr)
          || !g_socket_set_option(
              sock.get(), IPPROTO_IPV6, IPV6_MULTICAST_IF, static_cast<int>(interface.index), nullptr
          )) {
        return {};
      }
    } else {
      ip_mreqn route{};
      route.imr_ifindex = static_cast<int>(interface.index);
      if (setsockopt(g_socket_get_fd(sock.get()), IPPROTO_IP, IP_MULTICAST_IF, &route, sizeof(route)) != 0) {
        return {};
      }
      (void)g_socket_set_option(sock.get(), IPPROTO_IP, IP_MULTICAST_ALL, 0, nullptr);
    }
    if (!g_socket_bind(sock.get(), bind.get(), listener, nullptr)) {
      return {};
    }
    luciole::GioPtr<GInetAddress> group(
        g_inet_address_new_from_string(family == G_SOCKET_FAMILY_IPV4 ? "239.255.255.250" : "ff02::c")
    );
    if (listener && !g_socket_join_multicast_group(sock.get(), group.get(), FALSE, interface.name.c_str(), nullptr)) {
      return {};
    }
    luciole::GioPtr<GSocketAddress> destination(G_SOCKET_ADDRESS(g_object_new(
        G_TYPE_INET_SOCKET_ADDRESS, "address", group.get(), "port", 3702, "scope-id",
        family == G_SOCKET_FAMILY_IPV6 ? interface.index : 0, nullptr
    )));
    return Socket{std::move(sock), interface, std::move(destination), !listener};
  }
  bool peerAllowed(GInetAddress* peer, const Interface& interface) {
    if (g_inet_address_get_is_any(peer)
        || g_inet_address_get_is_multicast(peer)
        || g_inet_address_get_is_loopback(peer)) {
      return false;
    }
    const bool ipv4 = g_inet_address_get_family(peer) == G_SOCKET_FAMILY_IPV4;
    if (ipv4 != (interface.family == AF_INET)) {
      return false;
    }
    const auto* bytes = g_inet_address_to_bytes(peer);
    for (std::size_t i = 0; i < (ipv4 ? 4U : 16U); ++i) {
      if ((bytes[i] & interface.mask[i]) != (interface.address[i] & interface.mask[i])) {
        return false;
      }
    }
    return true;
  }
  void discover(const luciole::CancelToken& token, const std::function<void(Event)>& emit) {
    // Even monitor construction can contact the desktop bus. Keep it on this worker with interface/socket I/O.
    const auto networkChanged = std::make_shared<std::atomic_bool>(false);
    const luciole::GioPtr<GNetworkMonitor> network(G_NETWORK_MONITOR(g_object_ref(g_network_monitor_get_default())));
    const auto handler = g_signal_connect_data(
        network.get(), "network-changed", G_CALLBACK(+[](GNetworkMonitor*, gboolean, gpointer data) {
          (*static_cast<std::shared_ptr<std::atomic_bool>*>(data))->store(true);
        }),
        new std::shared_ptr<std::atomic_bool>(networkChanged),
        +[](gpointer data, GClosure*) { delete static_cast<std::shared_ptr<std::atomic_bool>*>(data); },
        GConnectFlags(0)
    );
    struct Disconnect {
      GObject* object;
      gulong handler;
      ~Disconnect() { g_signal_handler_disconnect(object, handler); }
    } disconnect{G_OBJECT(network.get()), handler};
    std::vector<Socket> sockets;
    for (const auto& interface : interfaces()) {
      if (auto outgoing = socket(interface, false)) {
        sockets.push_back(std::move(*outgoing));
        if (auto listener = socket(interface, true)) {
          sockets.push_back(std::move(*listener));
        }
      }
    }
    if (sockets.empty()) {
      emit(
          {.finished = true,
           .error = luciole::FileError{
               luciole::FileErrorKind::Unsupported, "No multicast-capable network interface is available."
           }}
      );
    }
    struct Pending {
      std::string endpoint;
      Clock::time_point until;
    };
    struct Send {
      std::string text;
      Clock::time_point at;
    };
    std::map<std::string, Pending> pending;
    std::map<std::string, Clock::time_point> seen;
    std::deque<Send> sends;
    auto nextProbe = Clock::now();
    auto readyAt = nextProbe + 10s;
    bool finished = sockets.empty();
    GPollFD cancelFd{};
    const bool pollingCancel = g_cancellable_make_pollfd(luciole::gioCancellable(token), &cancelFd) != FALSE;
    struct Release {
      GCancellable* token;
      ~Release() {
        if (token) {
          g_cancellable_release_fd(token);
        }
      }
    } release{pollingCancel ? luciole::gioCancellable(token) : nullptr};
    std::vector<GPollFD> poll;
    poll.reserve(sockets.size());
    for (const auto& item : sockets) {
      poll.push_back({g_socket_get_fd(item.socket.get()), G_IO_IN, 0});
    }
    if (pollingCancel) {
      poll.push_back(cancelFd);
    }
    while (!token.cancelled()) {
      if (networkChanged->load()) {
        emit({.restart = true});
        return;
      }
      const auto now = Clock::now();
      std::erase_if(pending, [now](const auto& p) { return p.second.until < now; });
      std::erase_if(seen, [now](const auto& p) { return p.second < now; });
      if (now >= nextProbe) {
        for (const bool modern : {false, true}) {
          const auto id = uuid();
          pending.emplace(id, Pending{"", now + 10s});
          const auto text = luciole::wsd::probe(id, modern);
          // SOAP-over-UDP retransmissions reuse MessageID, with bounded randomized backoff.
          for (const int delay : {0, 150, 450, 1050}) {
            sends.push_back({text, now + std::chrono::milliseconds(delay + g_random_int_range(0, 100))});
          }
        }
        nextProbe = now + 60s;
      }
      for (auto it = sends.begin(); it != sends.end();) {
        if (it->at > now) {
          ++it;
          continue;
        }
        for (const auto& item : sockets) {
          if (item.sends) {
            (void)g_socket_send_to(
                item.socket.get(), item.group.get(), it->text.data(), it->text.size(), luciole::gioCancellable(token),
                nullptr
            );
          }
        }
        it = sends.erase(it);
      }
      if (!finished && now >= readyAt) {
        finished = true;
        emit({.finished = true});
      }
      (void)g_poll(poll.data(), static_cast<guint>(poll.size()), 250);
      for (std::size_t i = 0; i < sockets.size() && !token.cancelled(); ++i) {
        if (!(poll[i].revents & G_IO_IN)) {
          continue;
        }
        for (unsigned batch = 0; batch < 32; ++batch) {
          std::array<char, luciole::wsd::kPacketLimit> buffer{};
          GInputVector input{buffer.data(), buffer.size()};
          GSocketAddress* raw = nullptr;
          int flags = 0;
          const auto size = g_socket_receive_message(
              sockets[i].socket.get(), &raw, &input, 1, nullptr, nullptr, &flags, luciole::gioCancellable(token),
              nullptr
          );
          const luciole::GioPtr<GSocketAddress> from(raw);
          if (size <= 0) {
            break;
          }
          if ((flags & MSG_TRUNC) || !G_IS_INET_SOCKET_ADDRESS(from.get())) {
            continue;
          }
          auto* address = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(from.get()));
          if (!peerAllowed(address, sockets[i].interface)) {
            continue;
          }
          const luciole::GString peer(g_inet_address_to_string(address));
          const luciole::wsd::Peer endpoint{
              peer.get(), sockets[i].interface.family == AF_INET6 ? sockets[i].interface.index : 0
          };
          for (auto& message : luciole::wsd::parse({buffer.data(), static_cast<std::size_t>(size)})) {
            const auto request = pending.find(message.relatesTo);
            if ((message.kind == luciole::wsd::Kind::Match || message.kind == luciole::wsd::Kind::Resolve)
                && (request == pending.end()
                    || request->second.until < Clock::now()
                    || (!request->second.endpoint.empty() && request->second.endpoint != message.endpoint)
                    || (!message.computer)
                    || (message.kind == luciole::wsd::Kind::Resolve && request->second.endpoint.empty()))) {
              continue;
            }
            const std::string key = message.messageId + '\n' + message.endpoint + '\n' + endpoint.address;
            if (seen.contains(key) || seen.size() >= 1024) {
              continue;
            }
            seen.emplace(key, Clock::now() + 10s);
            if (message.addresses.empty() && message.kind != luciole::wsd::Kind::Bye && pending.size() < 128) {
              const auto id = uuid();
              pending.emplace(id, Pending{message.endpoint, Clock::now() + 10s});
              sends.push_back({luciole::wsd::resolve(id, message.endpoint, message.modern), Clock::now()});
            }
            // Hello may omit Types. Resolve it before admitting a computer to the SMB list.
            if (message.kind == luciole::wsd::Kind::Hello && !message.computer) {
              if (!message.addresses.empty() && pending.size() < 128) {
                const auto id = uuid();
                pending.emplace(id, Pending{message.endpoint, Clock::now() + 10s});
                sends.push_back({luciole::wsd::resolve(id, message.endpoint, message.modern), Clock::now()});
              }
              continue;
            }
            const auto destination = luciole::Location::fromUri(luciole::wsd::smbUri(endpoint));
            std::optional<luciole::wsd::MetadataTarget> target;
            for (const auto& address : message.addresses) {
              target = luciole::wsd::metadataTarget(address, endpoint);
              if (target) {
                break;
              }
            }
            emit(
                {.message = std::move(message),
                 .peer = endpoint,
                 .location = destination,
                 .metadata = std::move(target)}
            );
          }
        }
      }
    }
  }

} // namespace

struct luciole::WsDiscovery::State : std::enable_shared_from_this<State> {
  struct Record {
    WsServer server;
    Clock::time_point seen;
    std::optional<std::uint64_t> instance, sequence;
    std::string requested;
  };
  WsSnapshot snapshot;
  std::map<std::string, Record> records;
  std::map<std::size_t, std::function<void()>> observers;
  std::size_t nextObserver = 1, jobs = 0;
  CancelToken token;
  std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
  guint expiry = 0, publication = 0;
  bool active = true;
  struct Metadata {
    std::string endpoint;
    wsd::Peer peer;
    wsd::MetadataTarget target;
    bool modern;
  };
  std::deque<Metadata> waiting;

  void publish() {
    snapshot.servers.clear();
    for (const auto& [key, record] : records) {
      (void)key;
      snapshot.servers.push_back(record.server);
    }
    const auto callbacks = observers;
    for (const auto& [id, callback] : callbacks) {
      if (observers.contains(id)) {
        callback();
      }
    }
  }
  void schedulePublish() {
    if (publication || !active) {
      return;
    }
    // Collapse bursts to one bounded UI publication, rather than rebuilding views for every packet.
    publication = g_timeout_add_full(
        G_PRIORITY_DEFAULT_IDLE, 50,
        +[](gpointer data) -> gboolean {
          if (const auto state = static_cast<std::weak_ptr<State>*>(data)->lock(); state && state->active) {
            state->publication = 0;
            state->publish();
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        +[](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
  }
  void stop() {
    active = false;
    observers.clear();
    token.cancel();
    {
      const std::scoped_lock lock(mailbox->mutex);
      mailbox->open = false;
      mailbox->events.clear();
      if (mailbox->source) {
        g_source_remove(std::exchange(mailbox->source, 0));
      }
    }
    if (publication) {
      g_source_remove(std::exchange(publication, 0));
    }
    if (expiry) {
      g_source_remove(std::exchange(expiry, 0));
    }
    waiting.clear();
  }
  void pump() {
    while (active && jobs < kMaxMetadataJobs && !waiting.empty()) {
      auto request = std::move(waiting.front());
      waiting.pop_front();
      ++jobs;
      const auto generation = token;
      const CancelToken deadline;
      const auto connection = g_cancellable_connect(
          gioCancellable(generation),
          G_CALLBACK(+[](GCancellable*, gpointer data) { static_cast<CancelToken*>(data)->cancel(); }),
          new CancelToken(deadline), +[](gpointer data) { delete static_cast<CancelToken*>(data); }
      );
      const guint timeout = g_timeout_add_full(
          G_PRIORITY_DEFAULT, 8000,
          +[](gpointer data) -> gboolean {
            static_cast<CancelToken*>(data)->cancel();
            return G_SOURCE_CONTINUE;
          },
          new CancelToken(deadline), +[](gpointer data) { delete static_cast<CancelToken*>(data); }
      );
      runJob<std::string>(
          deadline,
          [request, deadline] { return wsd::metadata(request.target, request.endpoint, request.modern, deadline); },
          [weak = weak_from_this(), request, generation, connection, timeout](JobResult<std::string> result) {
            g_source_remove(timeout);
            if (connection) {
              g_cancellable_disconnect(gioCancellable(generation), connection);
            }
            const auto state = weak.lock();
            if (!state || !state->active || generation.cancelled()) {
              return;
            }
            --state->jobs;
            const auto record = state->records.find(request.endpoint);
            if (result
                && !result->empty()
                && record != state->records.end()
                && record->second.server.peer == request.peer
                && record->second.server.name != *result) {
              record->second.server.name = std::move(*result);
              state->schedulePublish();
            }
            if (state->active) {
              state->pump();
            }
          }
      );
    }
  }
  void receive(Event event) {
    if (event.restart) {
      records.clear();
      refresh();
      return;
    }
    if (event.finished) {
      snapshot.searching = false;
      snapshot.error = std::move(event.error);
      schedulePublish();
      return;
    }
    if (!event.message || !event.location) {
      return;
    }
    auto& message = *event.message;
    auto record = records.find(message.endpoint);
    if (record != records.end()
        && message.instance
        && message.sequence
        && record->second.instance
        && (*message.instance < *record->second.instance
            || (*message.instance == *record->second.instance
                && record->second.sequence
                && *message.sequence < *record->second.sequence))) {
      return;
    }
    if (message.kind == wsd::Kind::Bye) {
      if (record != records.end() && record->second.server.peer == event.peer) {
        records.erase(record);
        schedulePublish();
      }
      return;
    }
    if (record == records.end()) {
      if (records.size() >= kMaxServers) {
        return;
      }
      record = records
                   .emplace(
                       message.endpoint,
                       Record{
                           .server = {message.endpoint, event.peer.address, event.peer, *event.location},
                           .seen = Clock::now(),
                           .instance = message.instance,
                           .sequence = message.sequence,
                           .requested = {}
                       }
                   )
                   .first;
    } else {
      record->second.seen = Clock::now();
      record->second.instance = message.instance;
      record->second.sequence = message.sequence;
      // A native IPv4 endpoint avoids ambiguous link-local scope when both families advertise one computer.
      if (record->second.server.peer.address.contains(':') && !event.peer.address.contains(':')) {
        record->second.server.peer = event.peer;
        record->second.server.location = *event.location;
        record->second.requested.clear();
      }
    }
    snapshot.searching = false;
    if (event.metadata
        && active
        && record->second.server.peer == event.peer
        && waiting.size() < kMaxServers
        && event.metadata->uri != record->second.requested) {
      record->second.requested = event.metadata->uri;
      waiting.push_back({message.endpoint, event.peer, std::move(*event.metadata), message.modern});
      pump();
    }
    schedulePublish();
  }
  void refresh() {
    if (!active) {
      return;
    }
    token.cancel();
    {
      const std::scoped_lock lock(mailbox->mutex);
      mailbox->open = false;
      mailbox->events.clear();
      if (mailbox->source) {
        g_source_remove(std::exchange(mailbox->source, 0));
      }
    }
    mailbox = std::make_shared<Mailbox>();
    token = CancelToken{};
    const auto generation = token;
    waiting.clear();
    jobs = 0;
    for (auto& [key, record] : records) {
      (void)key;
      record.requested.clear();
    }
    snapshot.searching = records.empty();
    snapshot.error.reset();
    if (!expiry) {
      expiry = g_timeout_add_seconds_full(
          G_PRIORITY_DEFAULT_IDLE, 15,
          +[](gpointer data) -> gboolean {
            if (const auto state = static_cast<std::weak_ptr<State>*>(data)->lock(); state && state->active) {
              const auto cutoff = Clock::now() - 180s;
              if (std::erase_if(state->records, [cutoff](const auto& r) { return r.second.seen < cutoff; })) {
                state->publish();
              }
              return G_SOURCE_CONTINUE;
            }
            return G_SOURCE_REMOVE;
          },
          new std::weak_ptr<State>(weak_from_this()),
          +[](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
      );
    }
    runJob<int>(
        generation,
        [weak = weak_from_this(), queue = mailbox, generation] {
          discover(generation, [weak, queue, generation](Event event) {
            const std::scoped_lock lock(queue->mutex);
            if (!queue->open || generation.cancelled() || queue->events.size() >= 1024) {
              return;
            }
            queue->events.push_back(std::move(event));
            if (queue->source) {
              return;
            }
            struct Delivery {
              std::weak_ptr<State> state;
              std::shared_ptr<Mailbox> queue;
              CancelToken token;
            };
            queue->source = g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer data) -> gboolean {
                  const auto delivery = *static_cast<Delivery*>(data);
                  for (unsigned count = 0; count < 32; ++count) {
                    Event next;
                    {
                      const std::scoped_lock guard(delivery.queue->mutex);
                      if (delivery.queue->events.empty()) {
                        delivery.queue->source = 0;
                        return G_SOURCE_REMOVE;
                      }
                      next = std::move(delivery.queue->events.front());
                      delivery.queue->events.pop_front();
                    }
                    if (const auto state = delivery.state.lock();
                        state && state->active && !delivery.token.cancelled()) {
                      state->receive(std::move(next));
                    }
                  }
                  return G_SOURCE_CONTINUE;
                },
                new Delivery{weak, queue, generation}, +[](gpointer data) { delete static_cast<Delivery*>(data); }
            );
          });
          return 0;
        },
        [weak = weak_from_this(), generation](JobResult<int> result) {
          if (const auto state = weak.lock(); state && state->active && !generation.cancelled() && !result) {
            state->snapshot.searching = false;
            state->snapshot.error = result.error();
            state->publish();
          }
        }
    );
    publish();
  }
};

luciole::WsDiscovery::WsDiscovery() : m_state(std::make_shared<State>()) {}
luciole::WsDiscovery::~WsDiscovery() { m_state->stop(); }
void luciole::WsDiscovery::refresh() {
  const auto state = m_state;
  state->refresh();
}
std::size_t luciole::WsDiscovery::subscribe(std::function<void()> callback) {
  const auto id = m_state->nextObserver++;
  m_state->observers.emplace(id, std::move(callback));
  return id;
}
void luciole::WsDiscovery::unsubscribe(std::size_t id) { m_state->observers.erase(id); }
const luciole::WsSnapshot& luciole::WsDiscovery::snapshot() const { return m_state->snapshot; }
