#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "imaging/pixbuf_image_decoder.h"
#include "system/icon_resolver.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <png.h>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  struct Fixture {
    fs::path root;
    Fixture() {
      const luciole::GString path(g_dir_make_tmp("luciole-icons-XXXXXX", nullptr));
      TEST_CHECK(path != nullptr);
      root = path.get();
      fs::create_directories(root / "config/gtk-3.0");
      fs::create_directories(root / "data/icons/Test/scalable/apps");
      fs::create_directories(root / "data/icons/Test/16x16/apps");
      fs::create_directories(root / "data/icons/Test/64x64/apps");
      fs::create_directories(root / "data/icons/Test/48x48/apps");
      fs::create_directories(root / "data/icons/Test/96x96/apps");
      fs::create_directories(root / "data/icons/Test/128x128/apps");
      fs::create_directories(root / "data/icons/Test/threshold/apps");
      fs::create_directories(root / "data/icons/Parent/scalable/apps");
      std::ofstream(root / "config/gtk-3.0/settings.ini") << "[Settings]\ngtk-icon-theme-name=Test\n";
      std::ofstream(root / "data/icons/Test/index.theme")
          << "[Icon Theme]\nName=Test\nInherits=Parent\nFollowsColorScheme=true\n"
             "Directories=scalable/apps,16x16/apps,48x48/apps,64x64/apps,96x96/apps,128x128/apps,threshold/apps\n"
             "[scalable/apps]\nSize=48\nType=Scalable\nMinSize=16\nMaxSize=256\n"
             "[16x16/apps]\nSize=16\nType=Fixed\n[48x48/apps]\nSize=48\nType=Fixed\n"
             "[64x64/apps]\nSize=64\nType=Fixed\n[96x96/apps]\nSize=96\nType=Fixed\n"
             "[128x128/apps]\nSize=128\nType=Scalable\nMinSize=128\nMaxSize=512\n"
             "[threshold/apps]\nSize=24\nType=Threshold\nThreshold=2\n";
      std::ofstream(root / "data/icons/Parent/index.theme")
          << "[Icon Theme]\nName=Parent\nDirectories=scalable/apps\n[scalable/apps]\nSize=48\nType=Scalable\n";
      for (const auto* name : {"folder", "text-x-generic", "custom-app"}) {
        std::ofstream(root / "data/icons/Test/scalable/apps" / (std::string(name) + ".svg"))
            << "<svg xmlns='http://www.w3.org/2000/svg' width='48' height='48'>"
               "<path fill='#00cc44' d='M0 0h48v48H0z'/></svg>";
      }
      fs::copy_file(
          root / "data/icons/Test/scalable/apps/folder.svg", root / "data/icons/Parent/scalable/apps/inherited.svg"
      );
      std::ofstream(root / "data/icons/Test/scalable/apps/broken.svg") << "broken SVG";
      for (const auto size : {16, 48, 64}) {
        std::ofstream(root / "data/icons/Test" / (std::to_string(size) + "x" + std::to_string(size)) / "apps/sized.svg")
            << "<svg xmlns='http://www.w3.org/2000/svg' width='"
            << size
            << "' height='"
            << size
            << "'>"
            << "<path fill='#00cc44' d='M4 8h"
            << size - 8
            << "v"
            << size - 16
            << "H4z'/></svg>";
      }
      // Themes such as Papirus use 48-pixel artwork for 96 and a different 64-pixel drawing for 128+.
      fs::create_symlink("../../48x48/apps/sized.svg", root / "data/icons/Test/96x96/apps/sized.svg");
      fs::create_symlink("../../64x64/apps/sized.svg", root / "data/icons/Test/128x128/apps/sized.svg");
      fs::copy_file(root / "data/icons/Test/16x16/apps/sized.svg", root / "data/icons/Test/threshold/apps/ranged.svg");
      fs::copy_file(root / "data/icons/Test/64x64/apps/sized.svg", root / "data/icons/Test/128x128/apps/ranged.svg");
      // Drawn like Breeze's folders: the fill follows the color scheme's accent, #3daee9 by default.
      for (const auto& path : {root / "data/icons/Test/scalable/apps/accent-folder.svg", root / "accent-folder.svg"}) {
        std::ofstream(path) << "<svg xmlns='http://www.w3.org/2000/svg' width='48' height='48'>"
                               "<style id='current-color-scheme'>.ColorScheme-Accent { color: #3daee9; }</style>"
                               "<path class='ColorScheme-Accent' style='fill:currentColor' d='M0 0h48v48H0z'/></svg>";
      }
      std::ofstream(root / "launcher.desktop") << "[Desktop Entry]\nType=Application\nIcon=custom-app\nExec=false\n";
      for (const int size : {16, 64}) {
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        image.width = image.height = size;
        image.format = PNG_FORMAT_RGBA;
        const std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4U, 255);
        TEST_CHECK(
            png_image_write_to_file(
                &image,
                (root / ("data/icons/Test/" + std::to_string(size) + "x" + std::to_string(size) + "/apps/bitmap.png"))
                    .c_str(),
                0, pixels.data(), 0, nullptr
            )
            != 0
        );
      }
      fs::copy_file(root / "data/icons/Test/16x16/apps/bitmap.png", root / "data/icons/Test/16x16/apps/mixed.png");
      fs::copy_file(root / "data/icons/Test/64x64/apps/sized.svg", root / "data/icons/Test/128x128/apps/mixed.svg");
      g_setenv("HOME", root.c_str(), TRUE);
      g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), TRUE);
      g_setenv("XDG_DATA_HOME", (root / "data").c_str(), TRUE);
      g_setenv("XDG_DATA_DIRS", (root / "empty").c_str(), TRUE);
      g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
      g_setenv("GSETTINGS_SCHEMA_DIR", (root / "empty").c_str(), TRUE);
    }
    ~Fixture() { fs::remove_all(root); }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int timeout = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  void testIcons() {
    Fixture fixture;
    Pump pump;
    IconResolver resolver(true);
    TEST_CHECK(IconResolver::activeThemeName() == "Test");
    TEST_CHECK(resolver.resolve("inherited", 48).contains("/Parent/"));
    TEST_CHECK(resolver.resolve("bitmap", 24).contains("/64x64/"));
    TEST_CHECK(resolver.resolve("bitmap", 12).contains("/16x16/"));
    TEST_CHECK(resolver.resolve("sized", 16).contains("/16x16/"));
    TEST_CHECK(resolver.resolve("sized", 48).contains("/48x48/"));
    TEST_CHECK(resolver.resolve("sized", 64).contains("/64x64/"));
    TEST_CHECK(resolver.resolve("sized", 96).contains("/96x96/"));
    TEST_CHECK(resolver.resolve("sized", 192).contains("/128x128/"));
    TEST_CHECK(resolver.resolve("ranged", 25).contains("/threshold/"));
    TEST_CHECK(resolver.resolve("ranged", 192).contains("/128x128/"));
    TEST_CHECK(resolver.resolve("mixed", 16).ends_with("/16x16/apps/mixed.png"));

    luciole::FileIconCache cache;
    const auto sized = cache.request({.names = {"sized"}, .filePath = {}, .desktopFile = {}}, 96);
    pump.until([&] { return sized->ready; });
    TEST_CHECK(sized->pixels && sized->path.contains("/96x96/") && sized->pixels->width == 96);
    const auto alpha = [&](int x) { return sized->pixels->rgba[(32U * 96U + x) * 4U + 3U]; };
    TEST_CHECK(alpha(6) == 0 && alpha(7) == 0 && alpha(8) == 255 && alpha(87) == 255 && alpha(88) == 0);
    const luciole::FileIconSource source{.names = {"broken", "missing", "folder"}, .filePath = {}, .desktopFile = {}};
    const auto first = cache.request(source, 48);
    TEST_CHECK(first == cache.request(source, 48));
    pump.until([&] { return first->ready; });
    TEST_CHECK(first->pixels && first->path.ends_with("folder.svg") && first->pixels->width == 48);
    TEST_CHECK(first->pixels->rgba[1] > first->pixels->rgba[0]);
    const auto large = cache.request(source, 96);
    pump.until([&] { return large->ready; });
    TEST_CHECK(large->pixels && large->pixels->width == 96);

    const luciole::GioPtr<GFileInfo> info(g_file_info_new());
    const luciole::GioPtr<GIcon> themed(g_themed_icon_new("inherited"));
    g_file_info_set_icon(info.get(), themed.get());
    g_file_info_set_name(info.get(), "launcher.desktop");
    g_file_info_set_file_type(info.get(), G_FILE_TYPE_REGULAR);
    const auto launcher =
        luciole::readFileIcon(info.get(), luciole::Location::fromPath((fixture.root / "launcher.desktop").native()));
    TEST_CHECK(launcher.names.front() == "inherited" && !launcher.desktopFile.empty());
    const auto app = cache.request(launcher, 48);
    pump.until([&] { return app->ready; });
    TEST_CHECK(app->path.ends_with("custom-app.svg"));

    const auto obsolete = cache.request(source, 80);
    cache.invalidate();
    const auto current = cache.request(source, 80);
    pump.until([&] { return current->ready; });
    TEST_CHECK(obsolete != current && !obsolete->ready);

    // A theme that follows the color scheme takes the palette's primary for its accent; a copy of the same SVG
    // outside the theme keeps its own color.
    const auto roots = IconResolver::colorSchemeThemeRoots();
    TEST_CHECK(std::ranges::contains(roots, (fixture.root / "data/icons/Test").native()));
    Palette colors{};
    colors.primary = rgbHex(0xe0457b);
    cache.setColorScheme(roots, iconColorSchemeStylesheet(colors));
    cache.invalidate();
    const auto accent = cache.request({.names = {"accent-folder"}, .filePath = {}, .desktopFile = {}}, 48);
    const auto foreign =
        cache.request({.names = {}, .filePath = (fixture.root / "accent-folder.svg").native(), .desktopFile = {}}, 48);
    pump.until([&] { return accent->ready && foreign->ready; });
    TEST_CHECK(accent->pixels && accent->path.contains("/Test/"));
    TEST_CHECK(
        (std::array{accent->pixels->rgba[0], accent->pixels->rgba[1], accent->pixels->rgba[2]})
        == (std::array<std::uint8_t, 3>{0xe0, 0x45, 0x7b})
    );
    TEST_CHECK(foreign->pixels);
    TEST_CHECK(
        (std::array{foreign->pixels->rgba[0], foreign->pixels->rgba[1], foreign->pixels->rgba[2]})
        == (std::array<std::uint8_t, 3>{0x3d, 0xae, 0xe9})
    );

    auto dying = std::make_unique<luciole::FileIconCache>();
    bool destroyed = false;
    dying->setChangedCallback([&] {
      dying.reset();
      destroyed = true;
    });
    const auto abandoned = dying->request(source, 32);
    pump.until([&] { return destroyed; });
    TEST_CHECK(abandoned->ready);
  }
} // namespace

int main() {
  luciole::installPixbufImageDecoder();
  try {
    testIcons();
    std::println("file icon checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "file icon test: {}", error.what());
    return 1;
  }
}
