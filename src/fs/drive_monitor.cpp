#include "fs/drive_monitor.h"

#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"
#include "fs/location_mount.h"
#include "fs/system_mount_monitor.h"
#include "fs/system_mounts.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <map>
#include <set>
#include <utility>

namespace {
  // Owned references to the GVolume or GMount behind one Drive, and the device holding it.
  struct Handle {
    luciole::GioPtr<GVolume> volume;
    luciole::GioPtr<GMount> mount;
    luciole::GioPtr<GDrive> device;
    std::optional<luciole::MountConfiguration> systemMount = std::nullopt;
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

  std::string processName(int pid) {
    std::ifstream file(std::format("/proc/{}/comm", pid));
    std::string name;
    std::getline(file, name);
    return name.empty() ? std::format("process {}", pid) : name;
  }

  // The first name of a themed icon, without "-symbolic"; empty for other icons.
  std::string themedName(GIcon* icon) {
    if (icon == nullptr || !G_IS_THEMED_ICON(icon)) {
      return {};
    }
    const auto* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    if (names == nullptr || names[0] == nullptr) {
      return {};
    }
    std::string name(names[0]);
    constexpr std::string_view symbolic = "-symbolic";
    if (name.ends_with(symbolic)) {
      name.resize(name.size() - symbolic.size());
    }
    return name;
  }

  bool boundDirectory(GMount* mount) {
    const luciole::GioPtr<GFile> root(g_mount_get_root(mount));
    const luciole::GString path(g_file_get_path(root.get()));
    if (!path) {
      return false;
    }
    // These names also support the project's GLib 2.76 minimum.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GUnixMountEntry* entry = g_unix_mount_at(path.get(), nullptr);
    const char* rootPath = entry ? g_unix_mount_get_root_path(entry) : nullptr;
    const char* options = entry ? g_unix_mount_get_options(entry) : nullptr;
    const bool result = rootPath && luciole::bindMount(rootPath, options ? options : "");
    if (entry) {
      g_unix_mount_free(entry);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    return result;
  }
} // namespace

struct luciole::DriveMonitor::State : std::enable_shared_from_this<State> {
  // Null until start(): GIO starts the volume-monitor services synchronously.
  GioPtr<GVolumeMonitor> monitor;
  SystemMountMonitor systemMounts;
  std::vector<Drive> drives;
  std::map<std::string, Handle> handles;
  std::function<void()> changed;
  std::function<void()> usageChanged;
  std::set<std::string> usagePending;

  // "/" leads the list as File System; GIO's volume monitor never reports it.
  void addRoot() {
    drives.push_back(
        {.key = "root",
         .label = "File System",
         .glyph = "database",
         .root = Location::fromPath("/"),
         .removal = luciole::DriveRemoval::None,
         .usage = std::nullopt,
         .iconName = {}}
    );
  }

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
    if (handle.volume) {
      handle.device.reset(g_volume_get_drive(handle.volume.get()));
    }
    GDrive* device = handle.device.get();
    if (mount) {
      const GioPtr<GFile> root(g_mount_get_root(mount));
      drive.root = locationFromFile(root.get());
      if (device && g_drive_can_stop(device) != FALSE) {
        drive.removal = DriveRemoval::PowerOff;
      } else if (g_mount_can_eject(mount) != FALSE) {
        drive.removal = DriveRemoval::Eject;
      } else if (g_mount_can_unmount(mount) != FALSE) {
        drive.removal = DriveRemoval::Unmount;
      }
      if (!handle.volume) {
        drive.key = "mount:" + drive.root->uri();
      }
    }
    const luciole::GioPtr<GIcon> icon(
        handle.volume ? g_volume_get_symbolic_icon(handle.volume.get())
                      : (mount ? g_mount_get_symbolic_icon(mount) : nullptr)
    );
    const auto iconName = themedName(icon.get());
    const auto kind = deviceGlyph(iconName);
    if (!kind.empty()) {
      drive.glyph = kind;
    } else if (drive.root && !drive.root->isNative()) {
      drive.glyph = "server";
    } else if (device && g_drive_is_removable(device)) {
      drive.glyph = "device-usb";
    } else {
      drive.glyph = "database";
    }
    if (kind.empty()) {
      drive.iconName = iconName;
    }
    handles.insert_or_assign(drive.key, std::move(handle));
    drives.push_back(std::move(drive));
  }

  void refresh() {
    auto previous = std::move(drives);
    drives.clear();
    handles.clear();
    addRoot();
    std::map<std::string, Handle> systemHandles;
    // A mount at a configured system mount path belongs to its system row, keyed by the target URI.
    const auto systemMountUri = [&](GMount* mount) -> std::optional<std::string> {
      const GioPtr<GFile> root(g_mount_get_root(mount));
      const auto location = locationFromFile(root.get());
      const bool owned = std::ranges::any_of(systemMounts.mounts(), [&](const SystemMount& candidate) {
        return Location::fromPath(candidate.configuration.path) == location;
      });
      return owned ? std::optional(location.uri()) : std::nullopt;
    };
    GList* volumes = g_volume_monitor_get_volumes(monitor.get());
    for (GList* item = volumes; item != nullptr; item = item->next) {
      GioPtr<GVolume> volume(G_VOLUME(item->data));
      GioPtr<GMount> mount(g_volume_get_mount(volume.get()));
      if (mount) {
        if (auto uri = systemMountUri(mount.get())) {
          systemHandles.emplace(
              std::move(*uri), Handle{.volume = std::move(volume), .mount = std::move(mount), .device = nullptr}
          );
        } else if (!boundDirectory(mount.get())) {
          add({.volume = std::move(volume), .mount = std::move(mount), .device = nullptr});
        }
        continue;
      }
      const GString kind(g_volume_get_identifier(volume.get(), G_VOLUME_IDENTIFIER_KIND_CLASS));
      const GString device(g_volume_get_identifier(volume.get(), G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
      const GioPtr<GFile> activation(g_volume_get_activation_root(volume.get()));
      // Unmounted GVfs fstab network volumes expose no path or device identifier, only their display label.
      const bool matchesSource = device && std::ranges::any_of(systemMounts.mounts(), [&](const auto& candidate) {
                                   return sameMountSource(candidate.configuration.source, device.get());
                                 });
      const GString name(g_volume_get_name(volume.get()));
      const bool matchesUnmountedNetwork = !activation
          && kind
          && std::string_view(kind.get()) == "network"
          && name
          && std::ranges::any_of(systemMounts.mounts(), [&](const auto& candidate) {
               return candidate.label == name.get();
             });
      if (matchesSource || matchesUnmountedNetwork) {
        continue;
      }
      add({.volume = std::move(volume), .mount = nullptr, .device = nullptr});
    }
    g_list_free(volumes);
    GList* mounts = g_volume_monitor_get_mounts(monitor.get());
    for (GList* item = mounts; item != nullptr; item = item->next) {
      GioPtr<GMount> mount(G_MOUNT(item->data));
      const GioPtr<GVolume> volume(g_mount_get_volume(mount.get()));
      if (!volume && g_mount_is_shadowed(mount.get()) == FALSE) {
        if (auto uri = systemMountUri(mount.get())) {
          systemHandles.emplace(
              std::move(*uri), Handle{.volume = nullptr, .mount = std::move(mount), .device = nullptr}
          );
          continue;
        }
        if (!boundDirectory(mount.get())) {
          add({.volume = nullptr, .mount = std::move(mount), .device = nullptr});
        }
      }
    }
    g_list_free(mounts);
    for (const auto& mount : systemMounts.mounts()) {
      if (!mount.visible) {
        continue;
      }
      const auto target = Location::fromPath(mount.configuration.path);
      auto key = "system-mount:" + target.uri();
      Handle handle{.volume = nullptr, .mount = nullptr, .device = nullptr, .systemMount = mount.configuration};
      if (const auto found = systemHandles.find(target.uri()); found != systemHandles.end()) {
        handle.volume = std::move(found->second.volume);
        handle.mount = std::move(found->second.mount);
      }
      const bool removable = !mount.configuration.automount
          && mount.state == SystemMountState::Mounted
          && handle.mount
          && g_mount_can_unmount(handle.mount.get());
      handles.emplace(key, std::move(handle));
      drives.push_back(
          {.key = std::move(key),
           .label = mount.label,
           .glyph = "server",
           .root = mount.state == SystemMountState::Mounted ? std::optional(target) : std::nullopt,
           .removal = removable ? DriveRemoval::Unmount : DriveRemoval::None,
           .usage = std::nullopt,
           .activation = mount.configuration.automount ? std::optional(target) : std::nullopt,
           .iconName = {}}
      );
    }
    // Mounted drives first, keeping GIO's order within each group; the root entry stays at the top.
    std::ranges::stable_partition(drives, [](const Drive& drive) { return drive.root.has_value(); });
    // Keep the last known usage until the next query answers, so bars do not flicker on rebuilds.
    for (auto& drive : drives) {
      const auto old = std::ranges::find(previous, drive.key, &Drive::key);
      if (drive.root && old != previous.end() && old->root == drive.root) {
        drive.usage = old->usage;
      }
    }
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
  m_state->addRoot();
}

luciole::DriveMonitor::~DriveMonitor() {
  if (m_state->monitor) {
    g_signal_handlers_disconnect_by_data(m_state->monitor.get(), m_state.get());
  }
  m_state->changed = {};
  m_state->usageChanged = {};
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
  m_state->systemMounts.start([weak = std::weak_ptr(m_state)] {
    if (const auto state = weak.lock()) {
      state->refresh();
    }
  });
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

void luciole::DriveMonitor::setUsageChangedCallback(std::function<void()> callback) {
  m_state->usageChanged = std::move(callback);
}

void luciole::DriveMonitor::refreshUsage() {
  struct Request {
    std::weak_ptr<State> state;
    std::string key;
    Location root;
  };
  const auto finished = [](GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<Request> request(static_cast<Request*>(data));
    GError* error = nullptr;
    const GioPtr<GFileInfo> info(g_file_query_filesystem_info_finish(G_FILE(source), result, &error));
    (void)errorText(error);
    const auto state = request->state.lock();
    if (!state) {
      return;
    }
    state->usagePending.erase(request->key);
    std::optional<DriveUsage> usage;
    if (info) {
      const auto size = infoUint64(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
      auto used = infoUint64(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_USED);
      if (const auto free = infoUint64(info.get(), G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
          !used && size && free && *free <= *size) {
        used = *size - *free;
      }
      if (size && *size > 0 && used) {
        usage = DriveUsage{.used = std::min(*used, *size), .size = *size};
      }
    }
    const auto drive = std::ranges::find_if(state->drives, [&](const Drive& candidate) {
      return candidate.key == request->key && candidate.root == request->root;
    });
    if (drive == state->drives.end() || drive->usage == usage) {
      return;
    }
    drive->usage = usage;
    if (state->usageChanged) {
      state->usageChanged();
    }
  };
  for (const auto& drive : m_state->drives) {
    if (drive.activation || !drive.root || !m_state->usagePending.insert(drive.key).second) {
      continue;
    }
    g_file_query_filesystem_info_async(
        gioFile(*drive.root).get(),
        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_USED "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
        G_PRIORITY_DEFAULT, nullptr, finished, new Request{m_state, drive.key, *drive.root}
    );
  }
}

void luciole::DriveMonitor::mount(
    const std::string& key, LocationMount& request, std::function<void(std::expected<Location, FileError>)> done
) {
  request.cancel();
  const auto* drive = find(key);
  if (drive != nullptr && drive->activation) {
    done(*drive->activation);
    return;
  }
  if (drive != nullptr && drive->root) {
    done(*drive->root);
    return;
  }
  const auto handle = m_state->handles.find(key);
  if (drive == nullptr || handle == m_state->handles.end() || (!handle->second.systemMount && !handle->second.volume)) {
    done(std::unexpected(FileError{FileErrorKind::Unavailable, "This drive is no longer available."}));
    return;
  }
  if (handle->second.systemMount) {
    request.startFstabMount(Location::fromPath(handle->second.systemMount->path), std::move(done));
  } else {
    request.startVolume(handle->second.volume.get(), std::move(done));
  }
}

void luciole::DriveMonitor::remove(const std::string& key, std::function<void(DriveRemoveResult)> done) {
  const auto* drive = find(key);
  const auto handle = m_state->handles.find(key);
  if (drive == nullptr
      || handle == m_state->handles.end()
      || !handle->second.mount
      || drive->removal == DriveRemoval::None
      || (drive->removal == DriveRemoval::PowerOff && !handle->second.device)) {
    done({.error = "This drive can no longer be removed.", .busy = false, .blockers = {}});
    return;
  }
  struct Request {
    std::weak_ptr<State> state;
    std::function<void(DriveRemoveResult)> done;
    DriveRemoval removal = DriveRemoval::None;
    GioPtr<GMountOperation> operation;
    DriveRemoveResult result;
  };
  auto* request = new Request{
      .state = m_state,
      .done = std::move(done),
      .removal = drive->removal,
      .operation = GioPtr<GMountOperation>(g_mount_operation_new()),
      .result = {}
  };
  // Files are open on the drive: record who holds them and give up rather than wait or force.
  const auto busy = +[](GMountOperation* operation, const char*, GArray* processes, const char**, gpointer data) {
    auto& result = static_cast<Request*>(data)->result;
    result.busy = true;
    result.blockers.clear();
    for (guint i = 0; i < processes->len; ++i) {
      const int pid = g_array_index(processes, GPid, i);
      result.blockers.push_back({.pid = pid, .name = processName(pid)});
    }
    g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
  };
  g_signal_connect(request->operation.get(), "show-processes", G_CALLBACK(busy), request);
  const auto finished = [](GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<Request> request(static_cast<Request*>(data));
    g_signal_handlers_disconnect_by_data(request->operation.get(), request.get());
    GError* error = nullptr;
    switch (request->removal) {
    case DriveRemoval::PowerOff:
      (void)g_drive_stop_finish(G_DRIVE(source), result, &error);
      break;
    case DriveRemoval::Eject:
      (void)g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error);
      break;
    case DriveRemoval::Unmount:
    case DriveRemoval::None:
      (void)g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error);
      break;
    }
    if (error != nullptr && error->domain == G_IO_ERROR && error->code == G_IO_ERROR_BUSY) {
      request->result.busy = true;
    }
    auto message = errorText(error);
    if (!message.empty() && !request->result.busy) {
      request->result.error = std::move(message);
    } else if (request->result.busy) {
      request->result.error = "The drive is in use.";
    }
    if (!request->state.expired()) {
      request->done(std::move(request->result));
    }
  };
  auto* operation = request->operation.get();
  switch (drive->removal) {
  case DriveRemoval::PowerOff:
    g_drive_stop(handle->second.device.get(), G_MOUNT_UNMOUNT_NONE, operation, nullptr, finished, request);
    break;
  case DriveRemoval::Eject:
    g_mount_eject_with_operation(
        handle->second.mount.get(), G_MOUNT_UNMOUNT_NONE, operation, nullptr, finished, request
    );
    break;
  case DriveRemoval::Unmount:
  case DriveRemoval::None:
    g_mount_unmount_with_operation(
        handle->second.mount.get(), G_MOUNT_UNMOUNT_NONE, operation, nullptr, finished, request
    );
    break;
  }
}

std::string_view luciole::deviceGlyph(std::string_view iconName) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kDevices{{
      {"phone", "device-mobile"},
      {"multimedia-player", "device-speaker"},
      // GVfs AFC reports iPads as computer-apple-ipad.
      {"computer-apple-ipad", "device-ipad"},
      {"camera-photo", "camera"},
  }};
  while (!iconName.empty()) {
    if (const auto* found =
            std::ranges::find(kDevices, iconName, &std::pair<std::string_view, std::string_view>::first);
        found != kDevices.end()) {
      return found->second;
    }
    const auto dash = iconName.rfind('-');
    iconName = dash == std::string_view::npos ? std::string_view{} : iconName.substr(0, dash);
  }
  return {};
}
