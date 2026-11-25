#include "fs/system_mounts.h"

#include "fs/file_error.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "fs/systemd_mounts.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <gio/gunixmounts.h>
#include <linux/magic.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>

namespace {
  struct Configuration {
    std::mutex mutex;
    std::timed_mutex discovery;
    std::vector<luciole::MountConfiguration> native;
    bool known = false;
    // The lazy query runs once per process when SystemMountMonitor has not published yet; a cancelled attempt retries.
    bool queried = false;
    std::map<std::string, std::shared_ptr<std::timed_mutex>> slots;
  };
  Configuration& configurationCache() {
    // Filesystem workers may finish after the last directory owner is destroyed.
    static auto* value = new Configuration;
    return *value;
  }

  std::unique_lock<std::timed_mutex> lockTimed(std::timed_mutex& mutex, const luciole::CancelToken& token) {
    std::unique_lock lock(mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(50))) {
      if (token.cancelled()) {
        luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
      }
    }
    if (token.cancelled()) {
      luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
    }
    return lock;
  }

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

  std::string_view normalizeMountPath(std::string_view path) {
    while (path.size() > 1 && path.ends_with('/')) {
      path.remove_suffix(1);
    }
    return path;
  }

  std::string_view normalizeMountSource(std::string_view source) {
    const auto colon = source.find(":/");
    const std::size_t minLength = colon != std::string_view::npos ? colon + 2 : 1;
    while (source.size() > minLength && source.ends_with('/')) {
      source.remove_suffix(1);
    }
    return source;
  }

  bool inside(std::string_view path, std::string_view parent) {
    path = normalizeMountPath(path);
    parent = normalizeMountPath(parent);
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
  }

  bool nfs(std::string_view type) { return type == "nfs" || type == "nfs4"; }
  bool smb(std::string_view type) { return type == "cifs" || type == "smb3"; }

  // A configured type and the kernel's name for the mounted filesystem; each protocol has two names.
  bool sameMountType(std::string_view configured, std::string_view mounted) {
    return configured == mounted || (nfs(configured) && nfs(mounted)) || (smb(configured) && smb(mounted));
  }

  // The opened mount path must be on the share itself, not on the directory beneath the mount point.
  bool shareFilesystem(std::string_view type, const struct statfs& filesystem) {
    if (nfs(type)) {
      return filesystem.f_type == NFS_SUPER_MAGIC;
    }
    if (smb(type)) {
      return filesystem.f_type == CIFS_SUPER_MAGIC || filesystem.f_type == SMB2_SUPER_MAGIC;
    }
    return true;
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
    auto path = normalizeMountPath(config.path);
    if (path.starts_with("/run/")) {
      path.remove_prefix(4);
    }
    if (!path.starts_with("/media/")) {
      return false;
    }
    path.remove_prefix(7);
    return !path.contains('/') || inside(path, user);
  }

  std::optional<luciole::MountConfiguration>
  automountForPath(std::string_view path, std::span<const luciole::MountConfiguration> configuration) {
    std::optional<luciole::MountConfiguration> result;
    const auto normalized = normalizeMountPath(path);
    for (const auto& config : configuration) {
      const auto configPath = normalizeMountPath(config.path);
      if (config.automount
          && (normalized == configPath || inside(normalized, configPath))
          && (!result || configPath.size() > normalizeMountPath(result->path).size())) {
        result = config;
      }
    }
    return result;
  }

  std::optional<std::string> readLinkTarget(const std::string& path) {
    std::string target(256, '\0');
    while (true) {
      const ssize_t length = ::readlink(path.c_str(), target.data(), target.size());
      if (length < 0) {
        return std::nullopt;
      }
      if (static_cast<std::size_t>(length) < target.size()) {
        target.resize(static_cast<std::size_t>(length));
        return target;
      }
      if (target.size() >= 65536) {
        return std::nullopt;
      }
      target.resize(target.size() * 2);
    }
  }

  struct ResolvedAutomount {
    luciole::MountConfiguration config;
    bool crossesMount = false;
  };

  std::optional<ResolvedAutomount> resolveAutomountPath(
      std::string_view input, std::span<const luciole::MountConfiguration> configuration, bool followFinal
  ) {
    if (input.empty()
        || input.front() != '/'
        || !std::ranges::any_of(configuration, &luciole::MountConfiguration::automount)) {
      return std::nullopt;
    }
    std::vector<std::string> pending;
    const auto pushComponents = [&](std::string_view text) {
      while (!text.empty()) {
        while (!text.empty() && text.back() == '/') {
          text.remove_suffix(1);
        }
        if (text.empty()) {
          break;
        }
        const auto slash = text.rfind('/');
        const auto component = slash == std::string_view::npos ? text : text.substr(slash + 1);
        pending.emplace_back(component);
        if (slash == std::string_view::npos) {
          break;
        }
        text.remove_suffix(text.size() - slash);
      }
    };
    pushComponents(input);
    bool trailingSlash = input.size() > 1 && input.ends_with('/');
    std::string resolved = "/";
    std::optional<luciole::MountConfiguration> matched;
    bool crossesMount = false;
    int linksFollowed = 0;
    constexpr int kMaxSymlinks = 40;
    while (!pending.empty()) {
      auto component = std::move(pending.back());
      pending.pop_back();
      const bool isFinal = pending.empty();
      if (component == ".") {
        if (matched) {
          crossesMount = true;
        }
        continue;
      }
      if (component == "..") {
        if (matched) {
          crossesMount = true;
        }
        if (resolved.size() > 1) {
          const auto slash = resolved.rfind('/');
          resolved.resize(slash == 0 ? 1 : slash);
        }
        if (!matched) {
          matched = automountForPath(resolved, configuration);
        }
        continue;
      }
      std::string candidate = resolved;
      if (candidate.size() > 1) {
        candidate.push_back('/');
      }
      candidate += component;
      if (!matched && (!isFinal || followFinal)) {
        if (auto target = readLinkTarget(candidate)) {
          if (++linksFollowed > kMaxSymlinks) {
            return std::nullopt;
          }
          if (isFinal) {
            trailingSlash = target->size() > 1 && target->ends_with('/');
          }
          if (!target->empty() && target->front() == '/') {
            resolved = "/";
          }
          pushComponents(*target);
          continue;
        }
      }
      if (matched) {
        crossesMount = true;
      }
      resolved = std::move(candidate);
      if (auto hit = automountForPath(resolved, configuration)) {
        matched = std::move(hit);
      }
    }
    if (!matched) {
      return std::nullopt;
    }
    const bool crosses = crossesMount || trailingSlash || inside(resolved, matched->path);
    return ResolvedAutomount{.config = std::move(*matched), .crossesMount = crosses};
  }

  std::shared_ptr<std::timed_mutex> mountSlotMutex(std::string_view path) {
    auto& cache = configurationCache();
    const std::scoped_lock lock(cache.mutex);
    auto& slot = cache.slots[std::string(normalizeMountPath(path))];
    if (!slot) {
      slot = std::make_shared<std::timed_mutex>();
    }
    return slot;
  }

  void discoverSystemMountsOnce(const luciole::CancelToken& token) {
    auto& cache = configurationCache();
    {
      const std::scoped_lock lock(cache.mutex);
      if (cache.known || cache.queried) {
        return;
      }
    }
    const auto discoveryLock = lockTimed(cache.discovery, token);
    {
      const std::scoped_lock lock(cache.mutex);
      if (cache.known || cache.queried) {
        return;
      }
    }
    const luciole::GioPtr<GDBusConnection> bus(
        g_bus_get_sync(G_BUS_TYPE_SYSTEM, luciole::gioCancellable(token), nullptr)
    );
    auto configuration = luciole::readSystemdMounts(bus.get(), token);
    if (token.cancelled()) {
      luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
    }
    const std::scoped_lock lock(cache.mutex);
    if (!cache.known) {
      if (configuration) {
        cache.native = std::move(*configuration);
        cache.known = true;
      }
      cache.queried = true;
    }
  }
  std::vector<luciole::MountConfiguration>
  automountsInside(const luciole::Location& location, std::span<const luciole::MountConfiguration> configuration) {
    const auto path = location.nativePath();
    if (!path
        || readLinkTarget(*path).has_value()
        || !std::ranges::any_of(configuration, &luciole::MountConfiguration::automount)) {
      return {};
    }
    std::vector<luciole::MountConfiguration> result;
    std::set<std::string> seen;
    const auto collect = [&](std::string_view dir) {
      for (const auto& config : configuration) {
        if (config.automount && inside(config.path, dir) && seen.emplace(normalizeMountPath(config.path)).second) {
          result.push_back(config);
        }
      }
    };
    collect(*path);
    const std::unique_ptr<char, decltype(&std::free)> canonical(::realpath(path->c_str(), nullptr), &std::free);
    if (canonical && std::string_view(canonical.get()) != *path) {
      collect(canonical.get());
    }
    return result;
  }

  luciole::Fd openConfiguredAutomount(const luciole::MountConfiguration& config, const luciole::CancelToken& token) {
    const auto slotMutex = mountSlotMutex(config.path);
    const auto slot = lockTimed(*slotMutex, token);
    if (luciole::mountState(config, luciole::readMountedFilesystems()) == luciole::SystemMountState::Unavailable) {
      luciole::throwFileError(
          luciole::FileErrorKind::NotMounted, std::format("The system automount at {} is unavailable.", config.path)
      );
    }
    auto directory = luciole::checkedFd(::open((config.path + "/.").c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    struct statfs filesystem{};
    if (::fstatfs(directory.get(), &filesystem) != 0) {
      luciole::throwErrno(errno);
    }
    if (luciole::mountState(config, luciole::readMountedFilesystems()) != luciole::SystemMountState::Mounted
        || !shareFilesystem(config.type, filesystem)) {
      luciole::throwFileError(
          luciole::FileErrorKind::NotMounted, std::format("The expected share is not mounted at {}.", config.path)
      );
    }
    if (token.cancelled()) {
      luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Cancelled.");
    }
    return directory;
  }
} // namespace

bool luciole::networkMountType(std::string_view type) {
  return type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3" || type == "smbfs" || type == "ncpfs";
}

bool luciole::sameMountSource(std::string_view left, std::string_view right) {
  return normalizeMountSource(left) == normalizeMountSource(right);
}

bool luciole::bindMount(std::string_view root, std::string_view options) {
  return normalizeMountPath(root) != "/" && option(options, "subvol") != normalizeMountPath(root);
}

std::vector<luciole::MountConfiguration> luciole::readFstabMounts() {
  std::vector<MountConfiguration> result;
  GList* points = g_unix_mount_points_get(nullptr);
  for (GList* item = points; item; item = item->next) {
    auto* point = static_cast<GUnixMountPoint*>(item->data);
    const auto* type = g_unix_mount_point_get_fs_type(point);
    const auto* path = g_unix_mount_point_get_mount_path(point);
    const auto* source = g_unix_mount_point_get_device_path(point);
    // A hand-edited entry can lack a field or name a relative target; skip it instead of failing every refresh.
    if (type && path && source && g_path_is_absolute(path) && networkMountType(type)) {
      const auto* options = g_unix_mount_point_get_options(point);
      result.push_back(
          {.path = std::string(normalizeMountPath(path)),
           .source = source,
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
    const auto* path = g_unix_mount_get_mount_path(mount);
    const auto* source = g_unix_mount_get_device_path(mount);
    const auto* type = g_unix_mount_get_fs_type(mount);
    if (path && source && type) {
      result.push_back({.path = std::string(normalizeMountPath(path)), .source = source, .type = type});
    }
  }
  g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_free));
  G_GNUC_END_IGNORE_DEPRECATIONS
  return result;
}

luciole::SystemMountState
luciole::mountState(const MountConfiguration& configuration, std::span<const MountedFilesystem> mounted) {
  const auto targetPath = normalizeMountPath(configuration.path);
  auto state = SystemMountState::Unavailable;
  for (const auto& mount : mounted) {
    if (normalizeMountPath(mount.path) != targetPath) {
      continue;
    }
    if (sameMountType(configuration.type, mount.type) && sameMountSource(mount.source, configuration.source)) {
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
  const auto path = location.nativePath();
  return path ? automountForPath(*path, configuration) : std::nullopt;
}

std::optional<luciole::MountConfiguration> luciole::resolveAutomountFor(
    const Location& location, std::span<const MountConfiguration> configuration, bool followFinal
) {
  if (auto direct = automountFor(location, configuration)) {
    return direct;
  }
  const auto path = location.nativePath();
  if (!path) {
    return std::nullopt;
  }
  auto resolved = resolveAutomountPath(*path, configuration, followFinal);
  return resolved ? std::optional(std::move(resolved->config)) : std::nullopt;
}

bool luciole::symlinkEntersUnmountedAutomount(
    std::string_view path, std::span<const MountConfiguration> configuration, std::span<const MountedFilesystem> mounted
) {
  const auto resolved = resolveAutomountPath(path, configuration, true);
  return resolved && resolved->crossesMount && mountState(resolved->config, mounted) != SystemMountState::Mounted;
}

std::optional<luciole::MountConfiguration>
luciole::automountInside(const Location& location, std::span<const MountConfiguration> configuration) {
  auto matches = automountsInside(location, configuration);
  return matches.empty() ? std::nullopt : std::optional(std::move(matches.front()));
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
    const auto targetPath = normalizeMountPath(config.path);
    if (const auto found = std::ranges::find_if(
            result, [&](const MountConfiguration& entry) { return normalizeMountPath(entry.path) == targetPath; }
        );
        found != result.end()) {
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

luciole::Fd luciole::openSystemMount(const Location& location, const CancelToken& token, bool followFinal) {
  discoverSystemMountsOnce(token);
  const auto config = resolveAutomountFor(location, configuredSystemMounts(), followFinal);
  if (!config) {
    return {};
  }
  return openConfiguredAutomount(*config, token);
}

std::vector<luciole::Fd>
luciole::openSystemMounts(std::span<const Location> locations, const CancelToken& token, bool followFinal) {
  discoverSystemMountsOnce(token);
  const auto configuration = configuredSystemMounts();
  std::set<std::string> seen;
  std::vector<Fd> mounts;
  for (const auto& location : locations) {
    if (const auto config = resolveAutomountFor(location, configuration, followFinal)) {
      if (seen.emplace(normalizeMountPath(config->path)).second) {
        mounts.push_back(openConfiguredAutomount(*config, token));
      }
    }
  }
  return mounts;
}

std::vector<luciole::Fd>
luciole::openSystemMountsInside(std::span<const Location> locations, const CancelToken& token) {
  discoverSystemMountsOnce(token);
  const auto configuration = configuredSystemMounts();
  if (!std::ranges::any_of(configuration, &MountConfiguration::automount)) {
    return {};
  }
  std::set<std::string> seen;
  std::vector<Fd> mounts;
  for (const auto& location : locations) {
    for (const auto& config : automountsInside(location, configuration)) {
      struct stat entry{};
      if (::lstat(config.path.c_str(), &entry) == 0 && seen.emplace(normalizeMountPath(config.path)).second) {
        mounts.push_back(openConfiguredAutomount(config, token));
      }
    }
  }
  return mounts;
}
