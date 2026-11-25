#include "fs/system_mount_monitor.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/systemd_mounts.h"

#include <algorithm>
#include <array>
#include <gio/gunixmounts.h>
#include <string_view>

struct luciole::SystemMountMonitor::State : std::enable_shared_from_this<State> {
  GioPtr<GUnixMountMonitor> monitor;
  GioPtr<GDBusConnection> bus;
  std::vector<guint> subscriptions;
  bool subscribed = false;
  CancelToken token;
  std::vector<SystemMount> mounts;
  std::function<void()> changed;
  bool pending = false;
  bool dirty = false;

  // Subscribe() makes systemd emit signals for every unit; these match rules let the bus deliver only the
  // mount-related ones.
  void subscribe() {
    struct Rule {
      const char* interface;
      const char* member;
      const char* arg0;
    };
    constexpr std::array<Rule, 5> kRules{{
        {.interface = "org.freedesktop.systemd1.Manager", .member = "UnitNew", .arg0 = nullptr},
        {.interface = "org.freedesktop.systemd1.Manager", .member = "UnitRemoved", .arg0 = nullptr},
        {.interface = "org.freedesktop.systemd1.Manager", .member = "Reloading", .arg0 = nullptr},
        {.interface = "org.freedesktop.DBus.Properties",
         .member = "PropertiesChanged",
         .arg0 = "org.freedesktop.systemd1.Mount"},
        {.interface = "org.freedesktop.DBus.Properties",
         .member = "PropertiesChanged",
         .arg0 = "org.freedesktop.systemd1.Automount"},
    }};
    for (const auto& rule : kRules) {
      subscriptions.push_back(g_dbus_connection_signal_subscribe(
          bus.get(), "org.freedesktop.systemd1", rule.interface, rule.member, nullptr, rule.arg0,
          G_DBUS_SIGNAL_FLAGS_NONE,
          [](GDBusConnection*, const char*, const char*, const char*, const char* member, GVariant* parameters,
             gpointer data) { static_cast<State*>(data)->signalled(member, parameters); },
          this, nullptr
      ));
    }
  }

  // Unit additions and removals still name every unit type, and a reload reports its start and its end.
  void signalled(std::string_view member, GVariant* parameters) {
    if (member == "UnitNew" || member == "UnitRemoved") {
      if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(so)"))) {
        return;
      }
      const char* unit = nullptr;
      g_variant_get_child(parameters, 0, "&s", &unit);
      if (!std::string_view(unit).ends_with(".mount") && !std::string_view(unit).ends_with(".automount")) {
        return;
      }
    } else if (member == "Reloading") {
      gboolean reloading = TRUE;
      if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) {
        g_variant_get(parameters, "(b)", &reloading);
      }
      if (reloading) {
        return;
      }
    }
    refresh();
  }

  void refresh() {
    if (pending) {
      dirty = true;
      return;
    }
    pending = true;
    dirty = false;
    struct Reply {
      GioPtr<GDBusConnection> bus;
      bool subscribed;
      std::optional<std::vector<MountConfiguration>> native;
      std::vector<SystemMount> mounts;
    };
    runJob<Reply>(
        token,
        [token = token, subscribed = subscribed,
         connection = GioPtr<GDBusConnection>(bus ? G_DBUS_CONNECTION(g_object_ref(bus.get())) : nullptr)] mutable {
          Reply reply;
          reply.subscribed = subscribed;
          if (!connection) {
            connection.reset(g_bus_get_sync(G_BUS_TYPE_SYSTEM, gioCancellable(token), nullptr));
          }
          reply.bus = std::move(connection);
          reply.native = readSystemdMounts(reply.bus.get(), token, &reply.subscribed);
          auto configuration =
              reply.native ? mergeMountConfiguration(readFstabMounts(), *reply.native) : configuredSystemMounts();
          reply.mounts = systemMounts(configuration, readMountedFilesystems(), g_get_home_dir(), g_get_user_name());
          return reply;
        },
        [weak = weak_from_this()](JobResult<Reply> reply) {
          const auto state = weak.lock();
          if (!state || state->token.cancelled()) {
            return;
          }
          state->pending = false;
          if (reply) {
            state->subscribed = reply->subscribed;
            if (reply->native) {
              publishNativeMounts(std::move(*reply->native));
            }

            if (!state->bus && reply->bus) {
              state->bus = std::move(reply->bus);
              state->subscribe();
              // Cover unit changes between the first snapshot and attaching the signal handler.
              state->dirty = true;
            }
            if (state->mounts != reply->mounts) {
              state->mounts = std::move(reply->mounts);
              if (state->changed) {
                state->changed();
              }
            }
          }
          if (state->dirty && !state->token.cancelled()) {
            state->refresh();
          }
        }
    );
  }
};

luciole::SystemMountMonitor::SystemMountMonitor() : m_state(std::make_shared<State>()) {}

luciole::SystemMountMonitor::~SystemMountMonitor() {
  m_state->token.cancel();
  m_state->changed = {};
  if (m_state->monitor) {
    g_signal_handlers_disconnect_by_data(m_state->monitor.get(), m_state.get());
  }
  for (const auto subscription : m_state->subscriptions) {
    g_dbus_connection_signal_unsubscribe(m_state->bus.get(), subscription);
  }
}

void luciole::SystemMountMonitor::start(std::function<void()> changed) {
  if (m_state->monitor) {
    return;
  }
  m_state->changed = std::move(changed);
  m_state->monitor.reset(g_unix_mount_monitor_get());
  for (const auto* signal : {"mounts-changed", "mountpoints-changed"}) {
    g_signal_connect_swapped(
        m_state->monitor.get(), signal, G_CALLBACK(+[](gpointer data) { static_cast<State*>(data)->refresh(); }),
        m_state.get()
    );
  }
  m_state->refresh();
}

const std::vector<luciole::SystemMount>& luciole::SystemMountMonitor::mounts() const { return m_state->mounts; }
