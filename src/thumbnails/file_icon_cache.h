#pragma once

#include "fs/file_icon_source.h"
#include "render/core/image_decoder.h"
#include "render/core/shared_texture_cache.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  struct FileIconImage {
    std::shared_ptr<const LoadedImageFile> pixels;
    std::string path;
    bool symbolic = false;
    bool ready = false;
    std::string sourceKey;
    std::string revision;
  };

  // Main-context cache. Workers resolve/decode; cells only present prepared pixels.
  class FileIconCache {
  public:
    FileIconCache();
    ~FileIconCache();
    FileIconCache(const FileIconCache&) = delete;
    FileIconCache& operator=(const FileIconCache&) = delete;

    std::shared_ptr<const FileIconImage> request(const FileIconSource& source, int pixels);
    SharedTextureCache& textures() { return m_textures; }
    void imageChanged();
    void invalidate();
    void setChangedCallback(std::function<void()> callback);
    // SVG icons under one of `roots` (a theme that follows the color scheme) render with `stylesheet` from later
    // requests on; no roots turns it off. invalidate() drops the icons drawn before.
    void setColorScheme(std::vector<std::string> roots, std::string stylesheet);

  private:
    SharedTextureCache m_textures;
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
