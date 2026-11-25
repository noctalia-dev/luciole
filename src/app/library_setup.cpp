#include "app/library_setup.h"

#include "config/keybinds.h"
#include "core/application_identity.h"
#include "core/files/resource_paths.h"
#include "core/ui_strings.h"
#include "imaging/pixbuf_image_decoder.h"
#include "luciole_build_info.h"
#include "ui/palette_file.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>
namespace luciole {
  void initializeLibrary() {
    setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
    const char* overridePath = std::getenv("LUCIOLE_ASSETS_DIR");
    const std::filesystem::path assets = overridePath ? overridePath : LUCIOLE_INSTALL_ASSETSDIR;
    paths::setAssetRoots({.application = assets, .library = assets});
    setUiStrings({
        .keyName = [](std::string_view name) -> std::string { return std::string(keybindKeyNameLabel(name)); },
        .selectPlaceholder = [] { return std::string("Select…"); },
    });
    installPixbufImageDecoder();
    // PaletteWatcher applies colors.toml once the main loop owner exists.
    setPalette(defaultPalette(ColorScheme::Dark));
  }
} // namespace luciole
