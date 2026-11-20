#pragma once

#include "fs/gio_ptr.h"
#include "fs/system_mounts.h"

#include <gio/gio.h>

namespace luciole {
  // Worker-only, bounded read of loaded systemd mount units. No units are loaded or started.
  std::optional<std::vector<MountConfiguration>>
  readSystemdMounts(GDBusConnection* connection, const CancelToken& token, bool* subscribed = nullptr);
} // namespace luciole
