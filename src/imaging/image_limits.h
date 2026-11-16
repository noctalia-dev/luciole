#pragma once

#include <cstddef>
#include <cstdint>

namespace luciole {
  // Largest encoded image file or data URI payload read into memory.
  inline constexpr std::size_t kMaxEncodedImageBytes = 64U * 1024U * 1024U;
  // Default decoded pixel budget for images shown in the UI.
  inline constexpr std::uint64_t kMaxImagePixels = 16U * 1024U * 1024U;
  // Upper bound for a caller-supplied PNG pixel budget.
  inline constexpr std::uint64_t kMaxImagePixelBudget = 64'000'000;
  inline constexpr int kMaxImageSide = 8192;
  inline constexpr double kMaxSvgSourceSide = 32768.0;
  inline constexpr int kMaxSvgRasterSide = 4096;
} // namespace luciole
