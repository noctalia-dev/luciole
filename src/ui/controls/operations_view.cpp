#include "ui/controls/operations_view.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <utility>

class OperationsView::Row final : public Flex {
public:
  Row(std::uint64_t id, const Action& action) {
    setDirection(FlexDirection::Vertical);
    setAlign(FlexAlign::Stretch);
    setPadding(Style::spaceXs, Style::spaceSm);
    setGap(Style::spaceSm);
    addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm},
            ui::glyph({.out = &m_glyph, .glyph = "progress", .glyphSize = 18.0F}),
            ui::label({.out = &m_title, .fontWeight = FontWeight::SemiBold, .maxLines = 1, .flexGrow = 1.0F}),
            ui::spinner({.out = &m_spinner, .spinnerSize = 16.0F, .visible = false, .participatesInLayout = false})
        )
    );
    addChild(ui::label({.out = &m_summary, .fontSize = Style::fontSizeCaption, .maxLines = 2}));
    addChild(ui::progressBar({.out = &m_progress, .height = 4.0F}));
    const auto button = [&](Button** out, const std::string& text, ActivityAction choice) {
      return ui::button(
          {.out = out,
           .text = text,
           .controlHeight = Style::controlHeightSm,
           .variant = ButtonVariant::Ghost,
           .onClick = [action, id, choice] { action(id, choice); }}
      );
    };
    addChild(
        ui::row(
            {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceXs},
            button(&m_details, "Details", ActivityAction::Details), button(&m_cancel, "Cancel", ActivityAction::Cancel),
            button(&m_review, "Review", ActivityAction::Review), button(&m_retry, "Retry Failed", ActivityAction::Retry)
        )
    );
    addChild(ui::separator({}));
    m_title->setUseMarkup(false);
    m_summary->setUseMarkup(false);
  }
  void bind(const luciole::JobActivity& activity) {
    m_title->setText(activity.title);
    // A running transfer carries a meters line with the measured speed and totals.
    m_summary->setText(activity.meters.empty() ? activity.summary : activity.meters);
    m_summary->setColor(colorSpecFromRole(activity.failed() ? ColorRole::Error : ColorRole::OnSurfaceVariant));
    m_glyph->setGlyph(
        activity.failed()                                         ? "alert-circle"
            : activity.state == luciole::ActivityState::Cancelled ? "x"
            : activity.finished                                   ? "check"
            : activity.state == luciole::ActivityState::Waiting   ? "clock"
                                                                  : "progress"
    );
    m_glyph->setColor(colorSpecFromRole(activity.failed() ? ColorRole::Error : ColorRole::Primary));
    const bool spinning = !activity.finished
        && !activity.progress
        && (activity.state == luciole::ActivityState::Preparing || activity.state == luciole::ActivityState::Working);
    m_spinner->setVisible(spinning);
    m_spinner->setParticipatesInLayout(spinning);
    if (spinning) {
      m_spinner->start();
    } else {
      m_spinner->stop();
    }
    m_progress->setVisible(activity.progress.has_value() && !activity.finished);
    m_progress->setParticipatesInLayout(activity.progress.has_value() && !activity.finished);
    m_progress->setProgress(activity.progress.value_or(0.0F));
    for (const auto& [button, shown] :
         {std::pair(m_cancel, activity.canCancel), std::pair(m_review, activity.canReview),
          std::pair(m_retry, activity.canRetry)}) {
      button->setVisible(shown);
      button->setParticipatesInLayout(shown);
    }
  }

private:
  void setLabelWidths(float available) {
    m_title->setMaxWidth(std::max(1.0F, available - 56.0F));
    m_summary->setMaxWidth(std::max(1.0F, available - 2 * Style::spaceXs));
  }
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
    setLabelWidths(constraints.hasMaxWidth ? constraints.maxWidth : 484.0F);
    return Flex::doMeasure(renderer, constraints);
  }
  void doArrange(Renderer& renderer, const LayoutRect& rect) override {
    setLabelWidths(rect.width);
    Flex::doArrange(renderer, rect);
  }
  void doLayout(Renderer& renderer) override {
    setLabelWidths(width());
    Flex::doLayout(renderer);
  }
  Label *m_title = nullptr, *m_summary = nullptr;
  Glyph* m_glyph = nullptr;
  Spinner* m_spinner = nullptr;
  ProgressBar* m_progress = nullptr;
  Button *m_details = nullptr, *m_cancel = nullptr, *m_review = nullptr, *m_retry = nullptr;
};

OperationsView::OperationsView(Action action) : m_action(std::move(action)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFillWidth(true);
  setFlexGrow(1.0F);
  addChild(
      ui::scrollView(
          {.out = &m_scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F}
      )
  );
  m_scroll->content()->setAlign(FlexAlign::Stretch);
}
OperationsView::~OperationsView() = default;

bool OperationsView::sync(std::vector<luciole::JobActivity> activities) {
  const bool structural = activities.size() != m_activities.size()
      || !std::ranges::equal(activities, m_activities, {}, &luciole::JobActivity::id, &luciole::JobActivity::id);
  m_structureDirty |= structural;
  m_activities = std::move(activities);
  return structural;
}

void OperationsView::syncRows() {
  if (std::exchange(m_structureDirty, false)) {
    m_rows.clear();
    while (!m_scroll->content()->children().empty()) {
      (void)m_scroll->content()->removeChild(m_scroll->content()->children().back().get());
    }
    for (const auto& activity : m_activities) {
      auto row = std::make_unique<Row>(activity.id, m_action);
      m_rows.push_back(row.get());
      m_scroll->content()->addChild(std::move(row));
    }
    if (m_activities.empty()) {
      m_scroll->content()->addChild(
          ui::label(
              {.text = "No file operations", .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1}
          )
      );
    }
  }
  for (std::size_t index = 0; index < m_rows.size(); ++index) {
    m_rows[index]->bind(m_activities[index]);
  }
}

void OperationsView::doLayout(Renderer& renderer) {
  syncRows();
  Flex::doLayout(renderer);
}

void OperationsView::revealFocus(InputArea* focus) { m_scroll->requestScrollIntoView(focus); }

LayoutSize OperationsView::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  syncRows();
  return Flex::doMeasure(renderer, constraints);
}
void OperationsView::doArrange(Renderer& renderer, const LayoutRect& rect) {
  syncRows();
  Flex::doArrange(renderer, rect);
}
