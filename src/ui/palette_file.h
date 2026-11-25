#pragma once

#include "model/directory_options.h"
#include "ui/palette.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace luciole {
  // Noctalia's built-in palette: light for ColorScheme::Light, dark otherwise.
  Palette defaultPalette(ColorScheme scheme);
  // $XDG_CONFIG_HOME/luciole/colors.toml
  std::filesystem::path palettePath();
  // Reads `[colors]` (Auto), `[dark]` or `[light]` with Noctalia template token keys and "#rrggbb"/"#rrggbbaa"
  // values over defaultPalette(scheme). Unknown keys are ignored. Dark and Light fall back to the built-in palette
  // when their table is absent, as in files from Noctalia versions that only wrote [colors].
  std::expected<Palette, std::string> parsePalette(std::string_view text, std::string_view source, ColorScheme scheme);
  // A missing file yields defaultPalette(scheme).
  std::expected<Palette, std::string> loadPalette(const std::filesystem::path& path, ColorScheme scheme);
} // namespace luciole
