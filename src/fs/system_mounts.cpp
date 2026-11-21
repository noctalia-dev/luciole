#include "fs/system_mounts.h"

#include "fs/file_error.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "fs/systemd_mounts.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <format>
#include <gio/gunixmounts.h>
#include <linux/magic.h>
#include <mutex>
#include <semaphore>
#include <sys/vfs.h>

namespace {
  struct Configuration {
    std::mutex mutex;
    std::vector<luciole::MountConfiguration> native;
    bool known = false;
    std::counting_semaphore<4> slots{4};
  };
  Configuration& configurationCache() {
    // Filesystem workers may finish after the last directory owner is destroyed.
    static auto* value = new Configuration;
    return *value;
  }

  class MountSlot {
  public:
    explicit MountSlot(const luciole::CancelToken& token) {
      while (!configurationCache().slots.try_acquire_for(std::chrono::milliseconds(50))) {
        if (token.cancelled()) {
          luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
        }
      }
    }
    ~MountSlot() { configurationCache().slots.release(); }
    MountSlot(const MountSlot&) = delete;
    MountSlot& operator=(const MountSlot&) = delete;
  };

  std::optional<std::string_view> option(std::string_view options, std::string_view name) {
    while (!options.empty()) {
      const auto end = options.find(',');
      const auto item = options.substr(0, end);
      const auto equal = item.find('=');
      if (item.substr(0, equal) == name) {
        return equal == std::string_view::npos ? std::string_view{} : item.substr(equal + 1);
      }
      if (end == std::string_view::npos) {
        break;
      }
      options.remove_prefix(end + 1);
    }
    return std::nullopt;
  }

  bool inside(std::string_view path, std::string_view parent) {
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
  }

  bool visible(const luciole::MountConfiguration& config, std::string_view home, std::string_view user) {
    if (option(config.options, "x-gvfs-show")) {
      return true;
    }
    if (option(config.options, "x-gvfs-hide")) {
      return false;
    }
    if (inside(config.path, home)) {
      return true;
    }
    auto path = std::string_view(config.path);
    if (path.starts_with("/run/")) {
      path.remove_prefix(4);
    }
    if (!path.starts_with("/media/")) {
      return false;
    }
    path.remove_prefix(7);
    return !path.contains('/') || inside(path, user);
  }
} // namespace

bool luciole::networkMountType(std::string_view type) {
  return type == "nfs" || type == "nfs4" || type == "cifs" || type == "smbfs" || type == "ncpfs";
}

std::vector<luciole::MountConfiguration> luciole::readFstabMounts() {
  std::vector<MountConfiguration> result;
  GList* points = g_unix_mount_points_get(nullptr);
  for (GList* item = points; item; item = item->next) {
    auto* point = static_cast<GUnixMountPoint*>(item->data);
    const auto* type = g_unix_mount_point_get_fs_type(point);
    if (networkMountType(type)) {
      const auto* options = g_unix_mount_point_get_options(point);
      result.push_back(
          {.path = g_unix_mount_point_get_mount_path(point),
           .source = g_unix_mount_point_get_device_path(point),
           .type = type,
           .options = options ? options : "",
           .automount = options && option(options, "x-systemd.automount").has_value()}
      );
    }
  }
  g_list_free_full(points, reinterpret_cast<GDestroyNotify>(g_unix_mount_point_free));
  return result;
}

std::vector<luciole::MountedFilesystem> luciole::readMountedFilesystems() {
  std::vector<MountedFilesystem> result;
  // These names also support the project's GLib 2.76 minimum.
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GList* mounts = g_unix_mounts_get(nullptr);
  for (GList* item = mounts; item; item = item->next) {
    auto* mount = static_cast<GUnixMountEntry*>(item->data);
    result.push_back(
        {.path = g_unix_mount_get_mount_path(mount),
         .source = g_unix_mount_get_device_path(mount),
         .type = g_unix_mount_get_fs_type(mount)}
    );
  }
  g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_free));
  G_GNUC_END_IGNORE_DEPRECATIONS
  return result;
}

luciole::SystemMountState
luciole::mountState(const MountConfiguration& configuration, std::span<const MountedFilesystem> mounted) {
  auto state = SystemMountState::Unavailable;
  for (const auto& mount : mounted) {
    if (mount.path != configuration.path) {
      continue;
    }
    const bool nfs =
        (configuration.type == "nfs" || configuration.type == "nfs4") && (mount.type == "nfs" || mount.type == "nfs4");
    if ((nfs || mount.type == configuration.type) && mount.source == configuration.source) {
      state = SystemMountState::Mounted;
    } else if (configuration.automount && mount.type == "autofs" && mount.source == "systemd-1") {
      state = SystemMountState::Waiting;
    } else {
      state = SystemMountState::Unavailable;
    }
  }
  return state;
}

std::vector<luciole::SystemMount> luciole::systemMounts(
    std::span<const MountConfiguration> configuration, std::span<const MountedFilesystem> mounted,
    std::string_view home, std::string_view user
) {
  std::vector<SystemMount> result;
  for (const auto& config : configuration) {
    const auto target = Location::fromPath(config.path);
    const auto name = option(config.options, "x-gvfs-name");
    const GString label(
        name && !name->empty() ? g_uri_unescape_segment(name->data(), name->data() + name->size(), nullptr) : nullptr
    );
    result.push_back(
        {.configuration = config,
         .state = mountState(config, mounted),
         .label = label && g_utf8_validate(label.get(), -1, nullptr) ? std::string(label.get())
                                                                     : target.displayName().value_or(config.path),
         .visible = visible(config, home, user)}
    );
  }
  return result;
}

std::optional<luciole::MountConfiguration>
luciole::automountFor(const Location& location, std::span<const MountConfiguration> configuration) {
  std::optional<MountConfiguration> result;
  const auto path = location.nativePath();
  if (!path) {
    return result;
  }
  for (const auto& config : configuration) {
    if (config.automount
        && (*path == config.path || inside(*path, config.path))
        && (!result || config.path.size() > result->path.size())) {
      result = config;
    }
  }
  return result;
}

void luciole::publishNativeMounts(std::vector<MountConfiguration> configuration) {
  const std::scoped_lock lock(configurationCache().mutex);
  configurationCache().native = std::move(configuration);
  configurationCache().known = true;
}

std::vector<luciole::MountConfiguration> luciole::configuredSystemMounts() {
  return mergeMountConfiguration(readFstabMounts(), knownNativeMounts());
}

std::vector<luciole::MountConfiguration> luciole::knownNativeMounts() {
  const std::scoped_lock lock(configurationCache().mutex);
  return configurationCache().native;
}

std::vector<luciole::MountConfiguration>
luciole::mergeMountConfiguration(std::vector<MountConfiguration> result, std::span<const MountConfiguration> native) {
  for (const auto& config : native) {
    if (const auto found = std::ranges::find(result, config.path, &MountConfiguration::path); found != result.end()) {
      // systemd's loaded unit is authoritative; fstab retains the desktop name and visibility.
      const auto options = found->options;
      *found = config;
      found->options = options + ',' + config.options;
    } else {
      result.push_back(config);
    }
  }
  return result;
}

bool luciole::prepareSystemMount(const Location& location, const CancelToken& token) {
  bool known = false;
  {
    const std::scoped_lock lock(configurationCache().mutex);
    known = configurationCache().known;
  }
  if (!known) {
    const GioPtr<GDBusConnection> bus(g_bus_get_sync(G_BUS_TYPE_SYSTEM, gioCancellable(token), nullptr));
    if (auto configuration = readSystemdMounts(bus.get(), token)) {
      publishNativeMounts(std::move(*configuration));
    }
  }
  const auto config = automountFor(location, configuredSystemMounts());
  if (!config) {
    return false;
  }
  const MountSlot slot(token);
  if (token.cancelled()) {
    throwFileError(FileErrorKind::Cancelled, "Cancelled.");
  }
  if (mountState(*config, readMountedFilesystems()) == SystemMountState::Unavailable) {
    throwFileError(FileErrorKind::NotMounted, std::format("The system automount at {} is unavailable.", config->path));
  }
  const auto directory = checkedFd(::open((config->path + "/.").c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
  struct statfs filesystem{};
  if (::fstatfs(directory.get(), &filesystem) != 0) {
    throwErrno(errno);
  }
  if (mountState(*config, readMountedFilesystems()) != SystemMountState::Mounted
      || ((config->type == "nfs" || config->type == "nfs4") && filesystem.f_type != NFS_SUPER_MAGIC)) {
    throwFileError(FileErrorKind::NotMounted, std::format("The expected share is not mounted at {}.", config->path));
  }
  if (token.cancelled()) {
    throwFileError(FileErrorKind::Cancelled, "Cancelled.");
  }
  return true;
}
