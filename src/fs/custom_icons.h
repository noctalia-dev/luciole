#pragma once

#include "fs/file_icon_source.h"
#include "fs/system_mounts.h"

#include <optional>
#include <span>
#include <string>

namespace luciole {
  // The icon a local folder sets for itself, in this order: GIO's metadata::custom-icon (an image URI) and
  // metadata::custom-icon-name (a themed name), as GNOME Files sets them, then Icon= in the folder's .directory
  // file, as Dolphin and PCManFM-Qt write it (a themed name or an absolute image path). nullopt when it sets none.
  // Reads the folder's metadata and one small file: call on a worker.
  std::optional<FileIconSource> customFolderIcon(const std::string& path);

  // What a symbolic link at `path` shows: for a folder, a custom icon set on the link or on its target folder, or
  // else the plain folder icon; for a file, the icon of the target's content type. nullopt for a dangling link. Follows
  // the link, which can block on a hung mount: call on a link worker. A target folder inside one of the `mounts`
  // automounts keeps the plain folder icon.
  std::optional<FileIconSource> linkTargetIcon(const std::string& path, std::span<const MountConfiguration> mounts);
} // namespace luciole
