#include "ui/browser/file_icon.h"

#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/rect_node.h"
#include "ui/browser/preview_layout.h"
#include "ui/builders.h"
#include "ui/surface_shadow.h"

#include <algorithm>
#include <cmath>
#include <tuple>

luciole::FileIcon::FileIcon(FileIconCache& cache, float size, ThumbnailService* thumbnails)
    : m_cache(cache), m_thumbnails(thumbnails), m_size(size) {
  m_shadow = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
  m_shadow->setVisible(false);
  m_image = static_cast<ImageNode*>(addChild(std::make_unique<ImageNode>()));
  m_image->setFitMode(ImageFitMode::Contain);
  for (const auto& [badge, glyph, name] :
       {std::tuple{&m_linkEmblem, &m_linkGlyph, "arrow-up-right"},
        std::tuple{&m_readOnlyEmblem, &m_readOnlyGlyph, "lock"}}) {
    addChild(
        ui::row(
            {.out = badge,
             .align = FlexAlign::Center,
             .justify = FlexJustify::Center,
             .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
             .radius = 12.0F,
             .border = colorSpecFromRole(ColorRole::OutlineVariant),
             .visible = false,
             .participatesInLayout = false},
            ui::glyph(
                {.out = glyph, .glyph = name, .glyphSize = 10.0F, .color = colorSpecFromRole(ColorRole::OnSurface)}
            )
        )
    );
  }
  setHitTestVisible(false);
}
luciole::FileIcon::~FileIcon() {
  m_lifetime.reset();
  m_texture.reset();
}
void luciole::FileIcon::bind(const FileIconSource& source, ColorSpec foreground) {
  setEmblems(false, false);
  if (!m_thumbnailSource && source == m_source && foreground == m_foreground) {
    return;
  }
  m_thumbnailSource.reset();
  m_thumbnail.reset();
  m_thumbnailPixels = 0;
  m_source = source;
  m_sourceHandle.reset();
  m_foreground = foreground;
  markLayoutDirty();
}
void luciole::FileIcon::bindFile(const DirectoryEntry& entry, ColorSpec foreground) {
  const bool changed = !m_thumbnailSource
      || entry.id != m_thumbnailSource->id
      || entry.location() != m_thumbnailSource->location()
      || entry.etag() != m_thumbnailSource->etag()
      || entry.size != m_thumbnailSource->size
      || entry.modifiedSeconds != m_thumbnailSource->modifiedSeconds
      || entry.contentRevision != m_thumbnailSource->contentRevision
      || entry.identity() != m_thumbnailSource->identity()
      || entry.type != m_thumbnailSource->type
      || entry.symlink != m_thumbnailSource->symlink
      || entry.contentType != m_thumbnailSource->contentType;
  const bool sameHandle = entry.icon && entry.icon == m_sourceHandle;
  if (!changed && (sameHandle || iconSource(entry.icon) == m_source) && foreground == m_foreground) {
    return;
  }
  if (changed) {
    m_thumbnail.reset();
    m_thumbnailPixels = 0;
    m_thumbnailSource = entry;
  }
  if (!sameHandle) {
    m_source = iconSource(entry.icon);
  }
  m_sourceHandle = entry.icon;
  m_foreground = foreground;
  markLayoutDirty();
}
void luciole::FileIcon::setEmblems(bool link, bool readOnly) {
  if (m_linkEmblem->visible() != link || m_readOnlyEmblem->visible() != readOnly) {
    m_linkEmblem->setVisible(link);
    m_readOnlyEmblem->setVisible(readOnly);
    markLayoutDirty();
  }
}
void luciole::FileIcon::setThumbnailVisible(bool visible) {
  if (m_thumbnailVisible != visible) {
    m_thumbnailVisible = visible;
    if (!visible) {
      m_texture.reset();
      m_asset.reset();
      m_thumbnail.reset();
      m_requestedPixels.reset();
      m_image->setTextureId({});
    }
    markLayoutDirty();
  }
}
void luciole::FileIcon::releaseFile(Renderer&) {
  setEmblems(false, false);
  const bool changed = m_prepared || m_thumbnailSource || m_thumbnail || m_asset || !m_texture.empty();
  m_prepared.reset();
  m_thumbnailSource.reset();
  m_thumbnail.reset();
  m_thumbnailPixels = 0;
  m_asset.reset();
  m_texture.reset();
  m_requestedPixels.reset();
  m_image->setTextureId({});
  if (changed) {
    markLayoutDirty();
  }
}
void luciole::FileIcon::setIconSize(float size) {
  if (m_size != size) {
    m_size = size;
    markLayoutDirty();
  }
}
void luciole::FileIcon::setPreparedImage(std::shared_ptr<const FileIconImage> image) {
  if (m_prepared != image) {
    m_prepared = std::move(image);
    markLayoutDirty();
  }
}
void luciole::FileIcon::setPreviewHeight(float height) {
  if (m_previewHeight != height) {
    m_previewHeight = height;
    markLayoutDirty();
  }
}
LayoutSize luciole::FileIcon::doMeasure(Renderer&, const LayoutConstraints&) {
  return {m_size, m_previewHeight > 0 ? m_previewHeight : m_size};
}
void luciole::FileIcon::doLayout(Renderer& renderer) {
  if (!m_thumbnailVisible) {
    m_texture.reset();
    m_image->setTextureId({});
    return;
  }
  const int pixels = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
  if (m_thumbnails
      && m_thumbnailSource
      && m_thumbnailSource->type == luciole::FileType::Regular
      && !m_thumbnailSource->symlink
      && m_thumbnailSource->modifiedSeconds) {
    if (pixels != m_thumbnailPixels || !m_thumbnails->touch(m_thumbnail, m_thumbnailVisible)) {
      m_thumbnail = m_thumbnails->request(*m_thumbnailSource, pixels, m_thumbnailVisible);
      m_thumbnailPixels = pixels;
    }
  }
  const auto asset = m_prepared && m_prepared->pixels            ? m_prepared
      : m_thumbnailVisible && m_thumbnail && m_thumbnail->pixels ? m_thumbnail
                                                                 : m_cache.request(m_source, pixels);
  m_asset = asset;
  if (asset && asset->pixels) {
    if (asset->pixels != m_requestedPixels) {
      const PreparedTextureRequest request{
          .source = asset->sourceKey, .revision = asset->revision, .mipmap = m_previewHeight > 0
      };
      if (m_requestedSource != request.source) {
        m_texture.reset();
      }
      if (m_texture.empty()) {
        const std::weak_ptr<int> alive = m_lifetime;
        m_texture = m_cache.textures().acquirePrepared(request, asset->pixels, nullptr, [this, alive] {
          if (!alive.expired()) {
            markLayoutDirty();
            m_cache.imageChanged();
          }
        });
      } else {
        (void)m_texture.setPreparedImage(request, asset->pixels);
      }
      m_requestedPixels = asset->pixels;
      m_requestedSource = request.source;
    }
  } else {
    m_texture.reset();
    m_requestedPixels.reset();
  }
  const auto texture = m_texture.texture();
  m_image->setTextureId(texture.id);
  m_image->setTextureSize(texture.width, texture.height);
  const bool symbolic = m_asset && m_asset->symbolic;
  m_image->setAlphaMaskTint(symbolic);
  m_image->setTint(symbolic ? resolveColorSpec(m_foreground) : Color{1, 1, 1, 1});
  // A preview image is placed at its fitted size so its corners can be rounded; icons keep the full box.
  const bool roundPreview = m_previewHeight > 0 && !symbolic && texture.width > 0 && texture.height > 0;
  float imageWidth = width(), imageHeight = height();
  if (texture.width > 0 && texture.height > 0) {
    const float fit = std::min(width() / texture.width, height() / texture.height);
    imageWidth = texture.width * fit;
    imageHeight = texture.height * fit;
  }
  const float left = (width() - imageWidth) * 0.5F;
  const float bottom = (height() + imageHeight) * 0.5F;
  if (roundPreview) {
    m_image->setPosition(left, bottom - imageHeight);
    m_image->setSize(imageWidth, imageHeight);
  } else {
    m_image->setPosition(0, 0);
    m_image->setSize(width(), height());
  }
  m_image->setRadius(roundPreview ? Style::radiusMd : 0.0F);
  m_image->setBorder(
      roundPreview ? resolveColorSpec(colorSpecFromRole(ColorRole::OutlineVariant)) : Color{},
      roundPreview ? Style::borderWidth : 0.0F
  );
  const bool shadow = roundPreview && Style::popupShadowsEnabled();
  m_shadow->setVisible(shadow);
  if (shadow) {
    const auto offset = shadowDirectionOffset(kPreviewShadow.direction);
    m_shadow->setStyle(surface_shadow::style(kPreviewShadow, 1.0F, {.radius = Radii(Style::radiusMd)}));
    m_shadow->setPosition(left + static_cast<float>(offset.x), bottom - imageHeight + static_cast<float>(offset.y));
    m_shadow->setFrameSize(imageWidth, imageHeight);
  }
  const float badgeSize = std::clamp(m_size * 0.3F, 12.0F, 22.0F);
  m_linkGlyph->setGlyphSize(badgeSize * 0.68F);
  m_readOnlyGlyph->setGlyphSize(badgeSize * 0.64F);
  if (m_linkEmblem->visible()) {
    m_linkEmblem->arrange(renderer, {left, bottom - badgeSize, badgeSize, badgeSize});
  }
  if (m_readOnlyEmblem->visible()) {
    m_readOnlyEmblem->arrange(renderer, {left + imageWidth - badgeSize, bottom - badgeSize, badgeSize, badgeSize});
  }
}
void luciole::FileIcon::doInvalidateGpuResources(Renderer&) {
  m_image->setTextureId({});
  markLayoutDirty();
}
