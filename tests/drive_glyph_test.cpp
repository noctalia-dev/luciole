#include "core/files/resource_paths.h"
#include "fs/drive_monitor.h"
#include "test_check.h"
#include "ui/browser/drive_glyph.h"

#include <cstdlib>
#include <print>

int main() {
  using luciole::deviceGlyph;
  TEST_CHECK(deviceGlyph("phone") == "device-mobile");
  TEST_CHECK(deviceGlyph("phone-samsung-galaxy-s") == "device-mobile");
  TEST_CHECK(deviceGlyph("phone-apple-iphone") == "device-mobile");
  TEST_CHECK(deviceGlyph("multimedia-player") == "device-speaker");
  TEST_CHECK(deviceGlyph("multimedia-player-apple-ipod-touch") == "device-speaker");
  TEST_CHECK(deviceGlyph("computer-apple-ipad") == "device-ipad");
  TEST_CHECK(deviceGlyph("computer").empty());
  TEST_CHECK(deviceGlyph("camera-photo") == "camera");
  TEST_CHECK(deviceGlyph("camera").empty());
  TEST_CHECK(deviceGlyph("drive-removable-media-usb").empty());
  TEST_CHECK(deviceGlyph("device-watch").empty());
  TEST_CHECK(deviceGlyph("").empty());

  const char* assets = std::getenv("LUCIOLE_ASSETS_DIR");
  TEST_CHECK(assets != nullptr);
  paths::setAssetRoots({.application = assets, .library = assets});
  luciole::Drive drive;
  drive.glyph = "server";
  TEST_CHECK(luciole::driveGlyph(drive) == "server");
  drive.iconName = "device-watch";
  TEST_CHECK(luciole::driveGlyph(drive) == "device-watch");
  drive.iconName = "drive-removable-media-usb";
  TEST_CHECK(luciole::driveGlyph(drive) == "server");
  std::println("Drive device glyphs passed");
}
