#include "ui/controls/selection_contour.h"

#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cairo.h>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <utility>

namespace {
  struct Vertex {
    std::size_t x, y;
    auto operator<=>(const Vertex&) const = default;
  };
  struct Edge {
    Vertex from, to;
    std::size_t direction;
  };
  struct Point {
    double x, y;
  };

  void contour(
      cairo_t* cr, const std::vector<std::uint8_t>& selected, std::size_t columns, double columnStride,
      double rowStride, double width, double height, double radius
  ) {
    constexpr auto absent = std::numeric_limits<std::size_t>::max();
    std::vector<Edge> edges;
    std::map<Vertex, std::array<std::size_t, 4>> outgoing;
    const auto add = [&](Vertex from, Vertex to, std::size_t direction) {
      auto [it, inserted] = outgoing.try_emplace(from, std::array{absent, absent, absent, absent});
      (void)inserted;
      it->second[direction] = edges.size();
      edges.push_back({from, to, direction});
    };
    const auto rows = selected.size() / columns;
    const auto active = [&](std::size_t x, std::size_t y) {
      return x < columns && y < rows && selected[y * columns + x] != 0;
    };
    for (std::size_t y = 0; y < rows; ++y) {
      for (std::size_t x = 0; x < columns; ++x) {
        if (!active(x, y)) {
          continue;
        }
        // Clockwise edges keep selected content on the right, including holes.
        if (y == 0 || !active(x, y - 1)) {
          add({x, y}, {x + 1, y}, 0);
        }
        if (!active(x + 1, y)) {
          add({x + 1, y}, {x + 1, y + 1}, 1);
        }
        if (!active(x, y + 1)) {
          add({x + 1, y + 1}, {x, y + 1}, 2);
        }
        if (x == 0 || !active(x - 1, y)) {
          add({x, y + 1}, {x, y}, 3);
        }
      }
    }
    const auto point = [&](Vertex v) {
      return Point{
          std::clamp(
              static_cast<double>(v.x) * columnStride - (columnStride * static_cast<double>(columns) - width) * 0.5,
              1.0, width - 1.0
          ),
          std::clamp(
              static_cast<double>(v.y) * rowStride - (rowStride * static_cast<double>(rows) - height) * 0.5, 1.0,
              height - 1.0
          )
      };
    };
    const auto next = [&](const Edge& edge) {
      const auto& choices = outgoing.at(edge.to);
      // Turning left at a diagonal junction produces a rounded neck, not crossing strokes.
      for (auto turn : {3UZ, 0UZ, 1UZ, 2UZ}) {
        const auto candidate = choices[(edge.direction + turn) % 4];
        if (candidate != absent) {
          return candidate;
        }
      }
      return absent;
    };
    std::vector<bool> visited(edges.size(), false);
    for (std::size_t start = 0; start < edges.size(); ++start) {
      if (visited[start]) {
        continue;
      }
      std::vector<Point> vertices;
      auto index = start;
      do {
        visited[index] = true;
        vertices.push_back(point(edges[index].from));
        index = next(edges[index]);
      } while (index != start && index != absent && !visited[index]);
      if (index != start || vertices.size() < 4) {
        continue;
      }
      constexpr double arc = 0.5522847498307936;
      for (std::size_t i = 0; i <= vertices.size(); ++i) {
        const auto current = vertices[i % vertices.size()];
        const auto previous = vertices[(i + vertices.size() - 1) % vertices.size()];
        const auto following = vertices[(i + 1) % vertices.size()];
        const double dx1 = current.x - previous.x, dy1 = current.y - previous.y;
        const double dx2 = following.x - current.x, dy2 = following.y - current.y;
        const double length1 = std::hypot(dx1, dy1), length2 = std::hypot(dx2, dy2);
        const double r = dx1 * dy2 == dy1 * dx2 ? 0 : std::min({radius, length1 * 0.5, length2 * 0.5});
        const Point in{current.x - dx1 * r / length1, current.y - dy1 * r / length1};
        const Point out{current.x + dx2 * r / length2, current.y + dy2 * r / length2};
        if (i == 0) {
          cairo_move_to(cr, out.x, out.y);
        } else {
          cairo_line_to(cr, in.x, in.y);
          cairo_curve_to(
              cr, in.x + dx1 * r * arc / length1, in.y + dy1 * r * arc / length1, out.x - dx2 * r * arc / length2,
              out.y - dy2 * r * arc / length2, out.x, out.y
          );
        }
      }
      cairo_close_path(cr);
    }
  }
  using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
  using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
} // namespace

float SelectionContour::cornerRadius() { return Style::scaledRadius(kBaseCornerRadius); }

SelectionContour::SelectionContour() : m_radius(cornerRadius()) {
  setHitTestVisible(false);
  m_fill = static_cast<ImageNode*>(addChild(std::make_unique<ImageNode>()));
  m_outline = static_cast<ImageNode*>(addChild(std::make_unique<ImageNode>()));
  m_fill->setAlphaMaskTint(true);
  m_fill->setMonochromeTint(true);
  m_outline->setAlphaMaskTint(true);
  m_outline->setMonochromeTint(true);
  m_paletteConn = paletteChanged().connect([this] { markLayoutDirty(); });
  m_radiusConn = Style::cornerRadiusChanged().connect([this] {
    m_radius = cornerRadius();
    m_dirty = true;
    markLayoutDirty();
  });
}
SelectionContour::~SelectionContour() { releaseTextures(); }
void SelectionContour::releaseTextures() {
  if (m_textures != nullptr) {
    if (m_fillTexture.valid()) {
      m_textures->unload(m_fillTexture);
    }
    if (m_outlineTexture.valid()) {
      m_textures->unload(m_outlineTexture);
    }
  }
}
void SelectionContour::bind(
    std::vector<std::uint8_t> selected, std::size_t columns, float columnStride, float rowStride
) {
  if (selected != m_selected || columns != m_columns || columnStride != m_columnStride || rowStride != m_rowStride) {
    m_selected = std::move(selected);
    m_columns = columns;
    m_columnStride = columnStride;
    m_rowStride = rowStride;
    m_dirty = true;
    markLayoutDirty();
  }
}
void SelectionContour::doLayout(Renderer& renderer) {
  // A calm tinted block; the outline matches the fill so merged selections read as one shape.
  m_fill->setTint(resolveColorSpec(colorSpecFromRole(ColorRole::PrimaryContainer)));
  m_outline->setTint(resolveColorSpec(colorSpecFromRole(ColorRole::PrimaryContainer)));
  if (!visible()) {
    return;
  }
  if (width() <= 2 || height() <= 2 || m_columns == 0) {
    return;
  }
  // Keep mask memory bounded even on unusually large or highly scaled surfaces.
  const float scale = std::min(renderer.renderScale(), 4096.0F / std::max(width(), height()));
  if (m_dirty || width() != m_rasterWidth || height() != m_rasterHeight || scale != m_rasterScale) {
    m_textures = &renderer.textureManager();
    const int pixelWidth = (static_cast<int>(std::ceil(width() * scale)) + 3) & ~3;
    const int pixelHeight = static_cast<int>(std::ceil(height() * scale));
    Surface fill(cairo_image_surface_create(CAIRO_FORMAT_A8, pixelWidth, pixelHeight), &cairo_surface_destroy);
    Surface outline(cairo_image_surface_create(CAIRO_FORMAT_A8, pixelWidth, pixelHeight), &cairo_surface_destroy);
    Context fillContext(cairo_create(fill.get()), &cairo_destroy);
    Context outlineContext(cairo_create(outline.get()), &cairo_destroy);
    cairo_scale(fillContext.get(), scale, scale);
    cairo_set_source_rgba(fillContext.get(), 1, 1, 1, 1);
    contour(fillContext.get(), m_selected, m_columns, m_columnStride, m_rowStride, width(), height(), m_radius);
    const std::unique_ptr<cairo_path_t, decltype(&cairo_path_destroy)> path(
        cairo_copy_path(fillContext.get()), &cairo_path_destroy
    );
    cairo_fill(fillContext.get());
    cairo_scale(outlineContext.get(), scale, scale);
    cairo_set_source_rgba(outlineContext.get(), 1, 1, 1, 1);
    cairo_append_path(outlineContext.get(), path.get());
    cairo_set_line_width(outlineContext.get(), Style::focusRingWidth);
    cairo_stroke(outlineContext.get());
    cairo_surface_flush(fill.get());
    cairo_surface_flush(outline.get());
    releaseTextures();
    m_fillTexture = m_textures->loadFromPixels(
        cairo_image_surface_get_data(fill.get()), pixelWidth, pixelHeight, TextureDataFormat::Alpha
    );
    m_outlineTexture = m_textures->loadFromPixels(
        cairo_image_surface_get_data(outline.get()), pixelWidth, pixelHeight, TextureDataFormat::Alpha
    );
    m_fill->setTextureId(m_fillTexture.id);
    m_outline->setTextureId(m_outlineTexture.id);
    m_fill->setTextureSize(pixelWidth, pixelHeight);
    m_outline->setTextureSize(pixelWidth, pixelHeight);
    m_rasterWidth = width();
    m_rasterHeight = height();
    m_rasterScale = scale;
    m_dirty = false;
  }
  const float imageWidth = std::ceil(width() * scale / 4) * 4 / scale;
  const float imageHeight = std::ceil(height() * scale) / scale;
  m_fill->arrange(renderer, {0, 0, imageWidth, imageHeight});
  m_outline->arrange(renderer, {0, 0, imageWidth, imageHeight});
}
void SelectionContour::doInvalidateGpuResources(Renderer&) {
  m_fillTexture = {};
  m_outlineTexture = {};
  m_fill->setTextureId({});
  m_outline->setTextureId({});
  m_dirty = true;
  markLayoutDirty();
}
