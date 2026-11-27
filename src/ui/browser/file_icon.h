#pragma once

#include "render/core/texture_handle.h"
#include "render/scene/node.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/palette.h"

class ImageNode;
class RectNode;
class Flex;
class Glyph;
class TextureManager;

namespace luciole {
  class FileIcon final : public Node {
  public:
    FileIcon(FileIconCache& cache, float size, ThumbnailService* thumbnails = nullptr);
    void bindFile(const DirectoryEntry& entry, ColorSpec foreground);
    void setEmblems(bool link, bool readOnly);
    void setThumbnailVisible(bool visible);
    void releaseFile(Renderer& renderer);
    ~FileIcon() override;
    void bind(const FileIconSource& source, ColorSpec foreground);
    void setIconSize(float size);
    void setPreviewHeight(float height);
    void setPreparedImage(std::shared_ptr<const FileIconImage> image);

  protected:
    void doLayout(Renderer& renderer) override;
    LayoutSize doMeasure(Renderer&, const LayoutConstraints&) override;
    void doInvalidateGpuResources(Renderer&) override;

  private:
    FileIconCache& m_cache;
    ThumbnailService* m_thumbnails = nullptr;
    std::optional<DirectoryEntry> m_thumbnailSource;
    std::shared_ptr<const FileIconImage> m_thumbnail;
    int m_thumbnailPixels = 0;
    bool m_thumbnailVisible = true;
    float m_size;
    float m_previewHeight = 0;
    FileIconSource m_source;
    SharedFileIcon m_sourceHandle;
    ColorSpec m_foreground = colorSpecFromRole(ColorRole::OnSurface);
    RectNode* m_shadow = nullptr;
    ImageNode* m_image = nullptr;
    Flex* m_linkEmblem = nullptr;
    Flex* m_readOnlyEmblem = nullptr;
    Glyph* m_linkGlyph = nullptr;
    Glyph* m_readOnlyGlyph = nullptr;
    SharedTextureCache::Lease m_texture;
    std::shared_ptr<const LoadedImageFile> m_requestedPixels;
    std::string m_requestedSource;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    std::shared_ptr<const FileIconImage> m_asset;
    std::shared_ptr<const FileIconImage> m_prepared;
  };
} // namespace luciole
