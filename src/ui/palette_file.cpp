#include "ui/palette_file.h"

#include "fs/xdg_dirs.h"
#include "render/core/color.h"

#include <array>
#include <format>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>
#include <utility>

namespace {
  constexpr std::array<std::pair<ColorRole, Color Palette::*>, 28> kSlots{{
      {ColorRole::Primary, &Palette::primary},
      {ColorRole::OnPrimary, &Palette::onPrimary},
      {ColorRole::PrimaryContainer, &Palette::primaryContainer},
      {ColorRole::OnPrimaryContainer, &Palette::onPrimaryContainer},
      {ColorRole::Secondary, &Palette::secondary},
      {ColorRole::OnSecondary, &Palette::onSecondary},
      {ColorRole::SecondaryContainer, &Palette::secondaryContainer},
      {ColorRole::OnSecondaryContainer, &Palette::onSecondaryContainer},
      {ColorRole::Tertiary, &Palette::tertiary},
      {ColorRole::OnTertiary, &Palette::onTertiary},
      {ColorRole::TertiaryContainer, &Palette::tertiaryContainer},
      {ColorRole::OnTertiaryContainer, &Palette::onTertiaryContainer},
      {ColorRole::Error, &Palette::error},
      {ColorRole::OnError, &Palette::onError},
      {ColorRole::ErrorContainer, &Palette::errorContainer},
      {ColorRole::OnErrorContainer, &Palette::onErrorContainer},
      {ColorRole::Surface, &Palette::surface},
      {ColorRole::OnSurface, &Palette::onSurface},
      {ColorRole::OnSurfaceVariant, &Palette::onSurfaceVariant},
      {ColorRole::SurfaceContainerLow, &Palette::surfaceContainerLow},
      {ColorRole::SurfaceContainer, &Palette::surfaceContainer},
      {ColorRole::SurfaceContainerHigh, &Palette::surfaceContainerHigh},
      {ColorRole::SurfaceContainerHighest, &Palette::surfaceContainerHighest},
      {ColorRole::OutlineVariant, &Palette::outlineVariant},
      {ColorRole::Shadow, &Palette::shadow},
      {ColorRole::Scrim, &Palette::scrim},
      {ColorRole::Hover, &Palette::hover},
      {ColorRole::OnHover, &Palette::onHover},
  }};

  Color& slot(Palette& palette, ColorRole role) {
    for (const auto& [candidate, member] : kSlots) {
      if (candidate == role) {
        return palette.*member;
      }
    }
    std::unreachable();
  }
} // namespace

Palette luciole::defaultPalette(ColorScheme scheme) {
  if (scheme == ColorScheme::Light) {
    return {
        .primary = hex("#5058b1"),
        .onPrimary = hex("#f0eff7"),
        .primaryContainer = hex("#b2b5d8"),
        .onPrimaryContainer = hex("#3b3f95"),
        .secondary = hex("#555e88"),
        .onSecondary = hex("#f0eff7"),
        .secondaryContainer = hex("#b3b6cc"),
        .onSecondaryContainer = hex("#3e466f"),
        .tertiary = hex("#3b4168"),
        .onTertiary = hex("#fff0bd"),
        .tertiaryContainer = hex("#abaec3"),
        .onTertiaryContainer = hex("#3b4067"),
        .error = hex("#a53c59"),
        .onError = hex("#f0eff7"),
        .errorContainer = hex("#cbacbe"),
        .onErrorContainer = hex("#861d3f"),
        .surface = hex("#dcddea"),
        .onSurface = hex("#292b45"),
        .onSurfaceVariant = hex("#414765"),
        .surfaceContainerLow = hex("#d5d7e7"),
        .surfaceContainer = hex("#cfd2e4"),
        .surfaceContainerHigh = hex("#c2c6dd"),
        .surfaceContainerHighest = hex("#b5bad5"),
        .outlineVariant = hex("#a4aac9"),
        .shadow = hex("#333652"),
        .scrim = hex("#000000"),
        .hover = hex("#3b4168"),
        .onHover = hex("#fff0bd"),
    };
  }
  return {
      .primary = hex("#f7e9a8"),
      .onPrimary = hex("#302c1e"),
      .primaryContainer = hex("#585552"),
      .onPrimaryContainer = hex("#f7e9a8"),
      .secondary = hex("#b3b8ed"),
      .onSecondary = hex("#252740"),
      .secondaryContainer = hex("#444667"),
      .onSecondaryContainer = hex("#b3b8ed"),
      .tertiary = hex("#a0ddc0"),
      .onTertiary = hex("#1d3029"),
      .tertiaryContainer = hex("#3e5159"),
      .onTertiaryContainer = hex("#a0ddc0"),
      .error = hex("#ed8296"),
      .onError = hex("#3b2029"),
      .errorContainer = hex("#55364d"),
      .onErrorContainer = hex("#f68b9e"),
      .surface = hex("#15162e"),
      .onSurface = hex("#ece9f4"),
      .onSurfaceVariant = hex("#a5a7c6"),
      .surfaceContainerLow = hex("#1a1b35"),
      .surfaceContainer = hex("#20213d"),
      .surfaceContainerHigh = hex("#27284a"),
      .surfaceContainerHighest = hex("#2e2f58"),
      .outlineVariant = hex("#3e4166"),
      .shadow = hex("#0b0c1b"),
      .scrim = hex("#000000"),
      .hover = hex("#a0ddc0"),
      .onHover = hex("#1d3029"),
  };
}

std::filesystem::path luciole::palettePath() { return configHome() / "luciole" / "colors.toml"; }

std::expected<Palette, std::string>
luciole::parsePalette(std::string_view text, std::string_view source, ColorScheme scheme) {
  toml::table table;
  try {
    table = toml::parse(text, source);
  } catch (const toml::parse_error& error) {
    return std::unexpected(
        std::format("{}:{}: {}", source, error.source().begin.line, std::string_view(error.description()))
    );
  }
  const std::string_view name = scheme == ColorScheme::Dark ? "dark"
      : scheme == ColorScheme::Light                        ? "light"
                                                            : "colors";
  Palette result = defaultPalette(scheme);
  const auto* colors = table[name].as_table();
  if (colors == nullptr) {
    if (scheme == ColorScheme::Auto) {
      return std::unexpected(std::format("{}: missing [colors] table", source));
    }
    return result;
  }
  for (const auto& [key, node] : *colors) {
    const auto role = colorRoleFromToken(key.str());
    if (!role) {
      continue; // Noctalia exposes more tokens than the toolkit uses.
    }
    const auto* value = node.as_string();
    Color color;
    if (value == nullptr || !tryParseHexColor(value->get(), color)) {
      return std::unexpected(std::format("{}: {}.{} must be a hex color such as \"#1a1a39\"", source, name, key.str()));
    }
    slot(result, *role) = color;
  }
  return result;
}

std::expected<Palette, std::string> luciole::loadPalette(const std::filesystem::path& path, ColorScheme scheme) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error) {
      return defaultPalette(scheme);
    }
    return std::unexpected(std::format("{}: cannot be read", path.string()));
  }
  std::ostringstream text;
  text << file.rdbuf();
  return parsePalette(text.str(), path.string(), scheme);
}
