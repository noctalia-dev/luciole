#include "ui/controls/permission_results_view.h"

#include "browser/location_presentation.h"
#include "core/input/key_symbols.h"
#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/virtual_grid_view.h"

#include <algorithm>
#include <array>
#include <format>

namespace {
  using Outcome = luciole::PermissionOutcome;
  struct OutcomeStyle {
    const char* label;
    const char* glyph;
    ColorRole accent, badge, onBadge;
  };
  // Indexed by PermissionOutcome: changes read as primary, failures as error, everything else stays quiet.
  constexpr std::array<OutcomeStyle, 7> styles{{
      {"Ready", "circle-dot", ColorRole::OnSurfaceVariant, ColorRole::SurfaceContainerHigh,
       ColorRole::OnSurfaceVariant},
      {"Changed", "circle-check", ColorRole::Primary, ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer},
      {"Unchanged", "circle-minus", ColorRole::OnSurfaceVariant, ColorRole::SurfaceContainerHigh,
       ColorRole::OnSurfaceVariant},
      {"Failed", "alert-circle", ColorRole::Error, ColorRole::ErrorContainer, ColorRole::OnErrorContainer},
      {"Partially changed", "alert-triangle", ColorRole::Error, ColorRole::ErrorContainer, ColorRole::OnErrorContainer},
      {"Cancelled", "circle-x", ColorRole::OnSurfaceVariant, ColorRole::SurfaceContainerHigh,
       ColorRole::OnSurfaceVariant},
      {"Skipped", "player-skip-forward", ColorRole::OnSurfaceVariant, ColorRole::SurfaceContainerHigh,
       ColorRole::OnSurfaceVariant},
  }};
  constexpr float kGlyphSize = 18.0F;
  constexpr float kBadgeReserve = 140.0F;
  // One item: outcome glyph, name with an outcome badge, the path with mode and owner, then any note or error.
  class ResultRow final : public Flex {
  public:
    ResultRow() {
      setDirection(FlexDirection::Horizontal);
      // Rows share one cell height; centering keeps two-line items from leaving a gap at the bottom.
      setAlign(FlexAlign::Center);
      setPadding(Style::spaceSm, Style::spaceMd);
      setGap(Style::spaceSm);
      setStyleRadius(Style::radiusMd);
      const auto literal = [](Label& label) { label.setUseMarkup(false); };
      addChild(ui::glyph({.out = &m_glyph, .glyph = "circle-dot", .glyphSize = kGlyphSize}));
      addChild(
          ui::column(
              {.align = FlexAlign::Stretch, .gap = 2.0F, .flexGrow = 1.0F},
              ui::row(
                  {.align = FlexAlign::Center, .gap = Style::spaceSm},
                  ui::label(
                      {.out = &m_name,
                       .fontWeight = FontWeight::SemiBold,
                       .maxLines = 1,
                       .flexGrow = 1.0F,
                       .configure = literal}
                  ),
                  ui::row(
                      {.out = &m_badge,
                       .align = FlexAlign::Center,
                       .paddingV = 1.0F,
                       .paddingH = Style::spaceSm,
                       .styleRadius = Style::radiusMd},
                      ui::label(
                          {.out = &m_outcome, .fontSize = Style::fontSizeMini, .fontWeight = FontWeight::SemiBold}
                      )
                  )
              ),
              ui::label(
                  {.out = &m_facts,
                   .fontSize = Style::fontSizeCaption,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                   .maxLines = 1,
                   .configure =
                       [](Label& label) {
                         label.setUseMarkup(false);
                         // Keep the file end of long paths.
                         label.setEllipsize(TextEllipsize::Start);
                       }}
              ),
              ui::label({.out = &m_detail, .fontSize = Style::fontSizeCaption, .maxLines = 1, .configure = literal})
          )
      );
      setHitTestVisible(false);
    }
    void bind(const luciole::PermissionResult& row, bool hovered) {
      const auto& style = styles[static_cast<std::size_t>(row.outcome)];
      m_glyph->setGlyph(style.glyph);
      m_glyph->setColor(colorSpecFromRole(style.accent));
      m_name->setText(row.name);
      m_outcome->setText(style.label);
      m_outcome->setColor(colorSpecFromRole(style.onBadge));
      m_badge->setFill(colorSpecFromRole(style.badge));
      std::string facts = luciole::locationEditText(row.location);
      if (row.info) {
        const auto& info = *row.info;
        facts += std::format(
            "  ·  {}  ·  {}:{}", permissionModeText(info.mode),
            info.ownerName.empty() ? std::to_string(info.owner) : info.ownerName,
            info.groupName.empty() ? std::to_string(info.group) : info.groupName
        );
      }
      m_facts->setText(facts);
      // "No changes." on an Unchanged item only repeats the badge.
      std::string detail = row.error ? luciole::fileErrorText(*row.error) : "";
      if (!row.message.empty() && row.outcome != Outcome::Unchanged) {
        detail += (detail.empty() ? "" : " ") + row.message;
      }
      m_detail->setText(detail);
      m_detail->setVisible(!detail.empty());
      m_detail->setParticipatesInLayout(!detail.empty());
      const bool error = row.outcome == Outcome::Failed || row.outcome == Outcome::Partial;
      m_detail->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
      setFill(colorSpecFromRole(hovered ? ColorRole::SurfaceContainerHigh : ColorRole::SurfaceContainerLow));
    }

  private:
    void doLayout(Renderer& renderer) override {
      const float text = std::max(1.0F, width() - Style::spaceMd * 2 - kGlyphSize - Style::spaceSm);
      for (auto* label : {m_facts, m_detail}) {
        label->setMaxWidth(text);
      }
      m_name->setMaxWidth(std::max(1.0F, text - kBadgeReserve));
      Flex::doLayout(renderer);
    }
    Glyph* m_glyph = nullptr;
    Flex* m_badge = nullptr;
    Label *m_name = nullptr, *m_outcome = nullptr, *m_facts = nullptr, *m_detail = nullptr;
  };
} // namespace
class PermissionResultsView::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(PermissionResultsView& owner) : m_owner(owner) {}
  std::size_t itemCount() const override { return m_owner.m_results ? m_owner.m_results->size() : 0; }
  std::unique_ptr<Node> createTile() override { return std::make_unique<ResultRow>(); }
  void bindTile(Node& tile, std::size_t index, bool, bool hovered) override {
    static_cast<ResultRow&>(tile).bind((*m_owner.m_results)[index], hovered);
  }

private:
  PermissionResultsView& m_owner;
};
PermissionResultsView::PermissionResultsView() : m_adapter(std::make_unique<Adapter>(*this)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFlexGrow(1.0F);
  setFillWidth(true);
  setFillHeight(true);
  addChild(
      ui::virtualGridView(
          {.out = &m_grid,
           .columns = 1,
           // Fits the name, facts and one note line at the row's padding.
           .cellHeight = 76.0F,
           .squareCells = false,
           .rowGap = Style::spaceXs,
           .overscanRows = 1,
           .adapter = m_adapter.get(),
           .flexGrow = 1.0F}
      )
  );
}
bool PermissionResultsView::navigate(std::uint32_t key, std::uint32_t modifiers, bool preedit) {
  if (preedit || modifiers != 0 || !m_results || m_results->empty()) {
    return false;
  }
  const auto last = m_results->size() - 1;
  const auto current = std::min(m_grid->selectedIndex().value_or(0), last);
  auto next = current;
  const auto page = m_grid->pageItemStride();
  if (KeySymbol::isHome(key)) {
    next = 0;
  } else if (KeySymbol::isEnd(key)) {
    next = last;
  } else if (KeySymbol::isUp(key)) {
    next = current > 0 ? current - 1 : 0;
  } else if (KeySymbol::isDown(key)) {
    next = std::min(current + 1, last);
  } else if (KeySymbol::isPageUp(key)) {
    next = current > page ? current - page : 0;
  } else if (KeySymbol::isPageDown(key)) {
    next = std::min(current + page, last);
  } else {
    return false;
  }
  m_grid->setSelectedIndex(next, true);
  return true;
}
PermissionResultsView::~PermissionResultsView() { m_grid->setAdapter(nullptr); }
void PermissionResultsView::sync(std::shared_ptr<const std::vector<luciole::PermissionResult>> results) {
  if (results == m_results) {
    return;
  }
  m_results = std::move(results);
  m_grid->notifyDataChanged();
}
