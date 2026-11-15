#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace luciole {
  // Text from a small vocabulary repeated across many entries (MIME types, owner and group names). Equal texts
  // share one process-lifetime copy, so a value is one pointer and comparison is pointer equality. A default value
  // is unknown; an empty text is known. Constructing interns the text; that is thread-safe.
  class InternedText {
  public:
    InternedText() = default;
    // Unknown, spelled for default member initializers.
    constexpr InternedText(std::nullptr_t) {}
    // A null pointer is unknown.
    InternedText(const char* text);
    InternedText(std::string_view text);
    InternedText(const std::string& text) : InternedText(std::string_view(text)) {}

    explicit operator bool() const { return m_text != nullptr; }
    bool has_value() const { return m_text != nullptr; }
    const std::string& operator*() const { return *m_text; }
    const std::string* operator->() const { return m_text; }
    std::string_view value_or(std::string_view fallback) const { return m_text != nullptr ? *m_text : fallback; }
    bool operator==(const InternedText&) const = default;

  private:
    const std::string* m_text = nullptr;
  };
} // namespace luciole
