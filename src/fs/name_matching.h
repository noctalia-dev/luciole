#pragma once

#include "fs/gio_ptr.h"

#include <algorithm>
#include <glib.h>
#include <string>

namespace luciole {
  inline std::string foldedDisplayText(const std::string& text) {
    // For non-NUL ASCII, casefolding is lowercasing and composition changes nothing.
    if (std::ranges::all_of(text, [](unsigned char c) { return c != 0 && c < 0x80; })) {
      std::string folded = text;
      for (auto& c : folded) {
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c + ('a' - 'A'));
        }
      }
      return folded;
    }
    const GString valid(g_utf8_make_valid(text.c_str(), static_cast<gssize>(text.size())));
    const GString lower(g_utf8_casefold(valid.get(), -1));
    const GString normalized(g_utf8_normalize(lower.get(), -1, G_NORMALIZE_DEFAULT_COMPOSE));
    return normalized.get();
  }
} // namespace luciole
