#include "ui/controls/tooltip_presenter.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace {
  constexpr auto kShowDelay = std::chrono::milliseconds(500);
  constexpr auto kFileRefresh = std::chrono::seconds(2);
  constexpr float kMaxContentWidth = 280.0F;
  constexpr float kMaxTableWidth = 360.0F;
  constexpr std::size_t kMaxRows = 8;
  constexpr float kPadH = Style::spaceMd + Style::borderWidth;
  constexpr float kPadV = Style::spaceSm + Style::borderWidth;
  bool sameContent(const TooltipContent& a, const TooltipContent& b) {
    if (a.index() != b.index()) {
      return false;
    }
    if (const auto* text = std::get_if<std::string>(&a)) {
      return *text == std::get<std::string>(b);
    }
    if (const auto* rows = std::get_if<std::vector<TooltipRow>>(&a)) {
      const auto& other = std::get<std::vector<TooltipRow>>(b);
      return rows->size() == other.size()
          && std::equal(rows->begin(), rows->end(), other.begin(), [](const auto& x, const auto& y) {
               return x.key == y.key && x.value == y.value && x.valueEllipsize == y.valueEllipsize;
             });
    }
    return true;
  }
  bool empty(const TooltipContent& content) {
    if (const auto* text = std::get_if<std::string>(&content)) {
      return text->empty();
    }
    if (const auto* rows = std::get_if<std::vector<TooltipRow>>(&content)) {
      return rows->empty();
    }
    return true;
  }
} // namespace

TooltipPresenter::TooltipPresenter(std::function<void()> requestUpdate)
    : m_requestUpdate(std::move(requestUpdate)), m_fileData([this] { m_requestUpdate(); }) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setPadding(kPadV, kPadH);
  setFill(colorSpecFromRole(ColorRole::Surface));
  setStyleRadius(Style::radiusMd);
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  setParticipatesInLayout(false);
  setHitTestVisible(false);
  setExcludeSubtreeFromTabOrder(true);
  setZIndex(100);
  setVisible(false);
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  addChild(
      ui::label(
          {.out = &m_label,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurface),
           .maxWidth = kMaxContentWidth,
           .maxLines = 3,
           .configure = literal}
      )
  );
  auto table = ui::column({.out = &m_table, .align = FlexAlign::Stretch, .gap = Style::spaceXs});
  for (std::size_t i = 0; i < kMaxRows; ++i) {
    Row row{};
    table->addChild(
        ui::row(
            {.out = &row.container, .align = FlexAlign::Center, .gap = Style::spaceMd},
            ui::label(
                {.out = &row.key,
                 .fontSize = Style::fontSizeCaption,
                 .color = colorSpecFromRole(ColorRole::Secondary),
                 .maxLines = 1,
                 .configure = literal}
            ),
            ui::label(
                {.out = &row.value,
                 .fontSize = Style::fontSizeCaption,
                 .fontFamily = "monospace",
                 .color = colorSpecFromRole(ColorRole::OnSurface),
                 .maxLines = 1,
                 .textAlign = TextAlign::End,
                 .flexGrow = 1.0F,
                 .configure = literal}
            )
        )
    );
    m_rows.push_back(row);
  }
  addChild(std::move(table));
}

void TooltipPresenter::dismiss() {
  m_delay.stop();
  m_refresh.stop();
  m_fileData.clear();
  m_file.reset();
  m_controlContent = {};
  m_identity = nullptr;
  m_ready = false;
  m_armed = false;
  m_refreshDue = false;
  m_interval = {};
  setVisible(false);
}

void TooltipPresenter::populate(Renderer& renderer, const TooltipContent& content, LayoutSize bounds) {
  const auto* text = std::get_if<std::string>(&content);
  m_label->setVisible(text != nullptr);
  m_label->setParticipatesInLayout(text != nullptr);
  m_table->setVisible(!text);
  m_table->setParticipatesInLayout(!text);
  const float available = std::max(1.0F, bounds.width - 2 * (Style::spaceSm + kPadH));
  if (text) {
    m_label->setText(*text);
    m_label->setMaxWidth(std::min(kMaxContentWidth, available));
    return;
  }
  const auto& rows = std::get<std::vector<TooltipRow>>(content);
  const float width = std::min(kMaxTableWidth, available);
  float keyWidth = 0;
  for (std::size_t i = 0; i < std::min(rows.size(), kMaxRows); ++i) {
    keyWidth = std::max(keyWidth, renderer.measureText(rows[i].key, Style::fontSizeCaption).width);
  }
  keyWidth = std::min(keyWidth, width * 0.35F);
  m_table->setMinWidth(width);
  m_table->setMaxWidth(width);
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    auto& row = m_rows[i];
    const bool visible = i < rows.size();
    row.container->setVisible(visible);
    row.container->setParticipatesInLayout(visible);
    if (visible) {
      row.key->setText(rows[i].key);
      row.key->setMinWidth(keyWidth);
      row.key->setMaxWidth(std::max(1.0F, keyWidth));
      row.value->setText(rows[i].value);
      row.value->setMaxWidth(std::max(1.0F, width - keyWidth - Style::spaceMd));
      row.value->setEllipsize(rows[i].valueEllipsize);
    }
  }
}

void TooltipPresenter::place(Renderer& renderer, LayoutSize bounds) {
  const auto size = measure(renderer, LayoutConstraints::available(bounds.width, bounds.height));
  float x = m_file ? m_anchor.x + Style::spaceMd : m_anchor.x + (m_anchor.width - size.width) * 0.5F;
  float y = m_anchor.y + m_anchor.height + Style::spaceXs;
  if (m_placement == TooltipPlacement::Above
      || ((m_placement == TooltipPlacement::Default || m_placement == TooltipPlacement::Below)
          && y + size.height > bounds.height - Style::spaceSm)) {
    y = m_anchor.y - size.height - Style::spaceSm;
  } else if (m_placement == TooltipPlacement::Left || m_placement == TooltipPlacement::Right) {
    x = m_placement == TooltipPlacement::Left ? m_anchor.x - size.width - Style::spaceSm
                                              : m_anchor.x + m_anchor.width + Style::spaceSm;
    y = m_anchor.y + (m_anchor.height - size.height) * 0.5F;
  }
  x = std::clamp(x, Style::spaceSm, std::max(Style::spaceSm, bounds.width - size.width - Style::spaceSm));
  y = std::clamp(y, Style::spaceSm, std::max(Style::spaceSm, bounds.height - size.height - Style::spaceSm));
  setVisible(true);
  arrange(renderer, {x, y, size.width, size.height});
}

void TooltipPresenter::sync(
    Renderer& renderer, Node& root, float pointerX, float pointerY, LayoutSize bounds,
    std::optional<luciole::FileTooltipTarget> file
) {
  if (!m_armed) {
    return;
  }
  InputArea* area = nullptr;
  for (auto* node = Node::hitTest(&root, pointerX, pointerY); node; node = node->parent()) {
    if (auto* input = dynamic_cast<InputArea*>(node); input && input->hasTooltip()) {
      area = input;
      break;
    }
  }
  TooltipContent content = file ? TooltipContent{} : area ? area->tooltipContent() : TooltipContent{};
  if (!file && !area) {
    dismiss();
    return;
  }
  LayoutRect anchor{};
  const auto placement = file ? TooltipPlacement::Default : area->tooltipPlacement();
  if (file) {
    anchor = {pointerX, pointerY, 0.0F, Style::spaceMd};
  } else {
    const auto* node = area->tooltipAnchorNode() ? area->tooltipAnchorNode() : area;
    anchor = {0, 0, node->width(), node->height()};
    Node::absolutePosition(node, anchor.x, anchor.y);
    float rootX = 0.0F, rootY = 0.0F;
    Node::absolutePosition(&root, rootX, rootY);
    // TooltipPresenter is arranged relative to `root`. Dialog roots can have a
    // non-zero scene position inside their popup frame, so convert the absolute
    // anchor back to the presenter's local coordinates before placement.
    anchor.x -= rootX;
    anchor.y -= rootY;
    if (area->hasTooltipAnchorInsets()) {
      const auto inset = area->tooltipAnchorInsets();
      anchor.x += inset.left;
      anchor.y += inset.top;
      anchor.width = std::max(1.0F, anchor.width - inset.left - inset.right);
      anchor.height = std::max(1.0F, anchor.height - inset.top - inset.bottom);
    }
  }
  const bool sameFile = file && m_file && luciole::sameTooltipFile(*file, *m_file);
  const bool changedStaticContent =
      !file && area->tooltipRefreshInterval().count() <= 0 && !sameContent(content, m_controlContent);
  if ((file.has_value() != m_file.has_value())
      || (file ? !sameFile : area != m_identity)
      || changedStaticContent
      || placement != m_placement
      || anchor.x != m_anchor.x
      || anchor.y != m_anchor.y
      || anchor.width != m_anchor.width
      || anchor.height != m_anchor.height) {
    m_fileData.clear();
    m_file = file;
    m_controlContent = content;
    m_identity = area;
    m_anchor = anchor;
    m_placement = placement;
    m_ready = false;
    m_refreshDue = false;
    m_refresh.stop();
    m_interval = {};
    setVisible(false);
    m_delay.start(kShowDelay, [this] {
      m_ready = true;
      m_requestUpdate();
    });
  }
  if (!m_ready) {
    return;
  }
  if (file) {
    m_fileData.show(*file);
    if (m_refreshDue) {
      m_fileData.refresh();
    }
    content = m_fileData.rows();
  }
  m_refreshDue = false;
  auto interval = file ? std::chrono::milliseconds(kFileRefresh) : area->tooltipRefreshInterval();
  if (interval.count() > 0) {
    interval = std::max(interval, std::chrono::milliseconds(100));
  }
  if (interval != m_interval) {
    m_refresh.stop();
    m_interval = interval;
  }
  if (interval.count() > 0 && !m_refresh.active()) {
    m_refresh.start(interval, [this] {
      m_refreshDue = true;
      m_requestUpdate();
    });
  }
  if (empty(content)) {
    setVisible(false);
    return;
  }
  populate(renderer, content, bounds);
  place(renderer, bounds);
}
