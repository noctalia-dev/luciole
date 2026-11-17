#pragma once

#include "ui/style.h"

// Geometry shared by the workspace window chrome and its pages.
constexpr float kWorkspaceInset = Style::spaceSm;
constexpr float kTabStripHeight = Style::controlHeightSm + 2.0F * Style::spaceXs;
constexpr float kNavigationToolbarHeight = 54.0F;
// The pane's rounded top-left corner, following the corner roundness setting.
inline float workspaceRadius() { return Style::scaledRadiusXl(); }
constexpr float kMinimumPaneWidth = 160.0F;
constexpr float kMinimumPreviewWidth = 208.0F;
constexpr float kMinimumFreeSpacePaneWidth = 320.0F;
// The footer of a FileChooser window: one row of controls between small margins.
constexpr float kChooserFooterHeight = Style::controlHeight + 2.0F * Style::spaceSm;
