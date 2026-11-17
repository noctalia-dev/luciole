#pragma once

#include <cstdint>
#include <optional>

class PageContext;
namespace luciole {
  class MountInteraction;
}
class MountController {
public:
  explicit MountController(PageContext& page) : m_page(page) {}
  void onFrame();

private:
  luciole::MountInteraction* current(std::uint64_t context, std::uint64_t request);
  PageContext& m_page;
  struct Active {
    std::uint64_t context, request;
  };
  std::optional<Active> m_active;
};
