#include "app/page/status_bar_controller.h"

#include "app/page/file_keyboard_controller.h"
#include "app/page/page_context.h"
#include "app/page/settings_controller.h"
#include "app/workspace_layout.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "fs/gio_ptr.h"
#include "ui/builders.h"
#include "ui/palette.h"

#include <algorithm>
#include <format>
#include <utility>

StatusBarController::StatusBarController(
    PageContext& page, luciole::PinService& pins, SettingsController& settings, FileKeyboardController& keys
)
    : m_page(page), m_pins(pins), m_settings(settings), m_keys(keys) {}

std::unique_ptr<Flex> StatusBarController::build() {
  auto pill = ui::row(
      {.out = &m_pill,
       .align = FlexAlign::Center,
       .gap = Style::spaceMd,
       .paddingV = Style::spaceXs,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
       .radius = Style::radiusXl,
       .border = colorSpecFromRole(ColorRole::OutlineVariant),
       .minHeight = Style::controlHeightSm,
       .visible = false,
       .participatesInLayout = false},
      ui::label(
          {.out = &m_status,
           .fontSize = Style::fontSizeCaption,
           .maxLines = 1,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
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
  if (m_statusBrowser != &browser || m_statusContext != context) {
    m_statusBrowser = &browser;
    m_statusContext = context;
    m_contextLoaded = false;
    m_freeBytes.reset();
  }
  if (snapshot.status == luciole::DirectoryStatus::Ready) {
    m_contextLoaded = true;
  }
  const bool refreshing = snapshot.status == luciole::DirectoryStatus::Loading;
  const bool loading = refreshing && !m_contextLoaded;
  const bool alwaysShow = m_page.settings().alwaysShowStatusBar;
  std::string status;
  // The idle folder count stays hidden unless the setting keeps the pill visible.
  bool needed = true;
  if (status.empty()) {
    if (!m_actionError.empty()) {
      status = m_actionError;
    } else if (!m_pins.sessionError().empty()) {
      status = std::format("Could not save session: {}", m_pins.sessionError());
    } else if (!m_pins.error().empty()) {
      status = std::format("Could not update pins: {}", m_pins.error());
    } else if (!m_settings.error().empty()) {
      status = m_settings.error();
    } else if (!m_page.browser().message().empty()) {
      status = m_page.browser().message();
    } else if (snapshot.error) {
      status = snapshot.error->message;
    } else if (!m_page.browser().searchQuery().empty()) {
      status = std::format(
          "{}{} results",
          loading                                                      ? "Searching… · "
              : snapshot.status == luciole::DirectoryStatus::Cancelled ? "Search stopped · "
                                                                       : "",
          model.rows().size()
      );
      if (m_page.browser().skippedSearchFolders() != 0) {
        status += std::format(" · {} folders could not be searched", m_page.browser().skippedSearchFolders());
      }
    } else if (loading) {
      status = "Loading folder…";
    } else if (snapshot.status == luciole::DirectoryStatus::Cancelled && !m_contextLoaded) {
      status = "Loading stopped.";
    } else {
      needed = alwaysShow;
      std::size_t hidden = 0;
      if (!model.filter().showHidden) {
        for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
          hidden += snapshot.entries.brief(i).hidden;
        }
      }
      status = std::format("{} items", model.rows().size());
      if (hidden > 0) {
        status += std::format(" · {} hidden", hidden);
      }
    }
  }
  if (snapshot.location && snapshot.location->isTrash() && !snapshot.error && !refreshing) {
    status += " · Refresh to update";
  }
  if (!model.selection().selected.empty()) {
    needed = true;
    status += std::format(" · {} selected", model.selection().selected.size());
    const auto summary = model.selectionSummary();
    if (summary.pendingSizes != 0) {
      status += " · reading file sizes…";
    } else if (summary.overflow) {
      status += " · file size exceeds limit";
    } else if (summary.files != 0 && summary.unknownSizes == summary.files) {
      status += " · file size unknown";
    } else if (summary.files != 0) {
      const luciole::GString size(g_format_size_full(summary.bytes, G_FORMAT_SIZE_IEC_UNITS));
      status += std::format(
          " · {}{}{}", summary.unknownSizes == 0 ? "" : "at least ", size.get(),
          summary.folders != 0 || summary.others != 0 ? " in files" : ""
      );
    }
  }
  if (const auto typed = m_keys.typeaheadText(); !typed.empty()) {
    needed = true;
    status = "Select: " + typed + " · " + status;
  }
  m_status->setText(status);
  m_needed = needed;
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
    const luciole::GString size(g_format_size_full(*freeBytes, G_FORMAT_SIZE_IEC_UNITS));
    m_freeSpace->setText(std::format("{} free", size.get()));
  }
  m_pill->setVisible(m_needed);
  // Empty list: a glyph, a short title and, when useful, one explanatory sentence.
  struct Notice {
    const char* glyph;
    std::string title, detail;
  };
  const auto notice = [&] -> Notice {
    if (snapshot.error) {
      return {"alert-triangle", "Could not open this folder", status};
    }
    if (loading) {
      return {"hourglass-empty", status, {}};
    }
    if ((snapshot.status == luciole::DirectoryStatus::Cancelled && !m_contextLoaded)
        || !m_page.browser().message().empty()
        || !m_settings.error().empty()) {
      return {"alert-triangle", status, {}};
    }
    if (const auto& query = m_page.browser().searchQuery(); !query.empty()) {
      return {"search", "No results", std::format("No files or folders match “{}”.", query)};
    }
    if (snapshot.entries.empty()) {
      return snapshot.location == luciole::Location::trash() ? Notice{"trash", "Trash is empty", {}}
                                                             : Notice{"folder-open", "This folder is empty", {}};
    }
    return {"eye-off", "No visible items", "Hidden and backup files are not shown."};
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
  // Floats over the bottom-right corner of the file card, clear of the vertical scrollbar.
  const float inset = Style::spaceSm;
  const float trailing = inset + Style::scrollbarWidth + Style::scrollbarGap;
  const float maximum = std::max(1.0F, width - inset - trailing);
  LayoutConstraints constraints;
  constraints.setMaxWidth(maximum);
  const auto size = m_pill->measure(renderer, constraints);
  const float pillWidth = std::min(size.width, maximum);
  m_pill->arrange(
      renderer,
      {Style::rtl() ? trailing : width - trailing - pillWidth, height - inset - size.height, pillWidth, size.height}
  );
}

bool StatusBarController::noticeShown() const { return m_notice != nullptr && m_noticeArea->visible(); }

LayoutRect StatusBarController::statusRect() const { return m_status ? sceneRect(*m_status) : LayoutRect{}; }
