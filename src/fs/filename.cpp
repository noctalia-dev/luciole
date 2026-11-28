#include "fs/filename.h"

#include "fs/gio_ptr.h"

#include <glib.h>

std::optional<std::string> luciole::filenameProblem(const std::string& name) {
  if (name.empty()) {
    return "Enter a name.";
  }
  if (name == "." || name == "..") {
    return "Names cannot be . or ..";
  }
  if (name.contains('/') || name.contains('\0')) {
    return "Names cannot contain / or a NUL byte.";
  }
  if (!g_utf8_validate(name.data(), static_cast<gssize>(name.size()), nullptr)) {
    return "Enter a valid UTF-8 name.";
  }
  return std::nullopt;
}
std::size_t luciole::filenameStemLength(const std::string& name, bool directory) {
  const auto dot = name.rfind('.');
  return !directory && dot != std::string::npos && dot != 0 && dot + 1 < name.size() ? dot : name.size();
}
std::size_t luciole::nextRenameSelectionEnd(
    const std::string& name, bool directory, std::size_t selectionStart, std::size_t selectionEnd
) {
  const auto stem = filenameStemLength(name, directory);
  if (stem == name.size()) {
    return name.size();
  }
  return selectionStart == 0 && selectionEnd == stem ? name.size() : stem;
}
std::string luciole::filenameDisplayText(const std::string& name) {
  const GString text(g_filename_display_name(name.c_str()));
  return text.get();
}
