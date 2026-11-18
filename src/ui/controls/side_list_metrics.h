#pragma once

#include "ui/style.h"

namespace luciole {
  // Side-list metrics: panel padding, scroll viewport padding, row height, row padding and gap.
  inline constexpr float kSideListPadding = Style::spaceXs;
  inline constexpr float kSideListViewportPaddingV = Style::spaceMd;
  // Rows match the tab strip's tabs (controlHeightSm).
  inline constexpr float kSideListRowHeight = Style::controlHeightSm;
  inline constexpr float kSideListRowPaddingH = Style::spaceSm;
  inline constexpr float kSideListRowGap = Style::spaceSm;
  // Row icons at GTK's 16 px symbolic size (GNOME Files' sidebar); the Glyph default is the 14 px body size.
  inline constexpr float kSideListGlyphSize = Style::baseGlyphSize;
} // namespace luciole
