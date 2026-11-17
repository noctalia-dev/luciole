#include "ui/dialogs/dialog_chrome.h"

#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"

namespace luciole {

  void applyDialogStyle(Flex& root) {
    root.setFill(colorSpecFromRole(ColorRole::Surface));
    root.setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    root.setStyleRadius(Style::radiusLg);
    root.setPadding(Style::spaceMd);
    root.setGap(Style::spaceMd);
  }

  std::unique_ptr<Flex> dialogHeader(
      const std::string& title, const std::string& glyph, std::function<void()> close, Label** titleOut,
      std::unique_ptr<Node> actions
  ) {
    auto row = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm},
        ui::glyph({.glyph = glyph, .color = colorSpecFromRole(ColorRole::Primary)}),
        ui::label(
            {.out = titleOut,
             .text = title,
             .fontSize = Style::fontSizeTitle,
             .fontWeight = FontWeight::SemiBold,
             .maxLines = 1,
             .flexGrow = 1.0F,
             .configure = [](Label& label) { label.setUseMarkup(false); }}
        )
    );
    if (actions) {
      row->addChild(std::move(actions));
    }
    row->addChild(
        ui::button(
            {.glyph = "x",
             .glyphSize = Style::baseGlyphSize,
             .controlHeight = 24.0F,
             .variant = ButtonVariant::Ghost,
             .tooltip = "Close",
             .padding = Style::spaceXs,
             .width = 24.0F,
             .onClick = std::move(close),
             .configure = [](Button& button) { button.inputArea()->setTabFocusKey(std::string(kDialogCloseFocusKey)); }}
        )
    );
    return ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm}, std::move(row), ui::separator({}));
  }

  bool isDialogClose(const InputArea* area) { return area != nullptr && area->tabFocusKey() == kDialogCloseFocusKey; }

  void focusFirstDialogControl(InputDispatcher& dispatcher) {
    (void)dispatcher.cycleTabFocus(false);
    if (isDialogClose(dispatcher.focusedArea())) {
      (void)dispatcher.cycleTabFocus(false);
    }
    if (isDialogClose(dispatcher.focusedArea())) {
      dispatcher.setFocus(nullptr);
    }
  }

} // namespace luciole
