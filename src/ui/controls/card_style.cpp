#include "ui/controls/card_style.h"

#include "ui/builders.h"

std::unique_ptr<Flex> luciole::card(const std::string& title, Flex** body, std::unique_ptr<Node> trailing) {
  auto result = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs, .configure = [](Flex& container) {
                              // setCardStyle sets its own padding; tighten it like noctalia's settings cards.
                              container.setCardStyle();
                              container.setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
                              container.setPadding(Style::spaceSm, Style::spaceMd);
                            }});
  if (!title.empty()) {
    auto heading = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm},
        ui::label(
            {.text = title,
             .fontSize = Style::fontSizeTitle,
             .fontWeight = FontWeight::Bold,
             .color = colorSpecFromRole(ColorRole::OnSurface),
             .flexGrow = 1.0F}
        )
    );
    if (trailing) {
      heading->addChild(std::move(trailing));
    }
    result->addChild(std::move(heading));
  }
  result->addChild(ui::column({.out = body, .align = FlexAlign::Stretch}));
  return result;
}
