#include "fs/system_mount_monitor.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/systemd_mounts.h"

#include <algorithm>
#include <gio/gunixmounts.h>

struct luciole::SystemMountMonitor::State : std::enable_shared_from_this<State> {
  GioPtr<GUnixMountMonitor> monitor;
  GioPtr<GDBusConnection> bus;
  guint subscription = 0;
  bool subscribed = false;
  CancelToken token;
  std::vector<SystemMount> mounts;
  std::function<void()> changed;
  bool pending = false;
  bool dirty = false;

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
              state->subscription = g_dbus_connection_signal_subscribe(
                  state->bus.get(), "org.freedesktop.systemd1", nullptr, nullptr, nullptr, nullptr,
                  G_DBUS_SIGNAL_FLAGS_NONE,
                  [](GDBusConnection*, const char*, const char*, const char* interface, const char* signal,
                     GVariant* parameters, gpointer data) {
                    bool relevant = false;
                    if (std::string_view(interface) == "org.freedesktop.systemd1.Manager") {
                      if (std::string_view(signal) == "UnitNew" || std::string_view(signal) == "UnitRemoved") {
                        const char* unit = nullptr;
                        g_variant_get_child(parameters, 0, "&s", &unit);
                        relevant = std::string_view(unit).ends_with(".mount")
                            || std::string_view(unit).ends_with(".automount");
                      } else if (std::string_view(signal) == "Reloading") {
                        gboolean reloading = FALSE;
                        g_variant_get(parameters, "(b)", &reloading);
                        relevant = !reloading;
                      }
                    }
                    if (std::string_view(interface) == "org.freedesktop.DBus.Properties"
                        && std::string_view(signal) == "PropertiesChanged") {
                      const char* changedInterface = nullptr;
                      g_variant_get_child(parameters, 0, "&s", &changedInterface);
                      relevant = std::string_view(changedInterface) == "org.freedesktop.systemd1.Mount"
                          || std::string_view(changedInterface) == "org.freedesktop.systemd1.Automount";
                    }
                    if (relevant) {
                      static_cast<State*>(data)->refresh();
                    }
                  },
                  state.get(), nullptr
              );
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
  if (m_state->subscription) {
    g_dbus_connection_signal_unsubscribe(m_state->bus.get(), m_state->subscription);
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
