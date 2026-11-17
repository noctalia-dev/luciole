#include "fs/network_listing.h"

#include "fs/background_job.h"
#include "fs/directory_session.h"
#include "fs/ws_discovery.h"

#include <map>
#include <set>
#include <utility>

namespace {
  std::shared_ptr<luciole::WsDiscovery> discovery() {
    // Only open Network listings retain this client. The last listing releases sockets/workers.
    static std::weak_ptr<luciole::WsDiscovery> live;
    auto result = live.lock();
    if (!result) {
      result = std::make_shared<luciole::WsDiscovery>();
      live = result;
      result->refresh();
    }
    return result;
  }
} // namespace

struct luciole::NetworkListing::State : std::enable_shared_from_this<State> {
  NetworkSnapshot snapshot;
  DirectorySession dns{512};
  std::shared_ptr<WsDiscovery> ws;
  std::size_t subscription = 0;
  std::function<void()> changed;
  bool active = true, started = false;
  SharedFileIcon serverIcon = std::make_shared<const FileIconSource>(
      FileIconSource{.names = {"network-server", "folder-remote", "folder"}, .filePath = {}, .desktopFile = {}}
  );

  void stop() {
    active = false;
    changed = {};
    dns.setChangedCallback({});
    dns.cancel();
    if (ws) {
      ws->unsubscribe(subscription);
      ws.reset();
    }
  }
  void publish() {
    if (!active || !ws) {
      return;
    }
    snapshot.entries.clear();
    const auto& native = ws->snapshot();
    const auto& local = dns.snapshot();
    snapshot.loading = native.searching || (local.status == DirectoryStatus::Loading && local.entries.empty());
    snapshot.warning = native.error;
    if (!snapshot.warning && local.error) {
      snapshot.warning =
          FileError{FileErrorKind::Unavailable, "DNS-SD discovery is unavailable: " + local.error->message};
    }
    std::set<std::string> destinations;
    for (const auto& server : native.servers) {
      const auto& location = server.location;
      if (!destinations.insert(location.uri()).second) {
        continue;
      }
      DirectoryEntry entry{
          .place = location,
          .name = "wsd:" + server.endpoint,
          .type = FileType::Directory,
          .size = {},
          .modifiedSeconds = {},
          .access = {.read = true, .write = false, .execute = true, .rename = false, .trash = false, .remove = false},
          .icon = serverIcon,
          .contentType = "inode/directory"
      };
      entry.setDisplayName(server.name);
      snapshot.entries.push_back(std::move(entry));
    }
    for (const auto& entry : local.entries) {
      const auto* target = entry.shortcutTarget();
      // This root represents SMB computers, rather than unrelated DNS-SD services such as printers.
      if (!target
          || entry.type != FileType::Shortcut
          || !target->uri().starts_with("smb:")
          || !destinations.insert(target->uri()).second
          || snapshot.entries.size() >= 1024) {
        continue;
      }
      auto copy = entry;
      const auto& display = entry.displayName();
      copy.place = entry.location();
      copy.name = "dnssd:" + entry.name;
      copy.setDisplayName(display);
      snapshot.entries.push_back(std::move(copy));
    }
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void start() {
    if (std::exchange(started, true)) {
      return;
    }
    ws = discovery();
    subscription = ws->subscribe([weak = weak_from_this()] {
      if (const auto state = weak.lock()) {
        state->publish();
      }
    });
    dns.setChangedCallback([weak = weak_from_this()] {
      if (const auto state = weak.lock()) {
        state->publish();
      }
    });
    runJob<Location>(
        [] { return Location::fromUri("dns-sd://local/"); },
        [weak = weak_from_this()](JobResult<Location> location) {
          if (const auto state = weak.lock(); state && state->active && location) {
            state->dns.open(*location);
          }
        }
    );
    publish();
  }
  void refresh() {
    if (!started) {
      start();
      return;
    }
    ws->refresh();
    dns.refresh();
  }
};

luciole::NetworkListing::NetworkListing() : m_state(std::make_shared<State>()) {}
luciole::NetworkListing::~NetworkListing() { m_state->stop(); }
void luciole::NetworkListing::start() {
  const auto state = m_state;
  state->start();
}
void luciole::NetworkListing::refresh() {
  const auto state = m_state;
  state->refresh();
}
void luciole::NetworkListing::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
const luciole::NetworkSnapshot& luciole::NetworkListing::snapshot() const { return m_state->snapshot; }
