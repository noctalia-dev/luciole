#pragma once

#include "fs/directory_session.h"
#include "model/size_format.h"

#include <array>
#include <span>
#include <string>
#include <utility>

namespace luciole {
  using SelectionFacts = std::vector<std::pair<std::string, std::string>>;
  // Reads selected entries only, with one worker and one replaceable latest request.
  // Large selections explicitly report the 4096-item inspection limit. No folder traversal.
  class SelectionDetailsService {
  public:
    SelectionDetailsService();
    ~SelectionDetailsService();
    SelectionDetailsService(const SelectionDetailsService&) = delete;
    SelectionDetailsService& operator=(const SelectionDetailsService&) = delete;
    void show(std::span<const DirectoryEntry> entries, const SizeUnits& units = {});
    void clear();
    void setChangedCallback(std::function<void()> changed);
    const SelectionFacts& fields() const;
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
