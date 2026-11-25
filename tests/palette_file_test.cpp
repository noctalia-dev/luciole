#include "render/core/color.h"
#include "test_check.h"
#include "ui/palette_file.h"

#include <print>
#include <string_view>

namespace {
  using luciole::ColorScheme;
  using luciole::defaultPalette;
  using luciole::parsePalette;

  constexpr std::string_view kBothSchemes = "[colors]\nsurface = \"#101010\"\n"
                                            "[dark]\nsurface = \"#202020\"\n"
                                            "[light]\nsurface = \"#e0e0e0\"\n";
  constexpr std::string_view kColorsOnly = "[colors]\nsurface = \"#101010\"\n";

  void testSchemeTables() {
    for (const auto& [scheme, surface] :
         {std::pair{ColorScheme::Auto, "#101010"}, {ColorScheme::Dark, "#202020"}, {ColorScheme::Light, "#e0e0e0"}}) {
      const auto palette = parsePalette(kBothSchemes, "test", scheme);
      TEST_CHECK(palette && palette->surface == hex(surface));
      TEST_CHECK(palette->primary == defaultPalette(scheme).primary);
    }
  }

  void testColorsOnlyFile() {
    const auto automatic = parsePalette(kColorsOnly, "test", ColorScheme::Auto);
    TEST_CHECK(automatic && automatic->surface == hex("#101010"));
    const auto dark = parsePalette(kColorsOnly, "test", ColorScheme::Dark);
    TEST_CHECK(dark && *dark == defaultPalette(ColorScheme::Dark));
    const auto light = parsePalette(kColorsOnly, "test", ColorScheme::Light);
    TEST_CHECK(light && *light == defaultPalette(ColorScheme::Light));
    TEST_CHECK(defaultPalette(ColorScheme::Light) != defaultPalette(ColorScheme::Dark));
  }

  void testErrors() {
    TEST_CHECK(!parsePalette("[dark]\nsurface = \"#000000\"\n", "test", ColorScheme::Auto));
    TEST_CHECK(!parsePalette("[light]\nsurface = \"blue\"\n", "test", ColorScheme::Light));
  }
} // namespace

int main() {
  testSchemeTables();
  testColorsOnlyFile();
  testErrors();
  std::println("palette file checks passed");
  return 0;
}
