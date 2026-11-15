#pragma once

#include "fs/location.h"

#include <memory>
#include <string>
#include <vector>

namespace luciole {
  struct FileIconSource {
    std::vector<std::string> names;
    std::string filePath;
    std::string desktopFile;
    bool operator==(const FileIconSource&) const = default;
  };

  // Equal sources are shared between entries; the pointee is immutable.
  using SharedFileIcon = std::shared_ptr<const FileIconSource>;

  const FileIconSource& emptyFileIcon();
  inline const FileIconSource& iconSource(const SharedFileIcon& icon) { return icon ? *icon : emptyFileIcon(); }
  // Pointer equality first; entries of one listing share their icons.
  inline bool sameIcon(const SharedFileIcon& a, const SharedFileIcon& b) {
    return a == b || iconSource(a) == iconSource(b);
  }
} // namespace luciole
