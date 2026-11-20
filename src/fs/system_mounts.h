#pragma once

#include "fs/cancel_token.h"
#include "fs/location.h"
#include "fs/posix_fd.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  enum class SystemMountState : std::uint8_t { Unavailable, Waiting, Mounted };

  struct NativeMountFiles {
    std::string mount;
    std::string automount;
    bool operator==(const NativeMountFiles&) const = default;
  };

  struct MountConfiguration {
    std::string path;
    std::string source;
    std::string type;
    std::string options;
    bool automount = false;
    // Persistent unit files identify a known native target after systemd unloads its inactive units.
    std::optional<NativeMountFiles> unitFiles = std::nullopt;
    bool operator==(const MountConfiguration&) const = default;
  };

  struct MountedFilesystem {
    std::string path;
    std::string source;
    std::string type;
  };

  struct SystemMount {
    MountConfiguration configuration;
    SystemMountState state = SystemMountState::Unavailable;
    std::string label;
    bool visible = false;
    bool operator==(const SystemMount&) const = default;
  };

  bool networkMountType(std::string_view type);
  std::vector<MountConfiguration> readFstabMounts();
  std::vector<MountConfiguration>
  mergeMountConfiguration(std::vector<MountConfiguration> result, std::span<const MountConfiguration> native);
  std::vector<MountedFilesystem> readMountedFilesystems();
  std::vector<SystemMount> systemMounts(
      std::span<const MountConfiguration> configuration, std::span<const MountedFilesystem> mounted,
      std::string_view home, std::string_view user
  );
  std::optional<MountConfiguration>
  automountFor(const Location& location, std::span<const MountConfiguration> configuration);
  SystemMountState mountState(const MountConfiguration& configuration, std::span<const MountedFilesystem> mounted);

  // Native systemd configuration is published by SystemMountMonitor. Readers never access a target directory.
  void publishNativeMounts(std::vector<MountConfiguration> configuration);
  std::vector<MountConfiguration> knownNativeMounts();
  // A known snapshot can authoritatively exclude a target even if a helper cached an older configuration.
  std::vector<MountConfiguration> configuredSystemMounts(bool* nativeKnown = nullptr);

  // Worker-only. Discover loaded units when the application has not published a snapshot yet.
  std::vector<MountConfiguration> discoverSystemMounts(const CancelToken& token);

  // Worker-only. Access triggers the system automount; verify the opened filesystem before browsing it.
  // Cancellation cannot interrupt an in-flight kernel mount, but prevents publishing its result.
  // Retain the verified root while a file operation is using the share. An empty descriptor means no automount.
  Fd openSystemMount(
      const Location& location, const CancelToken& token, std::optional<MountConfiguration> expected = std::nullopt
  );
} // namespace luciole
