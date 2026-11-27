#include "app/page/status_bar_controller.h"

#include "app/page/file_keyboard_controller.h"
#include "app/page/page_context.h"
#include "app/page/settings_controller.h"
#include "app/workspace_layout.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "fs/gio_ptr.h"
#include "i18n/i18n.h"
#include "platform/desktop_integration.h"
#include "render/animation/animation_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/size_format.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace {
  constexpr float kRevealMs = 160.0F;
  // The hidden tab sits this far below its docked position; the page body clips it.
  constexpr float kRevealSlide = 6.0F;
  constexpr auto kMessageTimeout = std::chrono::seconds(5);
} // namespace

StatusBarController::StatusBarController(
    PageContext& page, AnimationManager& animations, luciole::PinService& pins, SettingsController& settings,
    FileKeyboardController& keys
)
    : m_page(page), m_animations(animations), m_pins(pins), m_settings(settings), m_keys(keys) {}

StatusBarController::~StatusBarController() { m_animations.cancelForOwner(this); }

void StatusBarController::setMessage(std::string message) {
  message = i18n::text(message);
  m_actionError = std::move(message);
  if (m_actionError.empty()) {
    m_messageTimer.stop();
    return;
  }
  m_messageTimer.start(kMessageTimeout, [this] {
    m_actionError.clear();
    m_page.requestSync();
  });
}

void StatusBarController::clearMessage() { setMessage({}); }

std::unique_ptr<Flex> StatusBarController::build() {
  auto pill = ui::row(
      {.out = &m_pill,
       .align = FlexAlign::Center,
       .gap = Style::spaceMd,
       .paddingV = Style::spaceXs,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
       .opacity = 0.0F,
       .visible = false,
       .participatesInLayout = false,
       .configure = [](Flex& tab) { tab.setStyleRadii(Radii(Style::radiusMd, Style::radiusMd, 0.0F, 0.0F)); }},
      ui::row(
          {.align = FlexAlign::Center},
          ui::label(
              {.out = &m_message,
               .fontSize = Style::fontSizeCaption,
               .maxLines = 1,
               .visible = false,
               .participatesInLayout = false,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          ),
          ui::label(
              {.out = &m_status,
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 1,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          )
      ),
      ui::label(
          {.out = &m_freeSpace,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = 160.0F,
           .maxLines = 1,
           .textAlign = TextAlign::End}
      )
  );
  pill->setZIndex(5);
  return pill;
}

std::unique_ptr<Flex> StatusBarController::buildNotice() {
  return ui::column(
      {.out = &m_noticeArea,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceSm,
       .padding = Style::spaceLg,
       .flexGrow = 1.0F},
      ui::glyph(
          {.out = &m_noticeGlyph,
           .glyph = "folder-open",
           .glyphSize = 48.0F,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
      ),
      ui::label(
          {.out = &m_notice,
           .fontSize = Style::fontSizeTitle,
           .fontWeight = FontWeight::SemiBold,
           .maxLines = 2,
           .textAlign = TextAlign::Center,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      ),
      ui::label(
          {.out = &m_noticeDetail,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 4,
           .textAlign = TextAlign::Center,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      )
  );
}

void StatusBarController::sync(bool miller) {
  const auto& browser = m_page.browser();
  const auto& model = browser.model();
  const auto& snapshot = model.snapshot();
  const auto context = browser.contextSerial();
  if (m_statusContext != context) {
    m_statusContext = context;
    m_contextLoaded = false;
    m_freeBytes.reset();
  }
  if (snapshot.status == luciole::DirectoryStatus::Ready) {
    m_contextLoaded = true;
  }
  const bool refreshing = snapshot.status == luciole::DirectoryStatus::Loading;
  const bool loading = refreshing && (!m_contextLoaded || snapshot.incrementalResults);
  const bool alwaysShow = m_page.settings().alwaysShowStatusBar;
  // An action message or failure stands out in its own label; definite failures use the error color.
  std::string message;
  auto role = ColorRole::OnSurface;
  std::string status;
  const auto append = [&status](std::string_view part) {
    status += status.empty() ? "" : " · ";
    status += part;
  };
  // The idle folder count stays hidden unless the setting keeps the pill visible.
  bool needed = true;
  if (!m_actionError.empty()) {
    message = m_actionError;
  } else if (!m_pins.sessionError().empty()) {
    message = std::format("{}: {}", i18n::text("Could not save session"), m_pins.sessionError());
    role = ColorRole::Error;
  } else if (!m_pins.error().empty()) {
    message = std::format("{}: {}", i18n::text("Could not update pins"), m_pins.error());
    role = ColorRole::Error;
  } else if (!m_settings.error().empty()) {
    message = m_settings.error();
    role = ColorRole::Error;
  } else if (!m_page.browser().message().empty()) {
    message = m_page.browser().message();
  } else if (snapshot.error) {
    message = snapshot.error->message;
    role = ColorRole::Error;
  } else if (!m_page.browser().searchQuery().empty()) {
    status = std::format(
        "{}{} {}",
        loading                                                      ? i18n::text("Searching… · ")
            : snapshot.status == luciole::DirectoryStatus::Cancelled ? i18n::text("Search stopped · ")
                                                                     : "",
        model.rows().size(), i18n::trp("result", model.rows().size())
    );
    if (m_page.browser().skippedSearchFolders() != 0) {
      status +=
          std::format(" · {} {}", m_page.browser().skippedSearchFolders(), i18n::text("folders could not be searched"));
    }
  } else if (loading) {
    status = snapshot.location && snapshot.location->isNetworkDiscovery() ? i18n::text("Finding network locations…")
                                                                          : i18n::text("Loading folder…");
  } else if (snapshot.status == luciole::DirectoryStatus::Cancelled) {
    status = i18n::text("Loading stopped.");
  } else {
    needed = alwaysShow;
    std::size_t hidden = 0;
    if (!model.filter().showHidden) {
      for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        hidden += snapshot.entries.brief(i).hidden;
      }
    }
    status = std::format("{} {}", model.rows().size(), i18n::trp("item", model.rows().size()));
    if (hidden > 0) {
      status += std::format(" · {} {}", hidden, i18n::text("hidden"));
    }
  }
  if (snapshot.location && snapshot.location->isTrash() && !snapshot.error && !refreshing) {
    append(i18n::text("Refresh to update"));
  }
  if (!model.selection().selected.empty()) {
    needed = true;
    append(std::format("{} {}", model.selection().selected.size(), i18n::text("selected")));
    const auto summary = model.selectionSummary();
    if (summary.pendingSizes != 0) {
      append("reading file sizes…");
    } else if (summary.overflow) {
      append("file size exceeds limit");
    } else if (summary.files != 0 && summary.unknownSizes == summary.files) {
      append("file size unknown");
    } else if (summary.files != 0) {
      append(
          std::format(
              "{}{}{}", summary.unknownSizes == 0 ? "" : "at least ",
              luciole::formatSizeWithLocalizedUnits(summary.bytes),
              summary.folders != 0 || summary.others != 0 ? " in files" : ""
          )
      );
    }
  }
  if (const auto typed = m_keys.typeaheadText(); !typed.empty()) {
    needed = true;
    status = "Select: " + typed + (status.empty() ? "" : " · " + status);
  }
  // A hiding tab keeps its last message while it fades out.
  if (needed) {
    m_message->setText(message);
    m_message->setColor(colorSpecFromRole(role));
    m_message->setVisible(!message.empty());
    m_message->setParticipatesInLayout(!message.empty());
    m_status->setText(!message.empty() && !status.empty() ? " · " + status : status);
  }
  // The notice repeats the message, or the status when there is none.
  status = message.empty() ? status : message;
  auto freeBytes = snapshot.metadata.filesystem.freeBytes;
  if (freeBytes) {
    m_freeBytes = freeBytes;
  } else if (snapshot.metadataLoading) {
    freeBytes = m_freeBytes;
  } else {
    m_freeBytes.reset();
  }
  m_freeSpaceAvailable =
      freeBytes && !snapshot.filesystemError && !snapshot.error && snapshot.location && !snapshot.location->isTrash();
  if (m_freeSpaceAvailable) {
    m_freeSpace->setText(std::format("{} {}", luciole::formatSizeWithLocalizedUnits(*freeBytes), i18n::text("free")));
  }
  if (std::exchange(m_needed, needed) != needed) {
    reveal();
  }
  // Empty list: a glyph, a short title and, when useful, one explanatory sentence.
  struct Notice {
    const char* glyph;
    std::string title, detail;
  };
  const auto notice = [&] -> Notice {
    if (snapshot.error) {
      return {
          "alert-triangle",
          snapshot.location && snapshot.location->isNetworkDiscovery() ? i18n::text("Network browsing unavailable")
                                                                       : i18n::text("Could not open this folder"),
          status
      };
    }
    if (loading) {
      return {"hourglass-empty", status, {}};
    }
    if (snapshot.status == luciole::DirectoryStatus::Cancelled
        || !m_page.browser().message().empty()
        || !m_settings.error().empty()) {
      return {"alert-triangle", status, {}};
    }
    if (const auto& query = m_page.browser().searchQuery(); !query.empty()) {
      return {
          "search", i18n::text("No results"), std::format("{} “{}”.", i18n::text("No files or folders match"), query)
      };
    }
    if (snapshot.entries.empty()) {
      if (snapshot.location && snapshot.location->isNetworkDiscovery()) {
        return {
            "network", i18n::text("No network locations found"),
            i18n::text("You can still connect by entering a server address in the location bar.")
        };
      }
      if (snapshot.location && snapshot.location->isDeviceRoot()) {
        return {
            "device-mobile", i18n::text("Device locked or transfer disabled"),
            i18n::text("Unlock your phone and select “File transfer” (MTP) in the USB settings.")
        };
      }
      return snapshot.location == luciole::Location::trash()
          ? Notice{"trash", i18n::text("Trash is empty"), {}}
          : Notice{"folder-open", i18n::text("This folder is empty"), {}};
    }
    return {"eye-off", i18n::text("No visible items"), i18n::text("Hidden and backup files are not shown.")};
  }();
  m_noticeGlyph->setGlyph(notice.glyph);
  m_notice->setText(notice.title);
  m_noticeDetail->setText(notice.detail);
  m_noticeDetail->setVisible(!notice.detail.empty());
  m_noticeDetail->setParticipatesInLayout(!notice.detail.empty());
  const bool empty = model.rows().empty();
  m_noticeArea->setVisible(empty && !miller);
  m_noticeArea->setParticipatesInLayout(empty && !miller);
}

void StatusBarController::layout(float paneWidth) {
  const bool showFreeSpace =
      m_freeSpaceAvailable && (paneWidth >= kMinimumFreeSpacePaneWidth || m_page.settings().alwaysShowStatusBar);
  m_freeSpace->setVisible(showFreeSpace);
  m_freeSpace->setParticipatesInLayout(showFreeSpace);
}

void StatusBarController::arrange(Renderer& renderer, float width, float height) {
  if (!m_pill->visible()) {
    return;
  }
  // Docked to the bottom edge of the page body at its trailing corner, clear of the vertical scrollbar.
  const float inset = Style::spaceSm;
  const float trailing = inset + Style::scrollbarWidth + Style::scrollbarGap;
  const float maximum = std::max(1.0F, width - inset - trailing);
  // Text budget inside the tab; the message gives way first so the status after it stays readable.
  float budget = maximum - 2.0F * Style::spaceMd;
  if (m_freeSpace->visible()) {
    budget -= Style::spaceMd + m_freeSpace->Node::measure(renderer, {}).width;
  }
  budget = std::max(1.0F, budget);
  m_status->setMaxWidth(budget);
  m_message->setMaxWidth(
      m_message->visible() ? std::max(1.0F, budget - m_status->Node::measure(renderer, {}).width) : 0.0F
  );
  LayoutConstraints constraints;
  constraints.setMaxWidth(maximum);
  const auto size = m_pill->measure(renderer, constraints);
  const float pillWidth = std::min(size.width, maximum);
  m_pill->arrange(
      renderer, {Style::rtl() ? trailing : width - trailing - pillWidth, height - size.height, pillWidth, size.height}
  );
}

void StatusBarController::reveal() {
  m_animations.cancelForOwner(this);
  if (m_needed) {
    m_pill->setVisible(true);
  }
  const float from = m_reveal;
  const float target = m_needed ? 1.0F : 0.0F;
  m_animations.animate(
      0.0F, 1.0F, kRevealMs, Easing::EaseOutCubic,
      [this, from, target](float progress) {
        m_reveal = std::lerp(from, target, progress);
        m_pill->setOpacity(m_reveal);
        m_pill->setTranslate(0.0F, (1.0F - m_reveal) * kRevealSlide);
        m_page.surface().requestRedraw();
      },
      [this] {
        m_pill->setVisible(m_needed);
        m_page.surface().requestLayout();
      },
      this
  );
}

bool StatusBarController::noticeShown() const { return m_notice != nullptr && m_noticeArea->visible(); }

LayoutRect StatusBarController::statusRect() const { return m_status ? sceneRect(*m_status) : LayoutRect{}; }
