#pragma once

#include "i18n/i18n_service.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Free-function translation API. Call sites look like:
//
//   i18n::tr("clipboard.title")
//   i18n::tr("settings.entities.bar.label", "name", "top")
//
// Trailing arguments are alternating name/value pairs. Values may be any type
// formattable by std::format; names must be convertible to string_view.
// Missing keys fall back to their identifier so production UI never exposes
// diagnostic markers such as "!!key!!".

namespace i18n {

  namespace detail {

    template <typename T> inline std::string argToString(T&& v) {
      using U = std::remove_cvref_t<T>;
      if constexpr (std::is_convertible_v<const U&, std::string_view>) {
        return std::string(std::string_view(v));
      } else {
        return std::format("{}", std::forward<T>(v));
      }
    }

    using Pair = std::pair<std::string_view, std::string>;

    std::string interpolate(std::string_view tmpl, std::span<const Pair> args);

    template <typename Name, typename Value, typename... Rest>
    void fillPairs(Pair* pairs, std::size_t idx, Name&& name, Value&& value, Rest&&... rest) {
      pairs[idx] = Pair{std::string_view(name), argToString(std::forward<Value>(value))};
      if constexpr (sizeof...(Rest) > 0) {
        fillPairs(pairs, idx + 1, std::forward<Rest>(rest)...);
      }
    }

    template <typename... Args>
    std::string formatTranslation(std::string_view key, std::string_view raw, Args&&... args) {
      static_assert(
          sizeof...(Args) % 2 == 0, "i18n::tr() requires an even number of trailing args (name, value pairs)"
      );
      if (raw.empty()) {
        return std::string(key);
      }
      if constexpr (sizeof...(Args) == 0) {
        return std::string(raw);
      } else {
        constexpr std::size_t kPairs = sizeof...(Args) / 2;
        std::array<detail::Pair, kPairs> pairs;
        detail::fillPairs(pairs.data(), 0, std::forward<Args>(args)...);
        return detail::interpolate(raw, std::span<const detail::Pair>(pairs.data(), pairs.size()));
      }
    }

  } // namespace detail

  // Convert a snake_case config/wire identifier into the dash-case segment used
  // in translation keys (e.g. "art_size" -> "art-size").
  inline std::string keySegment(std::string_view id) {
    std::string out(id);
    std::ranges::replace(out, '_', '-');
    return out;
  }

  template <typename... Args> std::string tr(std::string_view key, Args&&... args) {
    return detail::formatTranslation(key, Service::instance().lookup(key), std::forward<Args>(args)...);
  }

  // Translates a user-facing English message when a catalog entry exists and
  // otherwise preserves the original text. This is useful for messages that
  // are assembled by lower-level services and must remain safe to display.
  inline std::string text(std::string_view original) {
    const auto translated = Service::instance().lookup(original);
    return translated.empty() ? std::string(original) : std::string(translated);
  }

  template <typename... Args> std::string trp(std::string_view key, std::uint64_t count, Args&&... args) {
    const auto raw = Service::instance().translations().lookupPlural(key, count);
    return detail::formatTranslation(key, raw, "count", count, std::forward<Args>(args)...);
  }

} // namespace i18n
