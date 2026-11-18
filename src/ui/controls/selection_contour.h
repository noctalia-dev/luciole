#pragma once

#include "render/core/texture_handle.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class ImageNode;
class TextureManager;

// One rounded contour for the visible selection, rasterized only during Layout.
class SelectionContour final : public Node {
public:
  // The selection corner at the default roundness; cornerRadius() applies the corner roundness setting.
  static constexpr float kBaseCornerRadius = 7.0F;
  static float cornerRadius();
  SelectionContour();
  ~SelectionContour() override;
  void bind(std::vector<std::uint8_t> selected, std::size_t columns, float columnStride, float rowStride);

protected:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer&) override;

private:
  void releaseTextures();
  std::vector<std::uint8_t> m_selected;
  std::size_t m_columns = 1;
  float m_columnStride = 0, m_rowStride = 0;
  // Tracks the corner roundness setting alongside the cell backgrounds built from this contour.
  float m_radius;
  float m_rasterWidth = 0, m_rasterHeight = 0, m_rasterScale = 0;
  bool m_dirty = true;
  ImageNode* m_fill = nullptr;
  ImageNode* m_outline = nullptr;
  TextureManager* m_textures = nullptr;
  TextureHandle m_fillTexture{}, m_outlineTexture{};
  Signal<>::ScopedConnection m_paletteConn;
  Signal<>::ScopedConnection m_radiusConn;
};
