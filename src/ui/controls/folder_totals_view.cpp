#include "ui/controls/folder_totals_view.h"

#include "fs/gio_ptr.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
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
              {.text = "Contents",
               .fontSize = Style::fontSizeTitle,
               .fontWeight = FontWeight::Bold,
               .color = colorSpecFromRole(ColorRole::OnSurface),
               .flexGrow = 1}
          ),
          ui::button(
              {.out = &m_stop,
               .text = "Stop",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Ghost,
               .tooltip = "Stop calculating folder contents",
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
           .text = "Calculating…",
           .fontSize = Style::fontSizeCaption,
           .maxLines = 3,
           .configure = literal}
      )
  );
  addChild(ui::label({.out = &m_counts, .fontSize = Style::fontSizeCaption, .maxLines = 3, .configure = literal}));
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
}

std::string folderTotalsSizeText(const luciole::FolderTotalsSnapshot& value) {
  if (value.overflow) {
    return "Size overflow";
  }
  if (value.bytes == 0 && value.partial()) {
    return "Size unknown";
  }
  const luciole::GString size(g_format_size_full(value.bytes, G_FORMAT_SIZE_IEC_UNITS));
  return value.bytes < 1024 ? std::string(size.get()) : std::format("{} ({} bytes)", size.get(), value.bytes);
}

std::string folderTotalsCountsText(const luciole::FolderTotalsSnapshot& value) {
  std::string counts = std::format(
      "{} {} · {} {}", value.files, value.files == 1 ? "file" : "files", value.folders,
      value.folders == 1 ? "folder" : "folders"
  );
  if (value.links) {
    counts += std::format(" · {} {}", value.links, value.links == 1 ? "link" : "links");
  }
  if (value.other) {
    counts += std::format(" · {} other", value.other);
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
  m_status->setText(std::format("{} · {}", phase, folderTotalsSizeText(value)));
  m_counts->setText(folderTotalsCountsText(value));
  std::string note = "Includes hidden items and subfolders. Links are not followed.";
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
  for (auto* label : {m_status, m_counts, m_note}) {
    label->setMaxWidth(std::max(1.0F, width));
  }
  m_stop->setVisible(running);
  m_stop->setParticipatesInLayout(running);
  m_stop->setEnabled(!value.cancelRequested);
}
