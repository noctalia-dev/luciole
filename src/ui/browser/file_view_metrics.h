#pragma once

#include "model/directory_options.h"

#include <algorithm>

namespace luciole {
  inline float treePrefixWidth(std::size_t depth, float nameWidth, float iconSize) {
    return std::min(static_cast<float>(depth) * 16.0F, std::max(0.0F, nameWidth - iconSize - 80.0F)) + 20.0F;
  }
  struct FileViewMetrics {
    float inset, iconSize, nameTop, captionTop, parentTop, height, minWidth, gap;
  };

  // Shared by cell layout, viewport allocation and pointer hit testing.
  inline FileViewMetrics gridMetrics(float iconSize, BrowsePresentation presentation, bool context) {
    const bool comfortable = presentation.density == ViewDensity::Comfortable;
    const float inset = comfortable ? 12.0F : 4.0F;
    const float nameTop = inset + iconSize + (comfortable ? 8.0F : 2.0F);
    const float captionTop = nameTop + 40.0F;
    const float parentTop = captionTop + 18.0F * presentation.captions.count();
    return {
        inset,
        iconSize,
        nameTop,
        captionTop,
        parentTop,
        parentTop + (context ? 20.0F : 0.0F) + (comfortable ? 8.0F : 0.0F),
        std::max(comfortable ? 88.0F : 72.0F, iconSize + (comfortable ? 72.0F : 40.0F)),
        comfortable ? 8.0F : 2.0F
    };
  }
  // Column view rows follow the density; List rows use fileRowIconSize().
  inline float millerRowIconSize(BrowsePresentation presentation) {
    return presentation.density == ViewDensity::Comfortable ? 28.0F : 20.0F;
  }
  // A listIconSize of 0 keeps the default size.
  inline float fileRowIconSize(int listIconSize) {
    return static_cast<float>(listIconSize > 0 ? listIconSize : kDefaultListIconSize);
  }
  inline float fileRowHeight(BrowsePresentation presentation, bool context, float iconSize) {
    return iconSize + (presentation.density == ViewDensity::Comfortable ? 16.0F : 8.0F) + (context ? 16.0F : 0.0F);
  }
} // namespace luciole
