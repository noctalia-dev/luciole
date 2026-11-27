#include "ui/controls/folder_totals_view.h"

#include "fs/gio_ptr.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/size_format.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

FolderTotalsView::FolderTotalsView(
    std::span<const luciole::DirectoryEntry> targets, std::function<void()> requestSync
) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceXs);
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::label(
              {.text = i18n::text("Contents"),
               .fontSize = Style::fontSizeTitle,
               .fontWeight = FontWeight::Bold,
               .color = colorSpecFromRole(ColorRole::OnSurface),
               .flexGrow = 1}
          ),
          ui::button(
              {.out = &m_stop,
               .text = i18n::text("Stop"),
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Ghost,
               .tooltip = i18n::text("Stop calculating folder contents"),
               .onClick = [this, requestSync] {
                 m_service.cancel();
                 requestSync();
               }}
          )
      )
  );
  addChild(
      ui::label(
          {.out = &m_status,
           .text = i18n::text("Calculating…"),
           .fontSize = Style::fontSizeCaption,
           .maxLines = 3,
           .configure = literal}
      )
  );
  addChild(ui::label({.out = &m_counts, .fontSize = Style::fontSizeCaption, .maxLines = 3, .configure = literal}));
  addChild(ui::label({.out = &m_freeSpace, .fontSize = Style::fontSizeCaption, .maxLines = 2, .configure = literal}));
  addChild(
      ui::label(
          {.out = &m_note,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 8,
           .configure = literal}
      )
  );
  m_service.setChangedCallback(std::move(requestSync));
  (void)m_service.start(targets);
  m_freeSpace->setVisible(false);
  m_freeSpace->setParticipatesInLayout(false);
}

std::string folderTotalsSizeText(const luciole::FolderTotalsSnapshot& value) {
  if (value.overflow) {
    return std::string(i18n::text("Size overflow"));
  }
  if (value.bytes == 0 && value.partial()) {
    return std::string(i18n::text("Size unknown"));
  }
  const auto units = luciole::localizedSizeUnits();
  const std::string size = luciole::formatSizeWithUnits(value.bytes, units);
  return value.bytes < 1024 ? size : std::format("{} ({})", size, luciole::formatByteCount(value.bytes, units));
}

std::string folderTotalsCountsText(const luciole::FolderTotalsSnapshot& value) {
  std::string counts = std::format(
      "{} {} · {} {}", value.files, i18n::trp("file", value.files), value.folders, i18n::trp("folder", value.folders)
  );
  if (value.links) {
    counts += std::format(" · {} {}", value.links, i18n::trp("link", value.links));
  }
  if (value.other) {
    counts += std::format(" · {} {}", value.other, i18n::text("other"));
  }
  return counts;
}

void FolderTotalsView::sync(float width) {
  const auto& value = m_service.snapshot();
  const bool running = m_service.busy();
  const char* phase = running                                ? (value.cancelRequested ? "Stopping…" : "Calculating…")
      : value.phase == luciole::FolderTotalsPhase::Cancelled ? "Stopped · partial result"
      : value.partial()                                      ? "Partial result"
                                                             : "Calculated";
  m_status->setText(std::format("{} · {}", i18n::text(phase), folderTotalsSizeText(value)));
  m_counts->setText(folderTotalsCountsText(value));
  std::string note = i18n::text("Includes hidden items and subfolders. Links are not followed.");
  if (value.unavailable) {
    note += std::format("\n{} unreadable, missing or repeated folders/items.", value.unavailable);
  }
  if (value.unsupported) {
    note += std::format("\n{} non-native or Trash items could not be calculated.", value.unsupported);
  }
  if (value.changed) {
    note += "\nItems changed during inspection; totals may be outdated.";
  }
  if (value.limited) {
    note += "\nInspection limit reached; some contents were not counted.";
  }
  if (!value.error.empty()) {
    note += "\n" + value.error;
  }
  m_note->setText(note);
  const bool hasFreeSpace = !m_freeSpace->text().empty();
  m_freeSpace->setVisible(hasFreeSpace);
  m_freeSpace->setParticipatesInLayout(hasFreeSpace);
  for (auto* label : {m_status, m_counts, m_freeSpace, m_note}) {
    label->setMaxWidth(std::max(1.0F, width));
  }
  m_stop->setVisible(running);
  m_stop->setParticipatesInLayout(running);
  m_stop->setEnabled(!value.cancelRequested);
}

void FolderTotalsView::setFreeSpace(const std::string& text) { m_freeSpace->setText(text); }
