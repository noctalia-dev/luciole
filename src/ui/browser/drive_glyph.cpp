#include "ui/browser/drive_glyph.h"

#include "render/text/glyph_registry.h"

const std::string& luciole::driveGlyph(const Drive& drive) {
  return !drive.iconName.empty() && GlyphRegistry::contains(drive.iconName) ? drive.iconName : drive.glyph;
}
