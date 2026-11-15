#pragma once

#include "render/scene/node.h"
#include "ui/style.h"

#include <memory>
#include <string>

class Flex;

namespace luciole {
  // Side-list metrics: panel padding, scroll viewport padding, row height, row padding and gap.
  inline constexpr float kSideListPadding = Style::spaceXs;
  inline constexpr float kSideListViewportPaddingV = Style::spaceMd;
  // Rows match the tab strip's tabs (controlHeightSm).
  inline constexpr float kSideListRowHeight = Style::controlHeightSm;
  inline constexpr float kSideListRowPaddingH = Style::spaceSm;
  inline constexpr float kSideListRowGap = Style::spaceSm;

  // Raised group in a dialog: SurfaceContainerLow card (controls fill SurfaceContainer, so they stay
  // visible on it), radiusXl, OutlineVariant border, spaceSm x spaceMd padding. A non-empty title adds a
  // fontSizeTitle Bold heading, with `trailing` (a badge or button) at its right end. `body` receives the
  // column that holds the card's content.
  std::unique_ptr<Flex> card(const std::string& title, Flex** body, std::unique_ptr<Node> trailing = nullptr);
} // namespace luciole
