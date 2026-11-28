#pragma once

#include "render/core/image_decoder.h"

#include <expected>
#include <gio/gio.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace luciole {
  class InstalledThumbnailers {
  public:
    static InstalledThumbnailers discover();
    bool contains(const std::string& mime) const { return m_commands.contains(mime); }
    std::expected<LoadedImageFile, std::string>
    render(int sourceFd, const std::string& mime, std::string_view name, int side, GCancellable* cancel) const;

  private:
    std::unordered_map<std::string, std::vector<std::string>> m_commands;
    std::string m_sandbox;
  };
} // namespace luciole
