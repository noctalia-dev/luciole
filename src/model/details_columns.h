#pragma once

#include "model/directory_options.h"

namespace luciole {
  struct DetailsColumns {
    bool showType = true;
    bool showSize = true;
    bool showModified = true;
    float nameWidth = 160.0F;
    float typeWidth = 144.0F;
    float sizeWidth = 110.0F;
    float modifiedWidth = 148.0F;
    std::array<bool, 24> extraVisible{};
    std::array<float, 24> extraWidths{148, 148, 120, 120, 104, 252, 120, 104, 104, 112, 112, 148,
                                      88,  88,  144, 140, 124, 112, 88,  72,  96,  104, 96,  112};
    bool visible(DetailsColumn column) const {
      const auto index = static_cast<std::size_t>(column);
      return index == 0 ? showType : index == 1 ? showSize : index == 2 ? showModified : extraVisible[index - 3];
    }
    float width(DetailsColumn column) const {
      const auto index = static_cast<std::size_t>(column);
      return index == 0 ? typeWidth : index == 1 ? sizeWidth : index == 2 ? modifiedWidth : extraWidths[index - 3];
    }
    float contentWidth = 594.0F;

    static DetailsColumns forWidth(
        float width, DetailsColumnVisibility visibility = {},
        DetailsColumnWidths requested = kDefaultDetailsColumnWidths
    ) {
      float metadata = 0;
      for (const auto column : kDefaultDetailsColumnOrder) {
        if (detailsColumnVisible(visibility, column)) {
          metadata += requested[static_cast<std::size_t>(column) + 1];
        }
      }
      const float name =
          requested[0] == 0 ? std::clamp(width - 8.0F - metadata, 160.0F, 16384.0F) : static_cast<float>(requested[0]);
      DetailsColumns result{
          .showType = visibility.type,
          .showSize = visibility.size,
          .showModified = visibility.modified,
          .nameWidth = name,
          .typeWidth = static_cast<float>(requested[1]) - 8.0F,
          .sizeWidth = static_cast<float>(requested[2]) - 8.0F,
          .modifiedWidth = static_cast<float>(requested[3]) - 8.0F,
          .contentWidth = 8.0F + name + metadata
      };
      for (std::size_t i = 0; i < result.extraVisible.size(); ++i) {
        result.extraVisible[i] = detailsColumnVisible(visibility, static_cast<DetailsColumn>(i + 3));
        result.extraWidths[i] = static_cast<float>(requested[i + 4]) - 8.0F;
      }
      return result;
    }
    bool operator==(const DetailsColumns&) const = default;
  };
} // namespace luciole
