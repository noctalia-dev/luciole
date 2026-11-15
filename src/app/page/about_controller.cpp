#include "app/page/about_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "imaging/image_limits.h"
#include "imaging/svg_rasterizer.h"
#include "luciole_app_icon.h"
#include "luciole_build_info.h"
#include "platform/file_launcher.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>

namespace {
  constexpr float kLogoSize = 96.0F;
} // namespace

AboutController::AboutController(PageContext& page, luciole::FileLauncher& launcher)
    : m_page(page), m_launcher(launcher) {}

void AboutController::open() {
  ScrollView* scroll = nullptr;
  Label* message = nullptr;
  Image* logo = nullptr;
  // Text width inside the dialog padding and the scroll view's reserved gutter.
  const float width =
      m_page.dialogWidth(DialogWidth::Medium) - Style::spaceMd * 2.0F - Style::scrollbarWidth - Style::scrollbarGap;
  auto body = ui::scrollView(
      {.out = &scroll,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .flexGrow = 1.0F,
       .configure = [](ScrollView& view) { view.setReserveScrollbarGutter(true); }}
  );
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->setGap(Style::spaceMd);
  body->content()->addChild(
      ui::row(
          {.justify = FlexJustify::Center},
          ui::image({.out = &logo, .fit = ImageFit::Contain, .width = kLogoSize, .height = kLogoSize})
      )
  );
  const auto text = [&](const std::string& value, bool caption = false) {
    body->content()->addChild(
        ui::label(
            {.text = value,
             .fontSize = caption ? Style::fontSizeCaption : Style::fontSizeBody,
             .maxWidth = width,
             .maxLines = 1000,
             .configure = [](Label& label) { label.setUseMarkup(false); }}
        )
    );
  };
  text(
      std::format(
          "Version {} · {}\nRevision {}\nBuilt with {}", LUCIOLE_VERSION, LUCIOLE_BUILD_TYPE, LUCIOLE_BUILD_REVISION,
          LUCIOLE_COMPILER
      )
  );
  const auto link = [this](const char* address) { m_launcher.open(luciole::Location::fromUri(address)); };
  body->content()->addChild(
      ui::row(
          {.gap = Style::spaceSm},
          ui::button(
              {.text = "Project website",
               .variant = ButtonVariant::Default,
               .onClick = [link] { link("https://noctalia.dev"); }}
          ),
          ui::button({.text = "Source code", .variant = ButtonVariant::Default, .onClick = [link] {
                        link("https://github.com/noctalia-dev/luciole");
                      }})
      )
  );
  body->content()->addChild(
      ui::label(
          {.out = &message,
           .color = colorSpecFromRole(ColorRole::Error),
           .maxWidth = width,
           .maxLines = 3,
           .visible = false,
           .participatesInLayout = false,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      )
  );
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("About Luciole", "info-circle", [this] { m_page.dismissMenu(); }), std::move(body)
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.menuSurface()) {
    m_page.activeDialog().bind(ActiveDialog::Kind::About, [this] {
      m_scroll = nullptr;
      m_message = nullptr;
      m_logo = nullptr;
    });
    m_scroll = scroll;
    m_message = message;
    m_logo = logo;
  }
}

void AboutController::layoutDialog() {
  if (!m_logo) {
    return;
  }
  auto& renderer = m_page.menuSurface()->renderTarget().renderer();
  const auto pixels = static_cast<int>(std::ceil(kLogoSize * std::max(1.0F, renderer.renderScale())));
  const std::span svg(reinterpret_cast<const std::uint8_t*>(luciole::kAppIconSvg), sizeof(luciole::kAppIconSvg) - 1);
  if (const auto image = luciole::rasterizeSvg(svg, pixels, false, luciole::kMaxImagePixels)) {
    m_logo->setSourceRaw(
        renderer, image->rgba.data(), image->rgba.size(), image->width, image->height, image->width * 4,
        PixmapFormat::RGBA
    );
  }
  m_logo = nullptr;
}

void AboutController::launcherChanged(const std::string& error) {
  if (m_message) {
    m_message->setText(error.empty() ? "" : std::format("Could not open the link: {}", error));
    m_message->setVisible(!error.empty());
    m_message->setParticipatesInLayout(!error.empty());
    m_page.menuSurface()->requestLayout();
  }
}

void AboutController::revealFocus(InputArea* focused) {
  if (m_scroll) {
    m_scroll->requestScrollIntoView(focused);
  }
}
