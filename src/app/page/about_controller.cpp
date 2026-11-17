#include "app/page/about_controller.h"

#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "imaging/image_limits.h"
#include "imaging/svg_rasterizer.h"
#include "luciole_app_icon.h"
#include "luciole_build_info.h"
#include "platform/file_launcher.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_dispatcher.h"
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
#include <string_view>

namespace {
  constexpr float kLogoSize = 112.0F;
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
      {.out = &scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F}
  );
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->setGap(Style::spaceSm);
  body->content()->addChild(
      ui::row(
          {.justify = FlexJustify::Center},
          ui::image({.out = &logo, .fit = ImageFit::Contain, .width = kLogoSize, .height = kLogoSize})
      )
  );
  const auto centered = [&](std::string value, float size, ColorRole role, FontWeight weight = FontWeight::Normal) {
    body->content()->addChild(
        ui::label(
            {.text = std::move(value),
             .fontSize = size,
             .fontWeight = weight,
             .color = colorSpecFromRole(role),
             .maxWidth = width,
             .maxLines = 3,
             .textAlign = TextAlign::Center,
             .configure = [](Label& label) { label.setUseMarkup(false); }}
        )
    );
  };
  centered("Luciole", Style::fontSizeHeader, ColorRole::OnSurface, FontWeight::SemiBold);
  centered("A smooth, native Wayland file manager", Style::fontSizeBody, ColorRole::OnSurfaceVariant);
  centered(std::format("Version {}", LUCIOLE_VERSION), Style::fontSizeBody, ColorRole::Primary, FontWeight::SemiBold);
  // Builds without a recorded revision omit it instead of printing "unknown".
  const std::string_view revision = LUCIOLE_BUILD_REVISION;
  centered(
      revision == "unknown" ? std::format("{} build · {}", LUCIOLE_BUILD_TYPE, LUCIOLE_COMPILER)
                            : std::format("{} build · {} · {}", LUCIOLE_BUILD_TYPE, revision, LUCIOLE_COMPILER),
      Style::fontSizeCaption, ColorRole::OnSurfaceVariant
  );
  const auto link = [this](const char* address) { m_launcher.open(luciole::Location::fromUri(address)); };
  const auto linkButton = [&](const char* label, const char* glyph, const char* address) {
    return ui::button({.text = label, .glyph = glyph, .variant = ButtonVariant::Default, .onClick = [link, address] {
                         link(address);
                       }});
  };
  body->content()->addChild(
      ui::row(
          {.justify = FlexJustify::Center, .gap = Style::spaceSm, .paddingV = Style::spaceSm},
          linkButton("Website", "world", "https://noctalia.dev"),
          linkButton("Source code", "brand-github", "https://github.com/noctalia-dev/luciole"),
          linkButton("Report an issue", "bug", "https://github.com/noctalia-dev/luciole/issues"),
          linkButton("Discord", "brand-discord", "https://discord.noctalia.dev")
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
      luciole::dialogHeader("About Luciole", "info-circle", [this] { m_page.dismissPopup(); }), std::move(body)
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface()) {
    // Nothing to act on by default: a focused link would read as selected. Tab still reaches the links.
    m_page.popupDispatcher().setFocus(nullptr);
    m_page.activePopup().bind(ActivePopup::Kind::About, [this] {
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
  auto& renderer = m_page.popupSurface()->renderTarget().renderer();
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
    m_page.popupSurface()->requestLayout();
  }
}

void AboutController::revealFocus(InputArea* focused) {
  if (m_scroll) {
    m_scroll->requestScrollIntoView(focused);
  }
}
