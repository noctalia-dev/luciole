#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace luciole {
  // Every control the browsing toolbar can show, in its default left-to-right order.
  enum class ToolbarItem : std::uint8_t {
    Sidebar,
    Back,
    Forward,
    Parent,
    Location,
    Search,
    NewFolder,
    EmptyTrash,
    Sort,
    View,
    Operations,
    Menu
  };
  inline constexpr std::size_t kToolbarItemCount = 12;
  using ToolbarOrder = std::array<ToolbarItem, kToolbarItemCount>;
  inline constexpr ToolbarOrder kDefaultToolbarOrder{
      ToolbarItem::Sidebar,  ToolbarItem::Back,   ToolbarItem::Forward,    ToolbarItem::Parent,
      ToolbarItem::Location, ToolbarItem::Search, ToolbarItem::NewFolder,  ToolbarItem::EmptyTrash,
      ToolbarItem::Sort,     ToolbarItem::View,   ToolbarItem::Operations, ToolbarItem::Menu
  };
  inline constexpr std::array<std::string_view, kToolbarItemCount> kToolbarItemNames{
      "sidebar",    "back",        "forward", "parent", "location",   "search",
      "new_folder", "empty_trash", "sort",    "view",   "operations", "menu"
  };

  inline bool validToolbarOrder(ToolbarOrder order) {
    std::ranges::sort(order);
    return order == kDefaultToolbarOrder;
  }
  inline std::string_view toolbarItemName(ToolbarItem item) {
    const auto index = static_cast<std::size_t>(item);
    return index < kToolbarItemNames.size() ? kToolbarItemNames[index] : std::string_view{};
  }
  inline std::optional<ToolbarItem> toolbarItemFromName(std::string_view name) {
    const auto* found = std::ranges::find(kToolbarItemNames, name);
    return found != kToolbarItemNames.end() ? std::optional(static_cast<ToolbarItem>(found - kToolbarItemNames.begin()))
                                            : std::nullopt;
  }
  // Swaps `item` with the nearest shown item in `direction` (-1 left, 1 right); hidden items keep their slots.
  template <class Shown>
  std::optional<ToolbarOrder> moveToolbarItem(ToolbarOrder order, ToolbarItem item, int direction, Shown shown) {
    const auto* found = std::ranges::find(order, item);
    if (!validToolbarOrder(order) || (direction != -1 && direction != 1) || found == order.end()) {
      return std::nullopt;
    }
    const auto source = found - order.begin();
    for (auto target = source + direction; target >= 0 && target < static_cast<std::ptrdiff_t>(order.size());
         target += direction) {
      if (shown(order[static_cast<std::size_t>(target)])) {
        std::swap(order[static_cast<std::size_t>(source)], order[static_cast<std::size_t>(target)]);
        return order;
      }
    }
    return std::nullopt;
  }
} // namespace luciole
