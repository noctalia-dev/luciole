#include "ui/controls/miller_view.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ui/browser/file_view_metrics.h"
#include "ui/builders.h"

#include <cmath>

MillerView::MillerView(
    luciole::MillerColumns& columns, luciole::FileIconCache& icons, luciole::ThumbnailService& thumbnails
)
    : m_columns(columns), m_icons(icons), m_thumbnails(thumbnails) {
  setFlexGrow(1);
  setClipChildren(true);
  auto viewport = ui::box({.out = &m_viewport, .fill = clearColorSpec(), .participatesInLayout = false});
  viewport->setClipChildren(true);
  auto wheel = std::make_unique<InputArea>();
  wheel->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  m_wheel = wheel.get();
  wheel->setOnPress([this](const InputArea::PointerData& data) {
    if (!data.pressed || !m_columns.enabled()) {
      return;
    }
    if (data.button == BTN_RIGHT && context) {
      context(std::nullopt, data.sceneX, data.sceneY);
    } else if (data.button == BTN_LEFT) {
      m_columns.active().model().clearSelection();
    }
  });
  wheel->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL
        && (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !modifiers || modifiers() != KeyMod::Shift)) {
      return false;
    }
    scroll(m_offset + data.scrollDelta(Style::scrollWheelStep));
    return true;
  });
  wheel->setPropagateEvents(true);
  viewport->addChild(std::move(wheel));
  addChild(std::move(viewport));
  auto scrollbar = std::make_unique<Scrollbar>();
  m_scrollbar = scrollbar.get();
  scrollbar->setParticipatesInLayout(false);
  scrollbar->setOrientation(ScrollOrientation::Horizontal);
  scrollbar->setOnScrollChanged([this](float value) { scroll(value); });
  addChild(std::move(scrollbar));
}
MillerView::~MillerView() {
  for (auto& [_, column] : m_views) {
    (void)m_viewport->removeChild(column.root);
  }
  m_views.clear();
}
std::size_t MillerView::indexOf(std::uint64_t id) const {
  const auto& columns = m_columns.columns();
  const auto found = std::ranges::find_if(columns, [id](const auto& column) { return column->id == id; });
  return static_cast<std::size_t>(found - columns.begin());
}
VirtualGridView* MillerView::view(std::size_t index) const {
  if (index >= m_columns.columns().size()) {
    return nullptr;
  }
  const auto found = m_views.find(m_columns.columns()[index]->id);
  return found == m_views.end() ? nullptr : found->second.grid;
}
VirtualGridView* MillerView::focusedView() const { return view(m_columns.focused()); }
std::optional<std::size_t> MillerView::columnAt(float x, float y) const {
  if (!visible() || !m_viewport->containsScenePoint(x, y)) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < m_columns.columns().size(); ++i) {
    const auto found = m_views.find(m_columns.columns()[i]->id);
    if (found != m_views.end()
        && found->second.root->visible()
        && found->second.grid->visible()
        && found->second.grid->containsScenePoint(x, y)) {
      return i;
    }
  }
  return std::nullopt;
}
void MillerView::scroll(float offset) {
  m_offset = std::clamp(offset, 0.0F, std::max(0.0F, m_extent - width()));
  markLayoutDirty();
}
void MillerView::revealFocused() {
  m_reveal = true;
  m_revealColumn = m_columns.focused();
  markLayoutDirty();
}
void MillerView::suspend() {
  for (auto& [_, column] : m_views) {
    column.grid->setAdapter(nullptr);
  }
}
void MillerView::loadVisible() {
  for (const auto& [id, view] : m_views) {
    if (view.root->visible()) {
      m_columns.load(indexOf(id));
    }
  }
}
void MillerView::synchronize() {
  for (auto it = m_views.begin(); it != m_views.end();) {
    if (indexOf(it->first) == m_columns.columns().size()) {
      (void)m_viewport->removeChild(it->second.root);
      it = m_views.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& source : m_columns.columns()) {
    const auto id = source->id;
    if (m_views.contains(id)) {
      continue;
    }
    auto adapter = std::make_unique<luciole::FileViewAdapter>(
        *source->browser, m_icons, m_thumbnails, luciole::ViewMode::Miller,
        [this] { return modifiers ? modifiers() : 0U; },
        [this, id] { return indexOf(id) == m_columns.focused() && keyboardFocus && keyboardFocus(); }
    );
    adapter->setCutPredicate([this](const luciole::Location& location) { return cut && cut(location); });
    adapter->setActivateCallback([this, id](luciole::EntryId entry) {
      if (focus) {
        focus(indexOf(id));
      }
      if (activate) {
        activate(entry);
      }
    });
    adapter->setContextCallback([this, id](std::optional<luciole::EntryId> entry, float x, float y) {
      if (focus) {
        focus(indexOf(id));
      }
      if (context) {
        context(entry, x, y);
      }
    });
    adapter->setDragCallback(
        [this](const auto& ids) {
          if (drag) {
            drag(ids);
          }
        },
        [this] {
          if (cancelDrag) {
            cancelDrag();
          }
        }
    );
    VirtualGridView* grid = nullptr;
    Label* notice = nullptr;
    auto root = ui::column(
        {.align = FlexAlign::Stretch, .clipChildren = true, .visible = false, .participatesInLayout = false},
        ui::virtualGridView(
            {.out = &grid,
             .columns = 1,
             .cellHeight = 36,
             .squareCells = false,
             .columnGap = 0,
             .rowGap = 0,
             .adapter = adapter.get(),
             .flexGrow = 1}
        ),
        ui::label(
            {.out = &notice,
             .fontSize = Style::fontSizeCaption,
             .maxLines = 3,
             .textAlign = TextAlign::Center,
             .visible = false,
             .participatesInLayout = false}
        )
    );
    grid->scrollView().setReserveScrollbarGutter(true);
    grid->scrollView().setViewportPaddingH(4);
    grid->scrollView().setViewportPaddingV(4);
    grid->focusArea()->setRetainsFocusOnPointerRelease(true);
    grid->focusArea()->setTextInputClient(textInputClient);
    grid->focusArea()->setOnFocusGain([this, id] {
      if (focus) {
        focus(indexOf(id));
      }
    });
    source->browser->setCaptureViewport([grid, controller = source->browser.get()] {
      const auto first = grid->firstVisibleIndex();
      const auto& rows = controller->model().rows();
      return luciole::BrowseViewport{
          .mode = luciole::ViewMode::Miller,
          .iconSize = controller->viewport().iconSize,
          .vertical = grid->scrollView().scrollOffset(),
          .withinRow = grid->firstVisibleOffset(),
          .first = first && *first < rows.size() ? std::optional(rows[*first]) : std::nullopt
      };
    });
    auto divider = std::make_unique<InputArea>();
    auto* handle = divider.get();
    handle->setZIndex(5);
    handle->setFocusable(true);
    handle->setTabStop(true);
    handle->setTooltip("Resize column; Left/Right adjusts, Home resets");
    handle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
    handle->setOnPress([this, id](const InputArea::PointerData& data) {
      const auto index = indexOf(id);
      if (data.button != BTN_LEFT || index >= m_columns.columns().size()) {
        return;
      }
      if (data.pressed) {
        m_resizing = id;
        m_resizeStart = data.sceneX;
        auto& column = *m_columns.columns()[index];
        m_resizePreferredWidth = column.width;
        m_resizeFixedWidth = column.fixedWidth;
        m_resizeWidth = m_views.at(id).root->width();
        column.width = m_resizeWidth;
        column.fixedWidth = true;
        markLayoutDirty();
      } else if (m_resizing == id) {
        m_resizing.reset();
        auto& column = *m_columns.columns()[index];
        if (std::abs(column.width - m_resizeWidth) < 0.5F) {
          column.width = m_resizePreferredWidth;
          column.fixedWidth = m_resizeFixedWidth;
        } else if (resized) {
          resized(static_cast<int>(std::lround(column.width)));
        }
        markLayoutDirty();
      }
    });
    handle->setOnMotion([this, id, handle](const InputArea::PointerData& data) {
      const auto index = indexOf(id);
      if (m_resizing != id || index >= m_columns.columns().size()) {
        return;
      }
      float x = 0, y = 0;
      Node::absolutePosition(handle, x, y);
      m_columns.columns()[index]->width =
          std::clamp(m_resizeWidth + (x + data.localX - m_resizeStart) * (Style::rtl() ? -1 : 1), 180.0F, 800.0F);
      markLayoutDirty();
    });
    handle->setOnCancel([this, id] {
      const auto index = indexOf(id);
      if (m_resizing == id && index < m_columns.columns().size()) {
        m_columns.columns()[index]->width = m_resizePreferredWidth;
        m_columns.columns()[index]->fixedWidth = m_resizeFixedWidth;
      }
      m_resizing.reset();
      markLayoutDirty();
    });
    handle->setOnKeyDown([this, id](const InputArea::KeyData& key) {
      const auto index = indexOf(id);
      if (index >= m_columns.columns().size() || key.modifiers != 0) {
        return;
      }
      auto& column = *m_columns.columns()[index];
      auto& value = column.width;
      if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
        value = std::clamp(
            m_views.at(id).root->width() + ((KeySymbol::isRight(key.sym) != Style::rtl()) ? 16.0F : -16.0F), 180.0F,
            800.0F
        );
        column.fixedWidth = true;
      } else if (KeySymbol::isHome(key.sym)) {
        value = 260;
        column.fixedWidth = false;
      } else {
        return;
      }
      if (resized) {
        resized(static_cast<int>(std::lround(value)));
      }
      markLayoutDirty();
    });
    Box* line = nullptr;
    handle->addChild(ui::box({.out = &line, .fill = colorSpecFromRole(ColorRole::OutlineVariant)}));
    root->addChild(std::move(divider));
    auto* raw = static_cast<Flex*>(m_viewport->addChild(std::move(root)));
    m_views.emplace(id, ColumnView{std::move(adapter), raw, grid, notice, handle, line});
    m_reveal = true;
    m_revealColumn = m_columns.columns().size() - 1;
  }
  for (std::size_t i = 0; i < m_columns.columns().size(); ++i) {
    const auto& column = *m_columns.columns()[i];
    auto& view = m_views.at(column.id);
    const auto& model = column.browser->model();
    const auto& snapshot = model.snapshot();
    view.adapter->setPresentation(presentation, snapshot.metadata.filesystem.readOnly);
    const bool context = !column.browser->searchQuery().empty() || (snapshot.location && snapshot.location->isTrash());
    const float rowHeight = luciole::fileRowHeight(presentation, context, luciole::millerRowIconSize(presentation));
    if (rowHeight != view.rowHeight) {
      const auto anchor = view.grid->firstVisibleIndex();
      const float offset = view.grid->firstVisibleOffset();
      view.grid->cancelPointerGesture();
      view.adapter->onPointerCancel();
      view.grid->setCellHeight(rowHeight);
      view.rowHeight = rowHeight;
      if (anchor) {
        view.grid->scrollToIndex(*anchor, true, std::min(offset, rowHeight - 1));
      }
    }
    view.adapter->setActivationMode(activationMode);
    view.adapter->setColumnActive(i == m_columns.focused());
    std::optional<luciole::EntryId> path;
    if (i + 1 < m_columns.columns().size() && model.selection().selected.size() == 1) {
      const auto id = *model.selection().selected.begin();
      if (model.entry(id) && model.entry(id)->location() == m_columns.columns()[i + 1]->location) {
        path = id;
      }
    }
    view.adapter->setPathEntry(path);
    if (view.path != path) {
      view.path = path;
      if (path) {
        const auto found = std::ranges::find(model.rows(), *path);
        if (found != model.rows().end()) {
          view.grid->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
        }
      }
    }
    view.grid->notifyDataChanged();
  }
  m_columns.collectRetired();
  markLayoutDirty();
}
LayoutSize MillerView::doMeasure(Renderer&, const LayoutConstraints& constraints) {
  return constraints.constrain(
      {constraints.hasMaxWidth ? constraints.maxWidth : 0, constraints.hasMaxHeight ? constraints.maxHeight : 0}
  );
}
void MillerView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }
void MillerView::doLayout(Renderer& renderer) {
  const auto& columns = m_columns.columns();
  std::vector<float> widths;
  widths.reserve(columns.size());
  const float automaticLimit = std::min(width(), std::max(160.0F, width() * 0.5F));
  std::size_t automatic = 0;
  m_extent = 0;
  for (const auto& column : columns) {
    const float span = column->fixedWidth ? column->width : std::min(column->width, automaticLimit);
    widths.push_back(span);
    m_extent += span;
    automatic += column->fixedWidth ? 0U : 1U;
  }
  const float spare = m_columns.enabled() ? std::max(0.0F, width() - m_extent) : 0;
  for (std::size_t i = 0; i < widths.size(); ++i) {
    if (automatic > 0 && !columns[i]->fixedWidth) {
      widths[i] += spare / static_cast<float>(automatic);
    } else if (automatic == 0 && i + 1 == widths.size()) {
      widths[i] += spare;
    }
  }
  m_extent += spare;
  if (m_layoutWidth != width()) {
    m_layoutWidth = width();
    m_reveal = true;
  }
  const float maximum = std::max(0.0F, m_extent - width());
  m_offset = std::clamp(m_offset, 0.0F, maximum);
  const float gutter = maximum > 0 ? Style::scrollbarWidth + Style::scrollbarGap : 0;
  const float h = std::max(0.0F, height() - gutter);
  if (std::exchange(m_reveal, false) && m_columns.enabled()) {
    const auto target = std::min(m_revealColumn.value_or(m_columns.focused()), columns.size() - 1);
    std::size_t first = target;
    std::size_t last = target;
    if (target == m_columns.focused() && target + 1 < columns.size()) {
      ++last;
    } else if (target > 0) {
      --first;
    }
    float left = 0;
    for (std::size_t i = 0; i < target; ++i) {
      left += widths[i];
    }
    float span = widths[target];
    float contextSpan = span;
    if (first != target) {
      contextSpan += widths[first];
    } else if (last != target) {
      contextSpan += widths[last];
    }
    if (contextSpan <= width() + 0.5F) {
      if (first != target) {
        left -= widths[first];
      }
      span = contextSpan;
    }
    m_revealColumn.reset();
    if (Style::rtl()) {
      left = m_extent - left - span;
    }
    if (left < m_offset) {
      m_offset = left;
    } else if (left + span > m_offset + width()) {
      m_offset = left + std::min(span, width()) - width();
    }
    m_offset = std::clamp(m_offset, 0.0F, maximum);
  }
  m_viewport->setFrameSize(width(), h);
  m_wheel->setFrameSize(width(), h);
  float start = 0;
  bool loadPending = false;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const auto& column = *columns[i];
    const bool last = i + 1 == columns.size();
    const float columnWidth = widths[i];
    auto& view = m_views.at(column.id);
    const float x = (Style::rtl() ? m_extent - start - columnWidth : start) - m_offset;
    const bool visible = x < width() && x + columnWidth > 0;
    view.root->setVisible(visible);
    view.grid->setAdapter(visible ? view.adapter.get() : nullptr);
    if (!visible) {
      start += columnWidth;
      continue;
    }
    const auto& model = column.browser->model();
    view.adapter->setDetailsColumns(
        {.showType = false, .showSize = false, .showModified = false, .nameWidth = columnWidth - 16}
    );
    view.root->setPosition(x, 0);
    view.root->setFrameSize(columnWidth, h);
    view.grid->arrange(renderer, {0, 0, columnWidth, h});
    const auto& snapshot = model.snapshot();
    loadPending = loadPending || snapshot.status == luciole::DirectoryStatus::Idle;
    const bool empty = model.rows().empty();
    view.notice->setText(
        snapshot.status == luciole::DirectoryStatus::Loading || snapshot.status == luciole::DirectoryStatus::Idle
            ? "Loading…"
            : snapshot.error ? snapshot.error->message
                             : "This folder is empty"
    );
    view.notice->setVisible(empty);
    if (empty) {
      view.notice->arrange(renderer, {12, 28, std::max(0.0F, columnWidth - 24), 100});
    }
    view.line->setFill(colorSpecFromRole(
        view.divider->hovered() || view.divider->focused() || m_resizing == column.id ? ColorRole::Primary
                                                                                      : ColorRole::OutlineVariant
    ));
    view.divider->setVisible(!last || columnWidth <= column.width + 0.5F || m_resizing == column.id);
    view.divider->arrange(renderer, {Style::rtl() ? 0.0F : columnWidth - 6, 0, 6, h});
    view.line->arrange(renderer, {2, 0, 1, h});
    start += columnWidth;
  }
  m_scrollbar->setVisible(maximum > 0);
  m_scrollbar->update(width(), m_extent, m_offset);
  m_scrollbar->arrange(renderer, {0, h + Style::scrollbarGap, width(), Style::scrollbarWidth});
  if (loadPending && requestUpdate) {
    requestUpdate();
  }
}
