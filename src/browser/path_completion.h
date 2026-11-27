#pragma once

#include "browser/location_presentation.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace luciole {
  struct PathSuggestion {
    Location location;
    std::string label;
    std::string text;
  };
  struct PathSuggestions {
    std::vector<PathSuggestion> items;
    std::string error;
    bool truncated = false;
  };
  // Bounded asynchronous directory suggestions; each request retires its predecessor.
  class PathCompletion {
  public:
    PathCompletion();
    ~PathCompletion();
    PathCompletion(const PathCompletion&) = delete;
    PathCompletion& operator=(const PathCompletion&) = delete;
    void complete(
        const std::string& text, const Location& base, bool showHidden, std::function<void(PathSuggestions)> callback
    );
    void children(const Location& parent, bool showHidden, std::function<void(PathSuggestions)> completed);
    void cancel();

  private:
    void enumerate(
        const Location& parent, std::string prefix, bool showHidden, std::function<void(PathSuggestions)> completed
    );
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
