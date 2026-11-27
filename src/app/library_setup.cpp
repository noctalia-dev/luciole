#include "app/library_setup.h"

#include "config/keybinds.h"
#include "core/application_identity.h"
#include "core/files/resource_paths.h"
#include "core/ui_strings.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "imaging/pixbuf_image_decoder.h"
#include "luciole_build_info.h"
#include "ui/palette_file.h"

#include <array>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <unistd.h>
namespace luciole {
  void initializeLibrary() {
    setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
    const char* overridePath = std::getenv("LUCIOLE_ASSETS_DIR");
    const std::filesystem::path installed = LUCIOLE_INSTALL_ASSETSDIR;
    const auto executable = [] {
      std::array<char, 4096> buffer{};
      const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
      if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size() - 1) {
        return std::filesystem::path{};
      }
      buffer[static_cast<std::size_t>(count)] = '\0';
      return std::filesystem::path(buffer.data());
    }();
    const auto sourceRoot = std::filesystem::current_path();
    const auto executableRoot = executable.empty() ? std::filesystem::path{} : executable.parent_path().parent_path();
    const auto sourceAssets = sourceRoot / "assets";
    const auto executableAssets = executableRoot / "assets";
    const auto sourceLibrary = sourceRoot / "lib" / "assets";
    const auto executableLibrary = executableRoot / "lib" / "assets";
    const auto validApplication = [](const std::filesystem::path& path) {
      std::error_code error;
      return std::filesystem::is_regular_file(path / "translations" / "en.json", error);
    };
    const auto validLibrary = [](const std::filesystem::path& path) {
      std::error_code error;
      return std::filesystem::is_regular_file(path / "fonts" / "noctalia-tabler.ttf", error)
          && std::filesystem::is_regular_file(path / "fonts" / "tabler.json", error);
    };
    std::filesystem::path assets;
    std::filesystem::path library;
    if (overridePath != nullptr && validApplication(overridePath)) {
      assets = overridePath;
    } else if (!executableRoot.empty() && validLibrary(executableLibrary) && validApplication(executableAssets)) {
      assets = executableAssets;
    } else if (validApplication(installed)) {
      assets = installed;
    } else if (validApplication(sourceAssets)) {
      assets = sourceAssets;
    } else if (validApplication(executableAssets)) {
      assets = executableAssets;
    } else {
      assets = installed;
    }
    if (validLibrary(assets)) {
      library = assets;
    } else if (validLibrary(sourceLibrary)) {
      library = sourceLibrary;
    } else if (validLibrary(executableLibrary)) {
      library = executableLibrary;
    } else {
      library = assets;
    }
    paths::setAssetRoots({.application = assets, .library = library});
    i18n::Service::instance().init();
    // Initialize the C locale from the environment so localeconv()-based number
    // formatting (StringUtils::format*WithLocaleSeparators)
    // uses the system's decimal point and thousands separator.
    setlocale(LC_ALL, "");
    setUiStrings({
        .keyName = [](std::string_view name) -> std::string { return std::string(keybindKeyNameLabel(name)); },
        .selectPlaceholder = [] { return i18n::tr("common.select"); },
    });
    installPixbufImageDecoder();
    // PaletteWatcher applies colors.toml once the main loop owner exists.
    setPalette(defaultPalette(ColorScheme::Dark));
  }
} // namespace luciole
