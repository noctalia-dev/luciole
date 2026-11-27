#pragma once

#include <array>
#include <charconv>
#include <clocale>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace luciole {
  struct SizeUnits {
    std::array<std::string, 8> scaled = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
    std::function<std::string_view(std::uint64_t)> byteWord = [](std::uint64_t count) -> std::string_view {
      return count == 1 ? "byte" : "bytes";
    };
  };

  // Integer with the system locale's thousands separator (localeconv, initialized
  // from the environment by setlocale at startup): "68 397 157 542" in uk,
  // "68,397,157,542" in en_US, plain digits in the C locale.
  inline std::string formatIntegerWithLocaleSeparators(std::uint64_t value) {
    const struct lconv* const lc = std::localeconv();
    const std::string_view separator = lc->thousands_sep;
    const std::string digits = std::to_string(value);
    if (separator.empty()) {
      return digits;
    }
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
      if (i != 0 && (digits.size() - i) % 3 == 0) {
        out.append(separator);
      }
      out.push_back(digits[i]);
    }
    return out;
  }

  inline std::string formatByteCount(std::uint64_t bytes, const SizeUnits& units) {
    return std::format("{} {}", formatIntegerWithLocaleSeparators(bytes), units.byteWord(bytes));
  }

  inline std::string formatSizeWithUnits(std::uint64_t bytes, const SizeUnits& units) {
    if (bytes < 1024) {
      return formatByteCount(bytes, units);
    }
    double value = static_cast<double>(bytes) / 1024;
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < units.scaled.size()) {
      value /= 1024;
      ++unit;
    }
    const struct lconv* const lc = std::localeconv();
    const std::string_view separator = lc->thousands_sep;
    const std::string_view decimalPoint = lc->decimal_point;
    char buffer[64]{};
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 2);
    const std::string text(ec == std::errc{} ? std::string_view(buffer, ptr) : std::string_view{});
    const std::size_t dot = text.find('.');
    std::string out;
    out.reserve(text.size() + 4);
    for (std::size_t i = 0; i < dot; ++i) {
      if (i != 0 && (dot - i) % 3 == 0) {
        out.append(separator);
      }
      out.push_back(text[i]);
    }
    out.append(decimalPoint);
    out.append(text.substr(dot + 1));
    return std::format("{} {}", out, units.scaled[unit]);
  }
} // namespace luciole
