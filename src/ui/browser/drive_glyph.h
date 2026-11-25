#pragma once

#include "fs/drive_monitor.h"

#include <string>

namespace luciole {
  // The glyph a drive row shows: the drive's icon name when a glyph has that name, else the monitor's choice.
  const std::string& driveGlyph(const Drive& drive);
} // namespace luciole
