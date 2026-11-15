#include "fs/drive_monitor.h"

#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"

#include <gio/gio.h>
#include <map>

namespace {
  // Owned reference to the GVolume or GMount behind one Drive.
  struct Handle {
    luciole::GioPtr<GVolume> volume;
    luciole::GioPtr<GMount> mount;
  };

  std::string identifier(GVolume* volume) {
    for (const auto* kind : {G_VOLUME_IDENTIFIER_KIND_UUID, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE}) {
      if (const luciole::GString value(g_volume_get_identifier(volume, kind)); value) {
        return value.get();
      }
    }
    const luciole::GString name(g_volume_get_name(volume));
    return name ? name.get() : "";
  }

  std::string errorText(GError* error) {
    const luciole::GErrorPtr owned(error);
    return owned ? owned->message : "";
  }
} // namespace

struct luciole::DriveMonitor::State : std::enable_shared_from_this<State> {
  // Null until start(): GIO starts the volume-monitor services synchronously.
  GioPtr<GVolumeMonitor> monitor;
  std::vector<Drive> drives;
  std::map<std::string, Handle> handles;
  std::function<void()> changed;

  void add(Handle handle) {
    Drive drive;
    GMount* mount = handle.mount.get();
    if (handle.volume) {
      drive.key = "volume:" + identifier(handle.volume.get());
      const GString name(g_volume_get_name(handle.volume.get()));
      drive.label = name ? name.get() : "Volume";
      if (!mount) {
        handle.mount.reset(g_volume_get_mount(handle.volume.get()));
        mount = handle.mount.get();
      }
    } else {
      const GString name(g_mount_get_name(mount));
      drive.label = name ? name.get() : "Mount";
    }
    if (mount) {
      const GioPtr<GFile> root(g_mount_get_root(mount));
      drive.root = locationFromFile(root.get());
      drive.ejectable = g_mount_can_eject(mount) != FALSE;
      drive.removable = drive.ejectable || g_mount_can_unmount(mount) != FALSE;
      if (!handle.volume) {
        drive.key = "mount:" + drive.root->uri();
      }
    }
    const GioPtr<GDrive> device(handle.volume ? g_volume_get_drive(handle.volume.get()) : nullptr);
    if (drive.root && !drive.root->isNative()) {
      drive.glyph = "server";
    } else if (device && g_drive_is_removable(device.get())) {
      drive.glyph = "device-usb";
    } else {
      drive.glyph = "database";
    }
    handles.insert_or_assign(drive.key, std::move(handle));
    drives.push_back(std::move(drive));
  }

  void refresh() {
    drives.clear();
    handles.clear();
    GList* volumes = g_volume_monitor_get_volumes(monitor.get());
    for (GList* item = volumes; item != nullptr; item = item->next) {
      add({.volume = GioPtr<GVolume>(G_VOLUME(item->data)), .mount = nullptr});
    }
    g_list_free(volumes);
    GList* mounts = g_volume_monitor_get_mounts(monitor.get());
    for (GList* item = mounts; item != nullptr; item = item->next) {
      GioPtr<GMount> mount(G_MOUNT(item->data));
      const GioPtr<GVolume> volume(g_mount_get_volume(mount.get()));
      if (!volume && g_mount_is_shadowed(mount.get()) == FALSE) {
        add({.volume = nullptr, .mount = std::move(mount)});
      }
    }
    g_list_free(mounts);
    if (changed) {
      changed();
    }
  }
};

luciole::DriveMonitor::DriveMonitor() : m_state(std::make_shared<State>()) {
  // The volume monitor used to initialize GIO's process-global mount cache here, before any worker filesystem
  // query; GIO races when concurrent first queries build it. Keep that serialized first query on this thread.
  const GioPtr<GFile> root(g_file_new_for_path("/"));
  const GioPtr<GFileInfo> info(
      g_file_query_filesystem_info(root.get(), G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, nullptr, nullptr)
  );
}

luciole::DriveMonitor::~DriveMonitor() {
  if (m_state->monitor) {
    g_signal_handlers_disconnect_by_data(m_state->monitor.get(), m_state.get());
  }
  m_state->changed = {};
}

void luciole::DriveMonitor::start() {
  if (m_state->monitor) {
    return;
  }
  m_state->monitor.reset(g_volume_monitor_get());
  for (const auto* signal :
       {"volume-added", "volume-removed", "volume-changed", "mount-added", "mount-removed", "mount-changed"}) {
    g_signal_connect_swapped(
        m_state->monitor.get(), signal, G_CALLBACK(+[](gpointer data) { static_cast<State*>(data)->refresh(); }),
        m_state.get()
    );
  }
  m_state->refresh();
}

const std::vector<luciole::Drive>& luciole::DriveMonitor::drives() const { return m_state->drives; }

const luciole::Drive* luciole::DriveMonitor::find(const std::string& key) const {
  for (const auto& drive : m_state->drives) {
    if (drive.key == key) {
      return &drive;
    }
  }
  return nullptr;
}

void luciole::DriveMonitor::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}

void luciole::DriveMonitor::mount(
    const std::string& key, std::function<void(std::optional<Location>, std::string)> done
) {
  const auto* drive = find(key);
  const auto handle = m_state->handles.find(key);
  if (drive == nullptr || handle == m_state->handles.end()) {
    done(std::nullopt, "This drive is no longer available.");
    return;
  }
  if (drive->root) {
    done(drive->root, {});
    return;
  }
  struct Request {
    std::weak_ptr<State> state;
    std::function<void(std::optional<Location>, std::string)> done;
  };
  const GioPtr<GMountOperation> operation(g_mount_operation_new());
  g_volume_mount(
      handle->second.volume.get(), G_MOUNT_MOUNT_NONE, operation.get(), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Request> request(static_cast<Request*>(data));
        GError* error = nullptr;
        const bool mounted = g_volume_mount_finish(G_VOLUME(source), result, &error) != FALSE;
        auto message = errorText(error);
        if (request->state.expired()) {
          return;
        }
        std::optional<Location> root;
        if (mounted) {
          if (const GioPtr<GMount> mount(g_volume_get_mount(G_VOLUME(source))); mount) {
            const GioPtr<GFile> file(g_mount_get_root(mount.get()));
            root = locationFromFile(file.get());
          }
        }
        request->done(root, std::move(message));
      },
      new Request{m_state, std::move(done)}
  );
}

void luciole::DriveMonitor::remove(const std::string& key, std::function<void(std::string)> done) {
  const auto* drive = find(key);
  const auto handle = m_state->handles.find(key);
  if (drive == nullptr || handle == m_state->handles.end() || !handle->second.mount || !drive->removable) {
    done("This drive can no longer be removed.");
    return;
  }
  struct Request {
    std::weak_ptr<State> state;
    std::function<void(std::string)> done;
    bool eject = false;
  };
  const GioPtr<GMountOperation> operation(g_mount_operation_new());
  const auto finished = [](GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<Request> request(static_cast<Request*>(data));
    GError* error = nullptr;
    if (request->eject) {
      (void)g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error);
    } else {
      (void)g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error);
    }
    auto message = errorText(error);
    if (!request->state.expired()) {
      request->done(std::move(message));
    }
  };
  auto* request = new Request{m_state, std::move(done), drive->ejectable};
  if (drive->ejectable) {
    g_mount_eject_with_operation(
        handle->second.mount.get(), G_MOUNT_UNMOUNT_NONE, operation.get(), nullptr, finished, request
    );
  } else {
    g_mount_unmount_with_operation(
        handle->second.mount.get(), G_MOUNT_UNMOUNT_NONE, operation.get(), nullptr, finished, request
    );
  }
}
