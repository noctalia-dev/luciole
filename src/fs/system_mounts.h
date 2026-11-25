#pragma once

#include "fs/cancel_token.h"
#include "fs/location.h"

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

  // Share types read from fstab and from native systemd units: NFS and SMB, plus legacy smbfs and ncpfs entries.
  bool networkMountType(std::string_view type);
  bool sameMountSource(std::string_view left, std::string_view right);
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
  // Resolves symlinked directory components without crossing into a matched automount.
  // When followFinal is false, a final symlink component is not followed (e.g. for operations on the link itself).
  std::optional<MountConfiguration> resolveAutomountFor(
      const Location& location, std::span<const MountConfiguration> configuration, bool followFinal = true
  );
  // True when following path would traverse across an automount that is not currently mounted.
  bool symlinkEntersUnmountedAutomount(
      std::string_view path, std::span<const MountConfiguration> configuration,
      std::span<const MountedFilesystem> mounted
  );
  SystemMountState mountState(const MountConfiguration& configuration, std::span<const MountedFilesystem> mounted);

  // Native systemd configuration is published by SystemMountMonitor. Readers never access a target directory.
  void publishNativeMounts(std::vector<MountConfiguration> configuration);
  std::vector<MountConfiguration> knownNativeMounts();
  std::vector<MountConfiguration> configuredSystemMounts();

  // Worker-only. Access triggers the system automount; verify the opened filesystem before browsing it.
  // Cancellation cannot interrupt an in-flight kernel mount, but prevents publishing its result.
  bool prepareSystemMount(const Location& location, const CancelToken& token);
} // namespace luciole
