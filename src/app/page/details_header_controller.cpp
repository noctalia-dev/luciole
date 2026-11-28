#include "app/page/details_header_controller.h"

#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/input/key_symbols.h"
#include "cursor-shape-v1-client-protocol.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_view.h"
#include "ui/builders.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

using Action = luciole::BrowserAction;

DetailsHeaderController::DetailsHeaderController(
    PageContext& page, luciole::ViewPreferenceWriter& preferences, luciole::FileViewAdapter& listAdapter,
    luciole::FileViewAdapter& gridAdapter, MenuController& menus
)
    : m_page(page), m_preferences(preferences), m_listAdapter(listAdapter), m_gridAdapter(gridAdapter), m_menus(menus) {
}

void DetailsHeaderController::load(const luciole::BrowserSettings& settings) {
  m_columnVisibility = settings.columns;
  m_columnOrder = settings.columnOrder;
  m_columnWidths = settings.columnWidths;
}

void DetailsHeaderController::applySettings(
    const luciole::BrowserSettings& before, const luciole::BrowserSettings& settings, bool force
) {
  if (force || before.columns != settings.columns) {
    m_columnVisibility = settings.columns;
  }
  if (force || before.columnOrder != settings.columnOrder) {
    m_columnOrder = settings.columnOrder;
  }
  if (force || before.columnWidths != settings.columnWidths) {
    m_columnWidths = settings.columnWidths;
  }
}

std::unique_ptr<Flex> DetailsHeaderController::buildHeader() {
  m_appliedColumnOrder = luciole::kDefaultDetailsColumnOrder;
  auto row = ui::row(
      {.out = &m_detailsHeaderRow, .align = FlexAlign::Center, .paddingH = 0.0F, .participatesInLayout = false}
  );
  row->addChild(
      ui::button(
          {.out = &m_nameHeader,
           .text = "Name",
           .controlHeight = Style::controlHeightSm,
           .contentAlign = ButtonContentAlign::Start,
           .variant = ButtonVariant::Ghost,
           .tooltip = "Sort by name; click again to reverse",
           .paddingH = Style::spaceSm,
           .onClick = [this] { sortBy(luciole::SortField::Name); }}
      )
  );
  for (const auto column : luciole::kDefaultDetailsColumnOrder) {
    const auto index = static_cast<std::size_t>(column);
    row->addChild(
        ui::button(
            {.out = &m_headers[index],
             .text = std::string(luciole::detailsColumnLabel(column)),
             .controlHeight = Style::controlHeightSm,
             .contentAlign = ButtonContentAlign::Start,
             .variant = ButtonVariant::Ghost,
             .tooltip = std::format("Sort by {}; click again to reverse", luciole::detailsColumnName(column)),
             .paddingH = Style::spaceSm,
             .onClick = [this, column] { sortBy(luciole::detailsColumnSort(column)); }}
        )
    );
  }
  return ui::column(
      {.out = &m_listHeader, .align = FlexAlign::Stretch},
      ui::row(
          {.align = FlexAlign::Center,
           .paddingV = Style::radiusMd,
           .height = Style::controlHeightSm + 2.0F * Style::radiusMd},
          ui::row(
              {.out = &m_detailsHeaderViewport,
               .align = FlexAlign::Stretch,
               .clipChildren = true,
               .height = 32.0F,
               .flexGrow = 1.0F},
              std::move(row)
          ),
          ui::box({.width = Style::scrollbarWidth + Style::scrollbarGap})
      ),
      ui::separator({})
  );
}

std::unique_ptr<Box> DetailsHeaderController::buildScrollbar() {
  auto horizontalBar = std::make_unique<Scrollbar>();
  horizontalBar->setOrientation(ScrollOrientation::Horizontal);
  horizontalBar->setParticipatesInLayout(false);
  horizontalBar->setOnScrollChanged([this](float offset) { scrollDetails(offset); });
  m_detailsScrollbar = horizontalBar.get();
  auto horizontalHost = ui::box(
      {.out = &m_detailsScrollHost,
       .fill = clearColorSpec(),
       .height = Style::scrollbarWidth + Style::scrollbarGap,
       .visible = false,
       .participatesInLayout = false}
  );
  horizontalHost->addChild(std::move(horizontalBar));
  return horizontalHost;
}

void DetailsHeaderController::attach(Node& root) {
  for (std::size_t index = 0; index < m_columnWidths.size(); ++index) {
    auto handle = std::make_unique<InputArea>();
    handle->setParticipatesInLayout(false);
    handle->setZIndex(10);
    handle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
    handle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    handle->setOnPress([this, index](const InputArea::PointerData& data) {
      if (data.pressed) {
        m_page.captureViewAnchor();
        m_page.fileList()->cancelPointerGesture();
        m_columnResize =
            ColumnResize{index,          data.sceneX,     headerWidth(index),
                         m_columnWidths, m_detailsOffset, static_cast<int>(std::lround(m_detailsColumns.nameWidth))};
      } else if (m_columnResize) {
        const auto before = m_columnResize->before;
        m_columnResize.reset();
        if (m_columnWidths != before) {
          m_preferences.saveColumnWidths(m_columnWidths);
        }
        m_columnHoverDirty = true;
      }
      m_page.surface().requestLayout();
    });
    handle->setOnMotion([this](const InputArea::PointerData&) {
      if (!m_columnResize) {
        return;
      }
      const auto& resize = *m_columnResize;
      m_columnWidths = resize.before;
      const int allocation = std::clamp(
          static_cast<int>(std::lround(resize.width + (m_page.pointerX() - resize.pressX) * (Style::rtl() ? -1 : 1))),
          luciole::minimumDetailsColumnWidth(resize.index), luciole::maximumDetailsColumnWidth(resize.index)
      );
      if (allocation != static_cast<int>(std::lround(resize.width))) {
        m_columnWidths[resize.index] = allocation;
        if (resize.index != 0 && m_columnWidths[0] == 0) {
          m_columnWidths[0] = resize.nameWidth;
        }
      }
      m_page.surface().requestLayout();
    });
    handle->setOnCancel([this] { cancelColumnResize(); });
    handle->setOnEnter([this](const auto&) { m_page.surface().requestLayout(); });
    handle->setOnLeave([this] { m_page.surface().requestLayout(); });
    handle->addChild(
        ui::separator(
            {.out = &m_columnResizeLines[index],
             .orientation = SeparatorOrientation::VerticalRule,
             .participatesInLayout = false,
             .configure = [](Separator& line) { line.setHitTestVisible(false); }}
        )
    );
    m_columnResizeHandles[index] = static_cast<InputArea*>(m_detailsHeaderViewport->addChild(std::move(handle)));
  }
  for (const auto column : m_columnOrder) {
    auto* header = columnHeader(column);
    header->setOnPress([this, column, header](float, float, bool pressed) {
      if (pressed) {
        m_columnDrag = ColumnDrag{column, m_page.pointerX(), m_page.pointerY(), header->inputArea()->cursorShape()};
      } else {
        cancelColumnDrag();
      }
    });
    header->setOnMotion([this] { updateColumnDrag(); });
    header->inputArea()->setOnCancel([this] { cancelColumnDrag(); });
  }
  root.addChild(
      ui::box(
          {.out = &m_columnDropLine,
           .fill = colorSpecFromRole(ColorRole::Primary),
           .visible = false,
           .participatesInLayout = false}
      )
  );
  m_columnDropLine->setZIndex(102);
  m_columnDropLine->setHitTestVisible(false);
}

void DetailsHeaderController::focusChanged(InputArea* focused) {
  if (m_nameHeader == nullptr || m_page.viewMode() != luciole::ViewMode::List) {
    return;
  }
  for (std::size_t index = 0; index < m_columnWidths.size(); ++index) {
    if (widthHeader(index)->inputArea() == focused) {
      m_revealDetailsHeader = widthHeader(index);
      m_page.surface().requestLayout();
    }
  }
}

void DetailsHeaderController::sync(const luciole::SortOptions& sort) {
  m_listAdapter.setDetailsColumns(m_detailsColumns);
  m_listAdapter.setDetailsColumnOrder(m_columnOrder);
  const char* indicator = sort.descending ? "arrow-down" : "arrow-up";
  m_nameHeader->setGlyph(indicator);
  m_nameHeader->glyph()->setVisible(sort.field == luciole::SortField::Name);
  m_nameHeader->glyph()->setParticipatesInLayout(sort.field == luciole::SortField::Name);
  for (const auto column : luciole::kDefaultDetailsColumnOrder) {
    auto* header = columnHeader(column);
    header->setGlyph(indicator);
    const bool active = sort.field == luciole::detailsColumnSort(column);
    header->glyph()->setVisible(active);
    header->glyph()->setParticipatesInLayout(active);
  }
  // An empty list shows only the empty-state notice; headers there would sort nothing.
  const bool shown = m_page.viewMode() == luciole::ViewMode::List && !m_page.browser().model().rows().empty();
  m_listHeader->setVisible(shown);
  m_listHeader->setParticipatesInLayout(shown);
}

bool DetailsHeaderController::applyOrder() {
  if (m_columnOrder == m_appliedColumnOrder) {
    return false;
  }
  for (const auto column : m_columnOrder) {
    auto* header = columnHeader(column);
    m_detailsHeaderRow->addChild(m_detailsHeaderRow->removeChild(header));
  }
  m_appliedColumnOrder = m_columnOrder;
  return true;
}

bool DetailsHeaderController::release(const PointerEvent& event, float x, float y) {
  if (m_columnDrag
      && m_columnDrag->active
      && event.type == PointerEvent::Type::Button
      && event.button == BTN_LEFT
      && !event.pressed) {
    finishColumnDrag(x, y);
    return true;
  }
  return false;
}

bool DetailsHeaderController::contextPress(const PointerEvent& event, float x, float y) {
  if (event.type == PointerEvent::Type::Button
      && event.pressed
      && event.button == BTN_RIGHT
      && m_listHeader->visible()
      && m_listHeader->containsScenePoint(x, y)) {
    std::optional<luciole::DetailsColumn> column;
    for (const auto candidate : m_columnOrder) {
      auto* header = columnHeader(candidate);
      if (header->visible() && header->containsScenePoint(x, y)) {
        column = candidate;
      }
    }
    m_menus.openColumnsMenu({x, y, 1, 1}, column);
    return true;
  }
  return false;
}

void DetailsHeaderController::scrollWheel(const PointerEvent& event) {
  if (!m_columnResize && !m_columnDrag) {
    InputArea::PointerData data;
    data.axisSource = event.axisSource;
    data.axisValue = event.axisValue;
    data.axisLines = event.axisLines;
    scrollDetails(m_detailsOffset + data.scrollDelta(Style::scrollWheelStep));
  }
}

bool DetailsHeaderController::headerContains(float x, float y) const {
  return m_listHeader->visible() && m_listHeader->containsScenePoint(x, y);
}

bool DetailsHeaderController::widthKey(const KeyboardEvent& event) {
  if (m_page.viewMode() == luciole::ViewMode::List
      && event.modifiers == KeyMod::Ctrl
      && (event.sym == XKB_KEY_Left || event.sym == XKB_KEY_Right)) {
    for (std::size_t index = 0; index < m_columnWidths.size(); ++index) {
      if (m_page.dispatcher().focusedArea() == widthHeader(index)->inputArea()) {
        auto widths = m_columnWidths;
        const int delta = (event.sym == XKB_KEY_Right ? 16 : -16) * (Style::rtl() ? -1 : 1);
        widths[index] = std::clamp(
            static_cast<int>(std::lround(headerWidth(index))) + delta, luciole::minimumDetailsColumnWidth(index),
            luciole::maximumDetailsColumnWidth(index)
        );
        if (index != 0 && widths[0] == 0 && widths[index] != m_columnWidths[index]) {
          widths[0] = static_cast<int>(std::lround(m_detailsColumns.nameWidth));
        }
        setDetailsColumnWidths(widths);
        m_revealDetailsHeader = widthHeader(index);
        return true;
      }
    }
  }
  return false;
}

bool DetailsHeaderController::menuKey(const KeyboardEvent& event) {
  if (luciole::keybindMatches(luciole::KeybindCommand::ContextMenu, event.sym, event.modifiers)) {
    for (std::size_t index = 0; index < m_columnWidths.size(); ++index) {
      auto* header = widthHeader(index);
      if (m_page.dispatcher().focusedArea() == header->inputArea()) {
        std::optional<luciole::DetailsColumn> column;
        for (const auto candidate : m_columnOrder) {
          if (columnHeader(candidate) == header) {
            column = candidate;
          }
        }
        auto anchor = sceneRect(*header);
        const auto viewport = sceneRect(*m_detailsHeaderViewport);
        const float left = std::max(anchor.x, viewport.x);
        anchor.width = std::max(1.0F, std::min(anchor.x + anchor.width, viewport.x + viewport.width) - left);
        anchor.x = left;
        m_menus.openColumnsMenu(anchor, column);
        return true;
      }
    }
  }
  return false;
}

void DetailsHeaderController::setDetailsColumnVisibility(luciole::DetailsColumnVisibility visibility) {
  if (visibility == m_columnVisibility) {
    return;
  }
  m_page.captureViewAnchor();
  m_page.dispatcher().cancelPointerCapture();
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  m_columnVisibility = visibility;
  m_preferences.saveDetailsColumns(visibility);
  m_page.requestSync();
}

Button* DetailsHeaderController::widthHeader(std::size_t index) const {
  return index == 0 ? m_nameHeader : columnHeader(static_cast<luciole::DetailsColumn>(index - 1));
}

float DetailsHeaderController::headerWidth(std::size_t index) const { return widthHeader(index)->width(); }

void DetailsHeaderController::cancelColumnResize() {
  if (!m_columnResize) {
    return;
  }
  m_columnWidths = m_columnResize->before;
  m_detailsOffset = m_columnResize->offset;
  m_columnResize.reset();
  m_columnHoverDirty = true;
  m_page.surface().requestLayout();
}

void DetailsHeaderController::setDetailsColumnWidths(luciole::DetailsColumnWidths widths) {
  if (m_columnWidths == widths) {
    return;
  }
  m_page.captureViewAnchor();
  m_page.dispatcher().cancelPointerCapture();
  m_page.fileList()->cancelPointerGesture();
  m_columnWidths = widths;
  m_preferences.saveColumnWidths(widths);
  m_page.surface().requestLayout();
}

void DetailsHeaderController::scrollDetails(float offset) {
  if (m_page.viewMode() != luciole::ViewMode::List || m_columnResize || m_columnDrag) {
    return;
  }
  const float maximum = std::max(0.0F, m_detailsColumns.contentWidth - m_detailsHeaderViewport->width());
  const float next = std::clamp(offset, 0.0F, maximum);
  if (next == m_detailsOffset) {
    return;
  }
  m_page.fileList()->cancelPointerGesture();
  m_detailsOffset = next;
  m_columnHoverDirty = true;
  m_page.surface().requestLayout();
}

bool DetailsHeaderController::layoutColumns(bool miller) {
  if (miller) {
    m_detailsScrollHost->setVisible(false);
    m_detailsScrollHost->setParticipatesInLayout(false);
    return false;
  }
  const bool list = m_page.viewMode() == luciole::ViewMode::List;
  auto* fileList = m_page.fileList();
  // The header remains laid out when the file grid is hidden for an empty
  // folder or an error. Do not derive its width from that stale grid.
  const float viewport = list ? m_detailsHeaderViewport->width() : fileList->scrollView().contentViewportWidth(true);
  const auto columns = luciole::DetailsColumns::forWidth(viewport, m_columnVisibility, m_columnWidths);
  const bool columnsChanged = columns != m_detailsColumns;
  m_detailsColumns = columns;
  m_viewport = viewport;
  const bool overflow = list && !m_page.browser().model().rows().empty() && columns.contentWidth > viewport + 0.5F;
  m_detailsScrollHost->setVisible(overflow);
  m_detailsScrollHost->setParticipatesInLayout(overflow);
  for (std::size_t index = 0; index < m_columnWidths.size(); ++index) {
    auto* header = widthHeader(index);
    const bool visible =
        index == 0 || luciole::detailsColumnVisible(m_columnVisibility, static_cast<luciole::DetailsColumn>(index - 1));
    header->setVisible(visible);
    header->setParticipatesInLayout(visible);
    const float allocation = index == 0 ? columns.nameWidth : static_cast<float>(m_columnWidths[index]);
    header->setMinWidth(allocation);
    header->setMaxWidth(allocation);
    if (!visible && m_page.dispatcher().focusedArea() == header->inputArea()) {
      m_page.dispatcher().setFocus(m_nameHeader->inputArea());
    }
  }
  m_detailsOffset = std::clamp(m_detailsOffset, 0.0F, std::max(0.0F, columns.contentWidth - viewport));
  m_listAdapter.setDetailsColumns(columns);
  fileList->setHorizontalOverflow(list ? columns.contentWidth : 0.0F, list ? m_detailsOffset : 0.0F);
  if (columnsChanged) {
    fileList->notifyDataChanged();
  }
  return true;
}

void DetailsHeaderController::layoutHeader(Renderer& renderer) {
  const bool list = m_page.viewMode() == luciole::ViewMode::List;
  const float viewport = m_viewport;
  const auto& columns = m_detailsColumns;
  m_detailsHeaderRow->arrange(
      renderer, {-m_detailsOffset, 0, std::max(viewport, columns.contentWidth), m_detailsHeaderViewport->height()}
  );
  // Reveal focused headers without moving the user's horizontal position
  // on every frame. Pointer resizing retains its own captured offset.
  auto* focused = m_page.dispatcher().focusedArea();
  Button* reveal = std::exchange(m_revealDetailsHeader, nullptr);
  if (focused != m_lastDetailsFocus && !m_columnResize && !m_columnDrag) {
    for (std::size_t index = 0; index < m_columnWidths.size(); ++index) {
      if (widthHeader(index)->inputArea() == focused) {
        reveal = widthHeader(index);
      }
    }
  }
  m_lastDetailsFocus = focused;
  if (list && reveal && reveal->visible()) {
    const auto bounds = sceneRect(*reveal);
    const auto clip = sceneRect(*m_detailsHeaderViewport);
    const float delta = bounds.width > clip.width
        ? (Style::rtl() ? bounds.x + bounds.width - clip.x - clip.width : bounds.x - clip.x)
        : bounds.x < clip.x                             ? bounds.x - clip.x
        : bounds.x + bounds.width > clip.x + clip.width ? bounds.x + bounds.width - clip.x - clip.width
                                                        : 0.0F;
    if (delta != 0.0F) {
      scrollDetails(m_detailsOffset + delta);
    }
  }
  m_detailsScrollbar->update(viewport, columns.contentWidth, m_detailsOffset);
  m_detailsScrollbar->arrange(renderer, {0, 0, viewport, Style::scrollbarWidth});
  for (std::size_t index = 0; index < m_columnWidths.size(); ++index) {
    auto* handle = m_columnResizeHandles[index];
    auto* header = widthHeader(index);
    handle->setVisible(list && header->visible());
    const float edge = header->x() + (Style::rtl() ? 0.0F : header->width()) - m_detailsOffset;
    handle->arrange(renderer, {edge - 4.0F, 0, 8.0F, m_detailsHeaderViewport->height()});
    auto* line = m_columnResizeLines[index];
    const bool active = handle->hovered() || (m_columnResize && m_columnResize->index == index);
    const bool followed = std::ranges::any_of(m_columnOrder, [this, header](const auto column) {
      const auto* next = columnHeader(column);
      return next->visible() && (Style::rtl() ? next->x() < header->x() : next->x() > header->x());
    });
    line->setVisible(followed || active);
    line->setColor(colorSpecFromRole(active ? ColorRole::Primary : ColorRole::OutlineVariant));
    line->arrange(renderer, {3.5F, 4.0F, 1.0F, std::max(0.0F, handle->height() - 8.0F)});
  }
}

std::optional<DetailsHeaderController::ColumnDrop> DetailsHeaderController::columnDropAt(float x, float y) const {
  if (!m_columnDrag || !m_listHeader->visible() || !m_detailsHeaderViewport->containsScenePoint(x, y)) {
    return std::nullopt;
  }
  const auto source = m_columnDrag->source;
  if (!columnHeader(source)->visible()) {
    return std::nullopt;
  }
  std::vector<std::pair<luciole::DetailsColumn, LayoutRect>> visible;
  auto left = static_cast<float>(m_page.surface().width());
  float right = 0;
  for (const auto column : m_columnOrder) {
    auto* header = columnHeader(column);
    if (!header->visible()) {
      continue;
    }
    const auto rect = sceneRect(*header);
    left = std::min(left, rect.x);
    right = std::max(right, rect.x + rect.width);
    if (column != source) {
      visible.emplace_back(column, rect);
    }
  }
  const auto bounds = sceneRect(*columnHeader(source));
  if (visible.empty() || x < left || x > right || y < bounds.y || y > bounds.y + bounds.height) {
    return std::nullopt;
  }
  std::optional<luciole::DetailsColumn> before;
  float marker = Style::rtl() ? visible.back().second.x : visible.back().second.x + visible.back().second.width;
  for (const auto& [column, rect] : visible) {
    if (Style::rtl() ? x > rect.x + rect.width * 0.5F : x < rect.x + rect.width * 0.5F) {
      before = column;
      marker = Style::rtl() ? rect.x + rect.width : rect.x;
      break;
    }
  }
  const auto order = luciole::insertDetailsColumnBefore(m_columnOrder, source, before);
  if (!order) {
    return std::nullopt;
  }
  return ColumnDrop{*order, {marker - 1.0F, bounds.y, 2.0F, bounds.height}};
}

void DetailsHeaderController::cancelColumnDrag() {
  const auto drag = std::exchange(m_columnDrag, std::nullopt);
  if (!drag) {
    return;
  }
  auto* header = columnHeader(drag->source);
  header->setCursorShape(drag->cursor);
  header->setHoverSuppressed(false);
  header->setOpacity(1.0F);
  if (m_page.dispatcher().focusedArea() == header->inputArea()) {
    m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  }
  m_columnHoverDirty = true;
  m_page.surface().requestLayout();
}

void DetailsHeaderController::updateColumnDrag() {
  if (!m_columnDrag) {
    return;
  }
  if (!m_columnDrag->active
      && std::hypot(m_page.pointerX() - m_columnDrag->pressX, m_page.pointerY() - m_columnDrag->pressY) < 6.0F) {
    return;
  }
  m_columnDrag->active = true;
  auto* header = columnHeader(m_columnDrag->source);
  header->setHoverSuppressed(true);
  header->setCursorShape(
      columnDropAt(m_page.pointerX(), m_page.pointerY()) ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                                         : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP
  );
  m_page.surface().requestLayout();
}

void DetailsHeaderController::finishColumnDrag(float x, float y) {
  const auto drop = columnDropAt(x, y);
  m_page.dispatcher().cancelPointerCapture();
  cancelColumnDrag();
  if (drop) {
    setDetailsColumnOrder(drop->order);
  }
  m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  m_page.dispatcher().syncPointerHover();
  m_page.surface().requestLayout();
}

void DetailsHeaderController::layoutDrag(Renderer& renderer) {
  const bool active = m_columnDrag && m_columnDrag->active;
  for (const auto column : m_columnOrder) {
    columnHeader(column)->setOpacity(active && m_columnDrag->source == column ? 0.65F : 1.0F);
  }
  const auto drop = active ? columnDropAt(m_page.pointerX(), m_page.pointerY()) : std::nullopt;
  const bool changed = drop && drop->order != m_columnOrder;
  m_columnDropLine->setVisible(changed);
  if (changed) {
    m_columnDropLine->arrange(renderer, drop->marker);
  }
}

Button* DetailsHeaderController::columnHeader(luciole::DetailsColumn column) const {
  return m_headers[static_cast<std::size_t>(column)];
}

std::optional<luciole::DetailsColumnOrder>
DetailsHeaderController::movedColumnOrder(luciole::DetailsColumn column, int direction) const {
  return luciole::moveDetailsColumn(m_columnOrder, column, direction, m_columnVisibility);
}

void DetailsHeaderController::setDetailsColumnOrder(luciole::DetailsColumnOrder order) {
  if (order == m_columnOrder || !luciole::validDetailsColumnOrder(order)) {
    return;
  }
  m_page.captureViewAnchor();
  m_page.dispatcher().cancelPointerCapture();
  m_listAdapter.onPointerCancel();
  m_gridAdapter.onPointerCancel();
  m_columnOrder = order;
  m_preferences.saveColumnOrder(order);
  m_page.requestSync();
}

void DetailsHeaderController::sortBy(luciole::SortField field) {
  auto options = m_page.browser().model().sort();
  options.descending = options.field == field && !options.descending;
  options.field = field;
  const auto action = field == luciole::SortField::Name ? Action::SortName
      : field == luciole::SortField::Type               ? Action::SortType
      : field == luciole::SortField::Size               ? Action::SortSize
      : field == luciole::SortField::Accessed           ? Action::SortAccessed
      : field == luciole::SortField::Created            ? Action::SortCreated
      : field == luciole::SortField::Owner              ? Action::SortOwner
      : field == luciole::SortField::Group              ? Action::SortGroup
      : field == luciole::SortField::Permissions        ? Action::SortPermissions
      : field == luciole::SortField::Location           ? Action::SortLocation

                                              : Action::SortModified;
  if (m_page.browser().model().sort().field == field) {
    m_page.dispatchAction(options.descending ? Action::Descending : Action::Ascending);
  } else {
    m_page.dispatchAction(action);
  }
}

void DetailsHeaderController::revealNameColumn() {
  if (m_page.viewMode() != luciole::ViewMode::List) {
    return;
  }
  const float target = Style::rtl() ? std::max(0.0F, m_detailsColumns.contentWidth - m_viewport) : 0.0F;
  if (m_detailsOffset != target) {
    scrollDetails(target);
  }
}

void DetailsHeaderController::cancelGesture() {
  if (m_columnDrag || m_columnResize) {
    m_page.dispatcher().cancelPointerCapture();
  }
}
