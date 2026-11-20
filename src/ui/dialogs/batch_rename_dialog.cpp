#include "ui/dialogs/batch_rename_dialog.h"

#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/dialog_chrome.h"

#include <algorithm>
#include <array>
#include <format>

namespace {
  using luciole::BatchRenameMode;
  constexpr std::array renameModes{BatchRenameMode::Numbering, BatchRenameMode::Replace, BatchRenameMode::Regex};
  constexpr std::array outcomeNames{"Renamed", "Unchanged", "Failed", "Cancelled", "Needs attention", "Unconfirmed"};
  class PreviewRow final : public Flex {
  public:
    PreviewRow() {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Stretch);
      setPadding(4, 6);
      setGap(2);
      addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceMd},
              ui::label(
                  {.out = &m_before,
                   .maxLines = 1,
                   .flexGrow = 1.0F,
                   .configure = [](Label& label) { label.setUseMarkup(false); }}
              ),
              ui::label({.out = &m_after, .maxLines = 1, .flexGrow = 1.0F, .configure = [](Label& label) {
                           label.setUseMarkup(false);
                         }})
          )
      );
      addChild(
          ui::label(
              {.out = &m_detail, .fontSize = Style::fontSizeCaption, .maxLines = 1, .configure = [](Label& label) {
                 label.setUseMarkup(false);
               }}
          )
      );
      setHitTestVisible(false);
    }
    void bind(const luciole::BatchRenameRow& row, const luciole::BatchRenameResult* result, bool hovered) {
      m_before->setText(row.before);
      m_after->setText(row.after);
      m_after->setColor(colorSpecFromRole(
          !row.error.empty() ? ColorRole::Error
              : row.changed  ? ColorRole::Primary
                             : ColorRole::OnSurfaceVariant
      ));
      m_detail->setText(
          !row.error.empty() ? row.error
              : result       ? outcomeNames[static_cast<std::size_t>(result->outcome)]
              : row.changed  ? ""
                             : "Unchanged"
      );
      m_detail->setColor(colorSpecFromRole(row.error.empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error));
      setFill(hovered ? colorSpecFromRole(ColorRole::SurfaceContainerHigh) : clearColorSpec());
    }

  private:
    void doLayout(Renderer& renderer) override {
      const float budget = std::max(1.0F, (width() - 12.0F - Style::spaceMd) * 0.5F);
      m_before->setMaxWidth(budget);
      m_before->setMinWidth(budget);
      m_after->setMaxWidth(budget);
      m_after->setMinWidth(budget);
      m_detail->setMaxWidth(std::max(1.0F, width() - 12.0F));
      Flex::doLayout(renderer);
    }
    Label *m_before = nullptr, *m_after = nullptr, *m_detail = nullptr;
  };
} // namespace
class BatchRenameDialog::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(BatchRenameDialog& owner) : m_owner(owner) {}
  std::size_t itemCount() const override { return m_owner.m_snapshot.rows ? m_owner.m_snapshot.rows->size() : 0; }
  std::unique_ptr<Node> createTile() override { return std::make_unique<PreviewRow>(); }
  void bindTile(Node& tile, std::size_t index, bool, bool hovered) override {
    const auto& snapshot = m_owner.m_snapshot;
    static_cast<PreviewRow&>(tile).bind(
        (*snapshot.rows)[index], snapshot.results ? &(*snapshot.results)[index] : nullptr, hovered
    );
  }
  std::string itemTooltip(std::size_t index) const override {
    const auto& snapshot = m_owner.m_snapshot;
    const auto& row = (*snapshot.rows)[index];
    std::string text = row.before + " → " + row.after;
    if (!row.error.empty()) {
      text += "\n" + row.error;
    }
    if (snapshot.results && (*snapshot.results)[index].actual) {
      text += "\n" + (*snapshot.results)[index].actual->uri();
    }
    return text;
  }

private:
  BatchRenameDialog& m_owner;
};
BatchRenameDialog::BatchRenameDialog(
    std::size_t count, std::function<void(luciole::BatchRenameOptions)> preview, std::function<void()> apply,
    std::function<void()> cancel, std::function<void()> close, std::function<void()> copyReport,
    std::function<void()> requestUpdate
)
    : m_adapter(std::make_unique<Adapter>(*this)), m_preview(std::move(preview)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  addChild(luciole::dialogHeader(std::format("Rename {} items", count), "forms", std::move(close)));
  auto scroll =
      ui::scrollView({.out = &m_scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .flexGrow = 1.0F});
  scroll->content()->setAlign(FlexAlign::Stretch);
  auto body = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm});
  body->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::segmented(
              {.out = &m_mode,
               .options =
                   std::vector<ui::SegmentedOption>{
                       {.label = "Numbering", .glyph = {}, .tooltip = {}},
                       {.label = "Find/replace", .glyph = {}, .tooltip = {}},
                       {.label = "Regex",
                        .glyph = {},
                        .tooltip = "Use an ECMAScript regular expression. In Replace, use $1, $2, … for captures."}
                   },
               .equalSegmentWidths = true,
               .flexGrow = 1.0F,
               .onChange = [this](std::size_t) { changed(); }}
          ),
          ui::toggle({.out = &m_extensions, .checkedImmediate = true, .onChange = [this](bool) { changed(); }}),
          ui::label({.text = "Keep extensions", .fontSize = Style::fontSizeCaption})
      )
  );
  const auto edit = [this](const std::string&) { changed(); };
  body->addChild(
      ui::column(
          {.out = &m_numbering, .align = FlexAlign::Stretch, .gap = Style::spaceSm},
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm}, ui::label({.text = "Pattern", .minWidth = 64.0F}),
              ui::input(
                  {.out = &m_pattern,
                   .value = "{name}_{number}",
                   .flexGrow = 1.0F,
                   .onChange = edit,
                   .configure =
                       [](Input& input) {
                         input.inputArea()->setTooltip(
                             "Use {name} for the original name and {number} for its sequence number. Double braces "
                             "produce literal braces."
                         );
                       }}
              )
          ),
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm}, ui::label({.text = "Start at"}),
              ui::stepper(
                  {.out = &m_start,
                   .minValue = 0,
                   .maxValue = 1000000000,
                   .value = 1,
                   .width = 150.0F,
                   .onValueChanged = [this](int) { changed(); }}
              ),
              ui::label({.text = "Digits"}),
              ui::stepper(
                  {.out = &m_digits,
                   .minValue = 1,
                   .maxValue = 9,
                   .value = 3,
                   .width = 130.0F,
                   .onValueChanged = [this](int) { changed(); }}
              )
          )
      )
  );
  body->addChild(
      ui::column(
          {.out = &m_replace,
           .align = FlexAlign::Stretch,
           .gap = Style::spaceSm,
           .visible = false,
           .participatesInLayout = false},
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm}, ui::label({.text = "Find", .minWidth = 64.0F}),
              ui::input({.out = &m_find, .flexGrow = 1.0F, .onChange = edit})
          ),
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm}, ui::label({.text = "Replace", .minWidth = 64.0F}),
              ui::input({.out = &m_replacement, .flexGrow = 1.0F, .onChange = edit})
          )
      )
  );
  body->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceMd},
          ui::label({.text = "Current name", .fontSize = Style::fontSizeCaption, .flexGrow = 1.0F}),
          ui::label({.text = "New name", .fontSize = Style::fontSizeCaption, .flexGrow = 1.0F})
      )
  );
  body->addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .height = 220.0F},
          ui::virtualGridView(
              {.out = &m_grid,
               .columns = 1,
               .cellHeight = 46.0F,
               .squareCells = false,
               .rowGap = 0.0F,
               .overscanRows = 1,
               .adapter = m_adapter.get(),
               .flexGrow = 1.0F}
          )
      )
  );
  scroll->content()->addChild(std::move(body));
  addChild(std::move(scroll));
  addChild(
      ui::label({.out = &m_status, .text = "Preparing selection…", .fontSize = Style::fontSizeCaption, .maxLines = 1})
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.out = &m_report,
               .text = "Copy report",
               .variant = ButtonVariant::Ghost,
               .visible = false,
               .participatesInLayout = false,
               .onClick = std::move(copyReport)}
          ),
          ui::button(
              {.out = &m_cancel, .text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = std::move(cancel)}
          ),
          ui::button(
              {.out = &m_apply,
               .text = "Rename",
               .enabled = false,
               .variant = ButtonVariant::Primary,
               .onClick = std::move(apply)}
          )
      )
  );
  auto tooltip = std::make_unique<TooltipPresenter>(std::move(requestUpdate));
  m_tooltip = tooltip.get();
  addChild(std::move(tooltip));
}
BatchRenameDialog::~BatchRenameDialog() { m_grid->setAdapter(nullptr); }
Input* BatchRenameDialog::firstInput() const { return m_mode->selectedIndex() == 0 ? m_pattern : m_find; }
luciole::BatchRenameOptions BatchRenameDialog::options() const {
  return {
      .mode = renameModes[m_mode->selectedIndex()],
      .pattern = m_pattern->value().substr(0, 4097),
      .find = m_find->value().substr(0, 4097),
      .replacement = m_replacement->value().substr(0, 4097),
      .start = m_start->value(),
      .digits = m_digits->value(),
      .keepExtensions = m_extensions->checked()
  };
}
void BatchRenameDialog::changed() {
  if (!m_pattern || !m_find || !m_replacement || !m_start || !m_digits || !m_extensions) {
    return;
  }
  const bool numbering = m_mode->selectedIndex() == 0;
  m_numbering->setVisible(numbering);
  m_numbering->setParticipatesInLayout(numbering);
  m_replace->setVisible(!numbering);
  m_replace->setParticipatesInLayout(!numbering);
  m_preview(options());
  markLayoutDirty();
}
void BatchRenameDialog::sync(
    const luciole::BatchRenameSnapshot& snapshot, std::size_t completed, bool clipboardAvailable
) {
  using P = luciole::BatchRenamePhase;
  const bool finished = snapshot.phase == P::Finished, applying = snapshot.phase == P::Applying;
  const bool editable = !finished && !applying;
  const bool rowsChanged = snapshot.rows != m_snapshot.rows || snapshot.results != m_snapshot.results;
  m_snapshot = snapshot;
  m_mode->setEnabled(editable);
  m_extensions->setEnabled(editable);
  for (auto* input : {m_pattern, m_find, m_replacement}) {
    input->setEnabled(editable);
  }
  m_start->setEnabled(editable);
  m_digits->setEnabled(editable);
  m_apply->setEnabled(snapshot.canApply && snapshot.phase == P::Ready);
  m_apply->setText(applying ? "Renaming…" : "Rename");
  m_cancel->setText(finished ? "Close" : "Cancel");
  m_report->setVisible(finished);
  m_report->setParticipatesInLayout(finished);
  m_report->setEnabled(clipboardAvailable && !snapshot.report.empty());
  const auto count = snapshot.rows ? snapshot.rows->size() : 0;
  std::string status = snapshot.error ? luciole::fileErrorText(*snapshot.error) : std::string{};
  bool error = !status.empty();
  if (status.empty()) {
    if (snapshot.phase == P::Preparing) {
      status = "Preparing selection…";
    } else if (snapshot.phase == P::Previewing) {
      status = "Checking names…";
    } else if (applying) {
      status = std::format("Renaming… · {} of {} items", completed, count);
    } else if (finished && snapshot.results) {
      for (std::size_t i = 0; i < outcomeNames.size(); ++i) {
        const auto amount = std::ranges::count(
            *snapshot.results, static_cast<luciole::BatchRenameOutcome>(i), &luciole::BatchRenameResult::outcome
        );
        if (amount > 0) {
          if (!status.empty()) {
            status += " · ";
          }
          status += std::format("{} {}", amount, outcomeNames[i]);
        }
      }
    } else if (snapshot.rows) {
      const auto errors = std::ranges::count_if(*snapshot.rows, [](const auto& row) { return !row.error.empty(); });
      const auto changes = std::ranges::count(*snapshot.rows, true, &luciole::BatchRenameRow::changed);
      error = errors > 0;
      status = errors > 0 ? std::format("{} names need attention", errors)
                          : std::format("{} of {} names will change", changes, count);
    }
  }
  m_status->setText(status);
  m_status->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  m_apply->setTooltip(status);
  m_grid->setOpacity(snapshot.phase == P::Previewing ? 0.6F : 1.0F);
  if (rowsChanged) {
    m_grid->notifyDataChanged();
  }
}

void BatchRenameDialog::reveal(const Node* node) { m_scroll->requestScrollIntoView(node); }
void BatchRenameDialog::tooltipPointer(float x, float y, bool arm) {
  m_pointerX = x;
  m_pointerY = y;
  if (arm) {
    m_tooltip->resume();
  } else {
    m_tooltip->dismiss();
  }
}
void BatchRenameDialog::syncTooltip(Renderer& renderer) {
  m_tooltip->sync(renderer, *this, m_pointerX, m_pointerY, {width(), height()});
}
