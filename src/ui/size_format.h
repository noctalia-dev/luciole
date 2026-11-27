#pragma once

#include "i18n/i18n.h"
#include "model/size_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace luciole {
  // Retain the catalog so background selection totals can use the same
  // language even if the active catalog changes during the calculation.
  inline SizeUnits localizedSizeUnits() {
    SizeUnits units;
    for (auto& name : units.scaled) {
      name = i18n::text(name);
    }
    units.byteWord = [catalog = i18n::Service::instance().translations()](std::uint64_t count) {
      const auto word = catalog.lookupPlural("byte", count);
      return word.empty() ? std::string_view(count == 1 ? "byte" : "bytes") : word;
    };
    return units;
  }

  // Byte count with the localized IEC unit names and the system locale's separators.
  inline std::string formatSizeWithLocalizedUnits(std::uint64_t bytes) {
    return formatSizeWithUnits(bytes, localizedSizeUnits());
  }
} // namespace luciole
