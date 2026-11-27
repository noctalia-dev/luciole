#pragma once

#include "model/thumbnail_options.h"
#include "ui/style.h"
#include "ui/surface_shadow.h"

#include <algorithm>
#include <cmath>

namespace luciole {
  // Room above and below the preview stage for its drop shadow, so the scroll view does not clip it.
  inline constexpr float kPreviewImageInset = static_cast<float>(surface_shadow::kBlurRadius);
  // Softer than the popup default (0.55): previews rest on the pane rather than floating over content.
  inline constexpr ShadowStyle kPreviewShadow{.direction = ShadowDirection::Down, .alpha = 0.35F};

  // The fixed box every preview (image, text page, or type icon) is shown in, so the facts below it never move.
  struct PreviewStage {
    float width, height;
  };

  // Quick Look's dialog header and footer plus a six-row Details card (measured at 1x): the stage takes what is left,
  // so the dialog only scrolls for files with richer metadata.
  inline constexpr float kQuickLookReserve = 320.0F;

  inline PreviewStage previewStage(float width, float height, bool quick) {
    // Full content width, so images line up with the Details card; only above and below keep room for the shadow.
    const float stageWidth = std::max(40.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
    // About 4:3, capped so the Details card stays in view.
    const float cap = std::max(80.0F, (quick ? height - kQuickLookReserve : height * 0.5F) - kPreviewImageInset * 2);
    return {stageWidth, std::min(stageWidth * 0.75F, cap)};
  }

  // Texture side to request for a source image contained in the stage.
  inline int previewImagePixels(int sourceWidth, int sourceHeight, float width, float height, float scale, bool quick) {
    const auto stage = previewStage(width, height, quick);
    float extent = std::max(stage.width, stage.height);
    if (sourceWidth > 0 && sourceHeight > 0) {
      const float fit =
          std::min(stage.width / static_cast<float>(sourceWidth), stage.height / static_cast<float>(sourceHeight));
      extent = static_cast<float>(std::max(sourceWidth, sourceHeight)) * fit;
    }
    return static_cast<int>(
        std::ceil(std::clamp(extent * std::max(0.01F, scale), 1.0F, static_cast<float>(kMaxPreviewImageSide)))
    );
  }
} // namespace luciole
