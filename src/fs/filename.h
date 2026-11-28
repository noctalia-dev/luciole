#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace luciole {
  std::optional<std::string> filenameProblem(const std::string& name);
  std::size_t filenameStemLength(const std::string& name, bool directory);
  // End byte offset from 0 when pressing Rename again while editing `name`: toggles between the stem and the full
  // filename (including extension) when an extension exists.
  std::size_t
  nextRenameSelectionEnd(const std::string& name, bool directory, std::size_t selectionStart, std::size_t selectionEnd);
  // Filename bytes as valid UTF-8 presentation text.
  std::string filenameDisplayText(const std::string& name);
} // namespace luciole
