#pragma once

#include "browser/directory_model.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace luciole {
  std::optional<std::string> selectionPatternProblem(const std::string& pattern);
  // Finds the next visible entry from `currentIndex` in `direction` (-1 or +1) stepping by `stride` (1 in
  // lists/columns, column count in grids) that satisfies `accept`.
  template <std::predicate<EntryId> Predicate>
  std::optional<EntryId> adjacentRenameTarget(
      std::span<const EntryId> rows, std::size_t currentIndex, int direction, std::size_t stride, Predicate&& accept
  ) {
    if (rows.empty() || direction == 0 || currentIndex >= rows.size()) {
      return std::nullopt;
    }
    const auto step = static_cast<std::int64_t>(std::max<std::size_t>(1, stride)) * (direction < 0 ? -1 : 1);
    auto index = static_cast<std::int64_t>(currentIndex) + step;
    const auto count = static_cast<std::int64_t>(rows.size());
    while (index >= 0 && index < count) {
      const auto candidate = rows[static_cast<std::size_t>(index)];
      if (accept(candidate)) {
        return candidate;
      }
      index += step;
    }
    return std::nullopt;
  }
  // Append-only, bounded input for the file pane's optional typeahead mode.
  // The clock is explicit so timeout, repeat cycling and edits are testable.
  class TypeaheadSelection {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto timeout = std::chrono::milliseconds(1200);
    std::optional<EntryId>
    append(DirectoryModel& model, const std::string& text, Clock::time_point now, bool composing = false);
    std::optional<EntryId>
    erase(DirectoryModel& model, std::uint32_t bytes, Clock::time_point now, bool composing = false);
    void clear();
    bool expire(Clock::time_point now);
    const std::string& text() const { return m_text; }

  private:
    std::optional<EntryId> match(DirectoryModel& model, bool cycle);
    std::string m_text;
    Clock::time_point m_changed;
  };
} // namespace luciole
