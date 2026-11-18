#include "app/page/location_bar_controller.h"

#include "app/page/file_keyboard_controller.h"
#include "app/page/location_bar_controller.h"
#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/input/key_symbols.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

LocationBarController::LocationBarController(
    PageContext& page, WaylandConnection& connection, FileKeyboardController& keys, MenuController& menus
)
    : m_page(page), m_connection(connection), m_keys(keys), m_menus(menus) {}

std::unique_ptr<Flex> LocationBarController::buildLocationArea() {
  auto breadcrumbScroll = ui::scrollView(
      {.out = &m_breadcrumbScroll,
       .orientation = ScrollOrientation::Horizontal,
       .scrollbarVisible = false,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .fillHeight = true,
       .flexGrow = 1.0F}
  );
  breadcrumbScroll->content()->setDirection(FlexDirection::Horizontal);
  breadcrumbScroll->content()->addChild(
      ui::row({.out = &m_breadcrumbs, .align = FlexAlign::Center, .gap = 0.0F, .height = Style::controlHeight})
  );
  auto locationArea = ui::column(
      {.out = &m_locationArea,
       .align = FlexAlign::Stretch,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
       .styleRadius = Style::radiusMd,
       .minWidth = 80.0F,
       .height = Style::controlHeight,
       .flexGrow = 1.0F},
      std::move(breadcrumbScroll),
      ui::input(
          {.out = &m_location,
           .placeholder = "Location",
           .frameVisible = false,
           .minLayoutWidth = 72.0F,
           .flexGrow = 1.0F,
           .visible = false,
           .participatesInLayout = false,
           .onChange = [this](const std::string& text) { requestCompletions(text); },
           .onSubmit =
               [this](const std::string& text) {
                 m_locationInvalid = !m_page.browser().openText(text);
                 if (!m_locationInvalid) {
                   endLocationEdit(true);
                 }
                 m_page.requestSync();
               },
           .onFocusLoss =
               [this] {
                 if (m_editingLocation) {
                   endLocationEdit(false);
                 }
               }}
      )
  );
  return locationArea;
}

std::unique_ptr<Flex> LocationBarController::buildSearchArea() {
  auto searchArea = ui::row(
      {.out = &m_searchArea,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs,
       .paddingH = Style::spaceSm,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
       .styleRadius = Style::radiusMd,
       .minWidth = 220.0F,
       .maxWidth = 220.0F,
       .height = Style::controlHeight,
       .configure = [](Flex& area) { area.setPadding(0.0F, 0.0F, 0.0F, Style::spaceSm); }},
      ui::glyph(
          {.glyph = "search",
           .glyphSize = Style::baseGlyphSize,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
      ),
      ui::input(
          {.out = &m_filterInput,
           .placeholder = "Search files…",
           .controlHeight = Style::controlHeight,
           .horizontalPadding = 0.0F,
           .clearButtonEnabled = true,
           .frameVisible = false,
           .minLayoutWidth = 72.0F,
           .flexGrow = 1.0F,
           .onChange = [this](const std::string& text) { changeNameFilter(text); },
           .onSubmit =
               [this](const std::string&) {
                 if (m_filterInput->composing()) {
                   return;
                 }
                 m_page.browser().submitSearchQuery();
                 focusResults();
               },
           .configure =
               [this](Input& input) {
                 input.setOnCompositionChange([this](bool composing) {
                   m_page.browser().setSearchComposing(composing);
                 });
                 input.setClearButtonWidth(24.0F);
                 input.inputArea()->setTooltip("Search names in this folder and all subfolders");
                 input.setOnFocusGain([this] {
                   m_filterOpen = true;
                   m_page.requestSync();
                 });
                 input.setOnFocusLoss([this] { m_page.requestSync(); });
               }}
      )
  );
  return searchArea;
}

void LocationBarController::buildCompletion(Node& toolbar) {
  auto completion = std::make_unique<ContextMenuControl>();
  m_completionControl = completion.get();
  completion->setOnActivate([this](const ContextMenuControlEntry& entry) {
    acceptCompletion(static_cast<std::size_t>(entry.id), false);
  });
  auto completionFrame = ui::column(
      {.out = &m_completionFrame,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
       .styleRadius = Style::radiusLg,
       .border = colorSpecFromRole(ColorRole::OutlineVariant),
       .visible = false,
       .participatesInLayout = false},
      std::move(completion)
  );
  completionFrame->setZIndex(110);
  toolbar.addChild(std::move(completionFrame));
}

bool LocationBarController::textFocused() const {
  return (m_location != nullptr && m_page.dispatcher().focusedArea() == m_location->inputArea())
      || (m_filterInput != nullptr && m_page.dispatcher().focusedArea() == m_filterInput->inputArea());
}

bool LocationBarController::breadcrumbsContain(float x, float y) const {
  return !m_editingLocation && m_breadcrumbScroll->containsScenePoint(x, y);
}

bool LocationBarController::completeOnTab(std::uint32_t modifiers) {
  return m_editingLocation
      && m_location != nullptr
      && m_page.dispatcher().focusedArea() == m_location->inputArea()
      && completionKey(XKB_KEY_Tab, modifiers);
}

void LocationBarController::pointer(const PointerEvent& event, float x, float y) {
  if (m_blankLocationClick
      && (event.type == PointerEvent::Type::Leave
          || event.type == PointerEvent::Type::Axis
          || (event.type == PointerEvent::Type::Motion
              && (std::abs(x - m_blankLocationClick->x) > 6.0F || std::abs(y - m_blankLocationClick->y) > 6.0F)))) {
    m_blankLocationClick.reset();
  }
  if (event.type == PointerEvent::Type::Button
      && event.pressed
      && m_editingLocation
      && m_completionFrame
      && !m_completionFrame->containsScenePoint(x, y)) {
    clearCompletions();
  }
}

bool LocationBarController::press(const PointerEvent& event, float x, float y) {
  if (event.type != PointerEvent::Type::Button || !event.pressed) {
    return false;
  }
  const bool twice = event.button == BTN_LEFT
      && m_lastBreadcrumb
      && !m_editingLocation
      && m_page.browser().model().snapshot().location == m_lastBreadcrumb
      && std::chrono::steady_clock::now() - m_breadcrumbClickTime < std::chrono::milliseconds(400)
      && std::abs(x - m_breadcrumbClickX) <= 6.0F
      && std::abs(y - m_breadcrumbClickY) <= 6.0F;
  m_lastBreadcrumb.reset();
  float localX = 0, localY = 0;
  if (twice
      && m_locationArea != nullptr
      && Node::mapFromScene(m_locationArea, x, y, localX, localY)
      && localX >= 0
      && localY >= 0
      && localX < m_locationArea->width()
      && localY < m_locationArea->height()) {
    // Recognize the second press across the ancestor navigation's scene rebuild.
    m_breadcrumbEditTarget = m_page.browser().model().snapshot().location;
    m_page.requestSync();
    return true;
  }
  const auto& location = m_page.browser().model().snapshot().location;
  const bool blank = event.button == BTN_LEFT
      && m_connection.keyboardModifiers() == 0
      && !m_editingLocation
      && location
      && m_locationArea
      && m_locationArea->containsScenePoint(x, y)
      && !(m_breadcrumbOverflow && m_breadcrumbOverflow->visible() && m_breadcrumbOverflow->containsScenePoint(x, y))
      && std::ranges::none_of(m_dropBreadcrumbs, [x, y](const auto& crumb) {
                       return crumb.second->containsScenePoint(x, y);
                     });
  if (blank) {
    const auto now = std::chrono::steady_clock::now();
    const bool twice = m_blankLocationClick
        && m_blankLocationClick->location == location
        && m_blankLocationClick->context == m_page.browser().contextSerial()
        && now - m_blankLocationClick->time < std::chrono::milliseconds(400)
        && std::abs(x - m_blankLocationClick->x) <= 6.0F
        && std::abs(y - m_blankLocationClick->y) <= 6.0F;
    if (twice) {
      m_blankLocationClick.reset();
      m_breadcrumbEditTarget = location;
      m_page.surface().requestUpdate();
      return true;
    }
    m_blankLocationClick = LocationBlankClick{*location, m_page.browser().contextSerial(), now, x, y};
  } else {
    m_blankLocationClick.reset();
  }
  return false;
}

void LocationBarController::keyPressed() {
  m_lastBreadcrumb.reset();
  m_blankLocationClick.reset();
}

bool LocationBarController::menuKey(const KeyboardEvent& event) {
  if ((event.sym == XKB_KEY_Menu && event.modifiers == 0)
      || (event.sym == XKB_KEY_F10 && event.modifiers == KeyMod::Shift)) {
    if (m_breadcrumbOverflow
        && m_breadcrumbOverflow->visible()
        && m_page.dispatcher().focusedArea() == m_breadcrumbOverflow->inputArea()) {
      openBreadcrumbOverflow();
      return true;
    }
    for (const auto& [target, button] : m_dropBreadcrumbs) {
      if (m_page.dispatcher().focusedArea() == button->inputArea()) {
        m_menus.openBreadcrumbMenu(target, sceneRect(*button));
        return true;
      }
    }
  }
  return false;
}

bool LocationBarController::completionKey(const KeyboardEvent& event) {
  return m_editingLocation && completionKey(event.sym, event.modifiers);
}

bool LocationBarController::escapeKey(const KeyboardEvent& event) {
  if (m_editingLocation && event.modifiers == 0 && KeySymbol::isEscape(event.sym)) {
    endLocationEdit(true);
    return true;
  }
  if (m_filterOpen && event.modifiers == 0 && KeySymbol::isEscape(event.sym) && !m_editingLocation) {
    closeFilter();
    return true;
  }
  return false;
}

bool LocationBarController::filterKey(const KeyboardEvent& event) {
  if (!event.pressed
      || event.preedit
      || event.modifiers != 0
      || !KeySymbol::isDown(event.sym)
      || m_filterInput == nullptr
      || m_page.dispatcher().focusedArea() != m_filterInput->inputArea()
      || m_filterInput->composing()
      || m_page.browser().model().rows().empty()) {
    return false;
  }
  focusResults();
  return true;
}

void LocationBarController::focusResults() {
  if (!m_page.browser().model().rows().empty()) {
    m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
    m_keys.selectFirstIfNone();
  }
}

bool LocationBarController::syncLocation(const std::optional<luciole::Location>& location) {
  const auto uri = location ? location->uri() : std::string{};
  if (uri == m_displayedLocation) {
    return false;
  }
  clearCompletions();
  if (location != m_lastBreadcrumb) {
    m_lastBreadcrumb.reset();
  }
  m_filterOpen = false;
  if (m_page.dispatcher().focusedArea() == m_filterInput->inputArea() || m_editingLocation) {
    m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  }
  m_editingLocation = false;
  m_location->setVisible(false);
  m_location->setParticipatesInLayout(false);
  m_breadcrumbScroll->setVisible(true);
  m_breadcrumbScroll->setParticipatesInLayout(true);
  m_breadcrumbsDirty = true;
  m_displayedLocation = uri;
  m_location->setValue(location ? luciole::locationEditText(*location) : std::string{});
  m_locationInvalid = false;
  return true;
}

void LocationBarController::sync() {
  m_location->setInvalid(m_locationInvalid);
  const bool recursive = m_page.browser().searchRecursive();
  m_filterInput->setPlaceholder(recursive ? "Search files…" : "Filter files…");
  m_filterInput->inputArea()->setTooltip(
      recursive ? "Search names in this folder and all subfolders" : "Filter names in the current folder only"
  );
  if (!m_filterInput->composing() && m_filterInput->value() != m_page.browser().searchQuery()) {
    m_filterInput->setValue(m_page.browser().searchQuery());
  }
  // Fields are borderless pills on the chrome; only focus and errors draw an outline.
  if (m_page.dispatcher().focusedArea() == m_filterInput->inputArea()) {
    m_searchArea->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
  } else {
    m_searchArea->clearBorder();
  }
  if (m_locationInvalid) {
    m_locationArea->setBorder(colorSpecFromRole(ColorRole::Error), Style::borderWidth);
  } else {
    m_locationArea->clearBorder();
  }
}

bool LocationBarController::onFrame() {
  if (const auto target = std::exchange(m_breadcrumbEditTarget, std::nullopt);
      target && target == m_page.browser().model().snapshot().location) {
    beginLocationEdit();
    return true;
  }
  return false;
}

void LocationBarController::prepareLayout() {
  const float searchWidth = std::clamp(static_cast<float>(m_page.surface().width()) * 0.22F, 148.0F, 220.0F);
  m_searchArea->setMinWidth(searchWidth);
  m_searchArea->setMaxWidth(searchWidth);
  if (m_breadcrumbsDirty) {
    m_breadcrumbsDirty = false;
    rebuildBreadcrumbs();
  }
}

void LocationBarController::clearCompletions() {
  m_pathCompletion.cancel();
  m_completionText.clear();
  m_completionItems = {};
  m_completionIndex.reset();
  m_completionDirty = true;
  m_page.surface().requestLayout();
}

void LocationBarController::requestCompletions(const std::string& text) {
  clearCompletions();
  if (!m_editingLocation || !m_page.browser().model().snapshot().location || m_location->composing()) {
    return;
  }
  const auto edit = m_location->textInputState();
  if (edit.cursor != static_cast<std::int32_t>(text.size()) || edit.anchor != edit.cursor) {
    return;
  }
  const auto context = m_page.browser().contextSerial();
  m_pathCompletion.complete(
      text, *m_page.browser().model().snapshot().location, m_page.browser().model().filter().showHidden,
      [this, text, context](luciole::PathSuggestions suggestions) {
        if (!m_editingLocation
            || context != m_page.browser().contextSerial()
            || m_location->value() != text
            || m_location->composing()) {
          return;
        }
        const auto edit = m_location->textInputState();
        if (edit.cursor != static_cast<std::int32_t>(text.size()) || edit.anchor != edit.cursor) {
          return;
        }
        m_completionText = text;
        m_completionContext = context;
        m_completionItems = std::move(suggestions);
        m_completionIndex.reset();
        m_completionDirty = true;
        m_page.surface().requestLayout();
      }
  );
}

void LocationBarController::acceptCompletion(std::size_t index, bool navigate) {
  if (index >= m_completionItems.items.size()) {
    return;
  }
  const auto suggestion = m_completionItems.items[index];
  clearCompletions();
  if (navigate) {
    m_page.browser().open(suggestion.location);
    endLocationEdit(true);
  } else {
    m_location->setValue(suggestion.text);
    m_location->selectRange(suggestion.text.size(), suggestion.text.size());
    m_page.dispatcher().setFocus(m_location->inputArea());
  }
}

bool LocationBarController::completionKey(std::uint32_t key, std::uint32_t modifiers) {
  const auto edit = m_location->textInputState();
  if (m_location->composing()
      || m_completionContext != m_page.browser().contextSerial()
      || m_location->value() != m_completionText
      || edit.cursor != static_cast<std::int32_t>(m_completionText.size())
      || edit.anchor != edit.cursor) {
    clearCompletions();
    return false;
  }
  if (modifiers != 0 && (!KeySymbol::isTab(key) || modifiers != KeyMod::Shift)) {
    return false;
  }
  if (m_completionItems.items.empty()) {
    return false;
  }
  if (KeySymbol::isEscape(key)) {
    clearCompletions();
    return true;
  }
  if (KeySymbol::isTab(key)) {
    acceptCompletion(m_completionIndex.value_or(0), false);
    return true;
  }
  if (KeySymbol::isEnter(key) && m_completionIndex) {
    acceptCompletion(*m_completionIndex, true);
    return true;
  }
  if (KeySymbol::isDown(key) || KeySymbol::isUp(key)) {
    const auto count = m_completionItems.items.size();
    m_completionIndex = m_completionIndex
        ? (*m_completionIndex + count + (KeySymbol::isDown(key) ? 1 : count - 1)) % count
        : KeySymbol::isDown(key) ? 0
                                 : count - 1;
    m_completionDirty = true;
    m_page.surface().requestLayout();
    return true;
  }
  if (KeySymbol::isLeft(key) || KeySymbol::isRight(key) || KeySymbol::isHome(key) || KeySymbol::isEnd(key)) {
    clearCompletions();
  }
  return false;
}

void LocationBarController::layoutCompletions(Renderer& renderer) {
  if (m_completionDirty) {
    m_completionDirty = false;
    std::vector<ContextMenuControlEntry> entries;
    constexpr std::size_t pageSize = 6;
    const auto page = m_completionIndex.value_or(0) / pageSize * pageSize;
    for (auto i = page; i < std::min(page + pageSize, m_completionItems.items.size()); ++i) {
      entries.push_back({.id = static_cast<int>(i), .label = m_completionItems.items[i].label});
    }
    if (m_completionItems.items.size() > pageSize || m_completionItems.truncated) {
      entries.push_back(
          {.label = m_completionItems.truncated ? "More matches; type to narrow" : "Arrow keys for more matches",
           .enabled = false}
      );
    }
    if (!m_completionItems.error.empty() && entries.empty()) {
      entries.push_back({.label = "Folder suggestions unavailable", .enabled = false});
    }
    const bool visible = m_editingLocation && !entries.empty() && m_page.popupSurface() == nullptr;
    m_completionFrame->setVisible(visible);
    m_completionControl->setMaxVisible(entries.size());
    m_completionControl->setEntries(std::move(entries));
    if (m_completionIndex) {
      m_completionControl->setHighlightedIndex(*m_completionIndex - page);
    } else {
      m_completionControl->setHighlightedIndex(m_completionControl->entryCount());
    }
  }
  if (!m_completionFrame->visible()) {
    return;
  }
  const auto anchor = sceneRect(*m_locationArea);
  const float width = std::min(anchor.width, static_cast<float>(m_page.surface().width()) - 16.0F);
  float x = 0, y = 0;
  // The dropdown extends outside the toolbar's hit bounds.
  (void)Node::mapFromScene(
      m_completionFrame->parent(),
      std::clamp(anchor.x, 8.0F, std::max(8.0F, static_cast<float>(m_page.surface().width()) - width - 8.0F)),
      anchor.y + anchor.height + 4.0F, x, y
  );
  m_completionControl->setMenuWidth(width);
  m_completionFrame->arrange(renderer, {x, y, width, m_completionControl->preferredHeight()});
}

void LocationBarController::beginLocationEdit() {
  m_blankLocationClick.reset();
  m_lastBreadcrumb.reset();
  m_breadcrumbEditTarget.reset();
  clearCompletions();
  m_editingLocation = true;
  m_locationInvalid = false;
  m_location->setValue(
      m_page.browser().model().snapshot().location
          ? luciole::locationEditText(*m_page.browser().model().snapshot().location)
          : std::string{}
  );
  m_location->setVisible(true);
  m_location->setParticipatesInLayout(true);
  m_breadcrumbScroll->setVisible(false);
  m_breadcrumbScroll->setParticipatesInLayout(false);
  m_page.dispatcher().setFocus(m_location->inputArea());
  m_location->selectAll();
  m_page.requestSync();
}

void LocationBarController::endLocationEdit(bool focusFiles) {
  clearCompletions();
  m_editingLocation = false;
  m_locationInvalid = false;
  m_location->setVisible(false);
  m_location->setParticipatesInLayout(false);
  m_breadcrumbScroll->setVisible(true);
  m_breadcrumbScroll->setParticipatesInLayout(true);
  if (focusFiles) {
    m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  }
  m_page.requestSync();
}

void LocationBarController::breadcrumbClick(const luciole::Location& target, const InputArea::PointerData* pointer) {
  m_lastBreadcrumb = pointer != nullptr ? std::optional(target) : std::nullopt;
  if (pointer != nullptr) {
    m_breadcrumbClickTime = std::chrono::steady_clock::now();
    m_breadcrumbClickX = pointer->sceneX;
    m_breadcrumbClickY = pointer->sceneY;
  }
  m_page.browser().open(target);
  m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
}

void LocationBarController::openBreadcrumbOverflow() {
  if (!m_breadcrumbOverflow || !m_breadcrumbOverflow->visible() || m_firstBreadcrumb == 0) {
    return;
  }
  m_lastBreadcrumb.reset();
  m_blankLocationClick.reset();
  const auto context = m_page.browser().contextSerial();
  std::vector<luciole::Location> targets;
  std::vector<ContextMenuControlEntry> entries;
  for (std::size_t index = 0; index < m_firstBreadcrumb; ++index) {
    const auto& part = m_breadcrumbParts[index];
    targets.push_back(part.location);
    entries.push_back({.id = static_cast<int>(index), .label = part.button->label()->text()});
  }
  m_menus.showContextMenu(
      std::move(entries),
      [this, context, targets = std::move(targets)](int id) {
        if (context == m_page.browser().contextSerial() && id >= 0 && static_cast<std::size_t>(id) < targets.size()) {
          breadcrumbClick(targets[static_cast<std::size_t>(id)]);
        }
      },
      sceneRect(*m_breadcrumbOverflow)
  );
}

bool LocationBarController::layoutBreadcrumbs(Renderer& renderer) {
  if (!m_breadcrumbScroll->visible() || m_breadcrumbParts.empty()) {
    return false;
  }
  const float available = m_breadcrumbScroll->contentViewportWidth();
  if (available == m_breadcrumbLayoutWidth
      && renderer.renderScale() == m_breadcrumbRenderScale
      && renderer.textMetricsGeneration() == m_breadcrumbMetricsGeneration) {
    return false;
  }

  std::vector<float> widths;
  std::vector<float> separators;
  widths.reserve(m_breadcrumbParts.size());
  separators.reserve(m_breadcrumbParts.size());
  float total = 0;
  for (const auto& part : m_breadcrumbParts) {
    part.button->setMaxWidth(160.0F);
    const float width = part.button->measure(renderer, {}).width;
    const float separator = part.separator ? part.separator->measure(renderer, {}).width : 0.0F;
    widths.push_back(width);
    separators.push_back(separator);
    total += width + separator;
  }
  const bool overflow = total > available && m_breadcrumbParts.size() > 1;
  std::size_t first = 0;
  float leafBudget = std::max(1.0F, available);
  if (overflow) {
    const float overflowWidth = m_breadcrumbOverflow->measure(renderer, {}).width;
    first = m_breadcrumbParts.size() - 1;
    leafBudget = std::max(1.0F, available - overflowWidth - separators.back());
    float used = overflowWidth + separators.back() + std::min(widths.back(), leafBudget);
    while (first > 1 && used + widths[first - 1] + separators[first - 1] <= available) {
      --first;
      used += widths[first] + separators[first];
    }
  }
  auto* focus = m_page.dispatcher().focusedArea();
  m_breadcrumbOverflow->setVisible(overflow);
  m_breadcrumbOverflow->setParticipatesInLayout(overflow);
  if (!overflow && focus == m_breadcrumbOverflow->inputArea()) {
    m_page.dispatcher().setFocus(m_breadcrumbParts.back().button->inputArea());
  }
  m_dropBreadcrumbs.clear();
  for (std::size_t index = 0; index < m_breadcrumbParts.size(); ++index) {
    const auto& part = m_breadcrumbParts[index];
    const bool visible = index >= first;
    part.button->setVisible(visible);
    part.button->setParticipatesInLayout(visible);
    if (part.separator) {
      const bool separatorVisible = visible && (index > first || overflow);
      part.separator->setVisible(separatorVisible);
      part.separator->setParticipatesInLayout(separatorVisible);
    }
    if (visible) {
      m_dropBreadcrumbs.emplace_back(part.location, part.button);
    } else if (focus == part.button->inputArea()) {
      m_page.dispatcher().setFocus(m_breadcrumbOverflow->inputArea());
    }
  }
  m_firstBreadcrumb = first;
  m_breadcrumbParts.back().button->setMaxWidth(std::min(160.0F, leafBudget));
  m_breadcrumbScroll->requestScrollToOffset(0);
  m_breadcrumbScroll->arrange(
      renderer,
      {m_breadcrumbScroll->x(), m_breadcrumbScroll->y(), m_breadcrumbScroll->width(), m_breadcrumbScroll->height()}
  );
  m_breadcrumbLayoutWidth = available;
  m_breadcrumbRenderScale = renderer.renderScale();
  m_breadcrumbMetricsGeneration = renderer.textMetricsGeneration();
  m_page.dispatcher().syncPointerHover();
  return true;
}

void LocationBarController::rebuildBreadcrumbs() {
  m_dropBreadcrumbs.clear();
  m_breadcrumbParts.clear();
  m_breadcrumbOverflow = nullptr;
  m_breadcrumbLayoutWidth = -1;
  while (!m_breadcrumbs->children().empty()) {
    m_breadcrumbs->removeChild(m_breadcrumbs->children().back().get());
  }
  const auto& location = m_page.browser().model().snapshot().location;
  if (!location) {
    return;
  }
  const auto crumbs = luciole::locationBreadcrumbs(*location);
  m_breadcrumbs->addChild(
      ui::button(
          {.out = &m_breadcrumbOverflow,
           .glyph = "dots",
           .glyphSize = Style::baseGlyphSize,
           .controlHeight = Style::controlHeightSm,
           .variant = ButtonVariant::Ghost,
           .tooltip = "Parent folders",
           .paddingH = Style::spaceSm,
           .onClick = [this] { openBreadcrumbOverflow(); }}
      )
  );
  for (std::size_t i = 0; i < crumbs.size(); ++i) {
    Node* separator = nullptr;
    if (i != 0) {
      separator =
          m_breadcrumbs->addChild(ui::label({.text = "/", .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
    }
    const auto target = crumbs[i].location;
    auto* partButton = static_cast<Button*>(m_breadcrumbs->addChild(
        ui::button(
            {.text = crumbs[i].label,
             .controlHeight = Style::controlHeightSm,
             .contentAlign = ButtonContentAlign::Start,
             .variant = ButtonVariant::Ghost,
             .tooltip = crumbs[i].label,
             .maxWidth = 160.0F,
             .paddingH = Style::spaceSm,
             .onClick = [this, target] { breadcrumbClick(target); },
             .configure =
                 [this, target](Button& button) {
                   button.label()->setUseMarkup(false);
                   button.inputArea()->setAcceptedButtons(
                       InputArea::buttonMask(BTN_LEFT) | InputArea::buttonMask(BTN_RIGHT)
                   );
                   button.inputArea()->setOnClick([this, target](const InputArea::PointerData& pointer) {
                     if (pointer.button == BTN_LEFT) {
                       breadcrumbClick(target, &pointer);
                     } else if (pointer.button == BTN_RIGHT) {
                       m_menus.openBreadcrumbMenu(target, {pointer.sceneX, pointer.sceneY, 1, 1});
                     }
                   });
                 }}
        )
    ));
    m_breadcrumbParts.push_back({target, partButton, separator});
  }
}

void LocationBarController::openFilter(bool selectAll, bool preserveTextInput) {
  m_blankLocationClick.reset();
  m_keys.clearTypeahead();
  m_lastBreadcrumb.reset();
  m_breadcrumbEditTarget.reset();
  if (m_editingLocation) {
    endLocationEdit(false);
  }
  m_filterOpen = true;
  if (!m_filterInput->composing() && m_filterInput->value() != m_page.browser().searchQuery()) {
    m_filterInput->setValue(m_page.browser().searchQuery());
  }
  m_page.dispatcher().setFocus(m_filterInput->inputArea(), preserveTextInput);
  if (selectAll) {
    m_filterInput->selectAll();
  }
  m_page.requestSync();
}

void LocationBarController::changeNameFilter(const std::string& text) {
  m_filterOpen = true;
  m_page.browser().setSearchComposing(m_filterInput->composing());
  if (text == m_page.browser().searchQuery()) {
    return;
  }
  m_page.resetFileView();
  m_page.browser().editSearchQuery(text);
}

void LocationBarController::closeFilter() {
  m_filterOpen = false;
  changeNameFilter({});
  m_filterOpen = false;
  m_filterInput->setValue({});
  m_page.dispatcher().setFocus(m_page.fileList()->focusArea());
  m_page.requestSync();
}
