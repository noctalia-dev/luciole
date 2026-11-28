#include "browser/selection_tools.h"

#include "fs/name_matching.h"

std::optional<std::string> luciole::selectionPatternProblem(const std::string& pattern) {
  if (pattern.empty()) {
    return "Enter a pattern.";
  }
  if (pattern.size() > 256
      || pattern.contains('\0')
      || !g_utf8_validate(pattern.data(), static_cast<gssize>(pattern.size()), nullptr)) {
    return "Use a valid UTF-8 pattern of at most 256 bytes.";
  }
  return std::nullopt;
}
void luciole::TypeaheadSelection::clear() {
  m_text.clear();
  m_changed = {};
}
bool luciole::TypeaheadSelection::expire(Clock::time_point now) {
  if (m_text.empty() || now - m_changed < timeout) {
    return false;
  }
  clear();
  return true;
}
std::optional<luciole::EntryId> luciole::TypeaheadSelection::match(DirectoryModel& model, bool cycle) {
  const auto id = model.prefixMatch(m_text, cycle);
  if (id) {
    (void)model.select(*id);
  }
  return id;
}
std::optional<luciole::EntryId> luciole::TypeaheadSelection::append(
    DirectoryModel& model, const std::string& text, Clock::time_point now, bool composing
) {
  if (!composing) {
    (void)expire(now);
  }
  if (text.empty() || text.contains('\0') || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return std::nullopt;
  }
  const bool cycle = g_utf8_strlen(text.c_str(), -1) == 1 && foldedDisplayText(text) == foldedDisplayText(m_text);
  if (!cycle && m_text.size() + text.size() > 256) {
    return std::nullopt;
  }
  if (!cycle) {
    m_text += text;
  }
  m_changed = now;
  return match(model, cycle);
}
std::optional<luciole::EntryId>
luciole::TypeaheadSelection::erase(DirectoryModel& model, std::uint32_t bytes, Clock::time_point now, bool composing) {
  if (!composing) {
    (void)expire(now);
  }
  const auto offset = m_text.size() - std::min(m_text.size(), static_cast<std::size_t>(bytes));
  // Protocol deletion lengths are bytes; refuse partial UTF-8 characters.
  if (!g_utf8_validate(m_text.data(), static_cast<gssize>(offset), nullptr)) {
    return std::nullopt;
  }
  m_text.resize(offset);
  m_changed = now;
  return match(model, false);
}
