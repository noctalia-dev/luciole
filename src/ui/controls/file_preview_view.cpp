#include "ui/controls/file_preview_view.h"

#include "fs/gio_ptr.h"
#include "ui/browser/preview_layout.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/card_style.h"
#include "ui/controls/file_metadata_view.h"
#include "ui/controls/label.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <iterator>

namespace {
  // Type icon shown in place of a preview that is loading or cannot render.
  constexpr float kFallbackIconSize = 96.0F;

  // One compact contents line for a selected folder; Properties keeps the detailed inspection notes.
  std::string folderContentsLine(const luciole::FolderTotalsSnapshot& totals) {
    const char* prefix = totals.phase == luciole::FolderTotalsPhase::Running
        ? (totals.cancelRequested ? "Stopping…" : "Calculating…")
        : totals.partial() ? "Partial result"
                           : "";
    std::string line = prefix;
    if (!line.empty()) {
      line += " · ";
    }
    if (totals.overflow) {
      line += "size overflow";
    } else if (totals.bytes == 0 && totals.partial()) {
      line += "size unknown";
    } else {
      const luciole::GString size(g_format_size_full(totals.bytes, G_FORMAT_SIZE_IEC_UNITS));
      line += totals.bytes < 1024 ? std::string(size.get()) : std::format("{} ({} bytes)", size.get(), totals.bytes);
    }
    std::format_to(
        std::back_inserter(line), " . {} {} . {} {}", totals.files, totals.files == 1 ? "file" : "files",
        totals.folders, totals.folders == 1 ? "folder" : "folders"
    );
    if (totals.links != 0) {
      std::format_to(std::back_inserter(line), " . {} {}", totals.links, totals.links == 1 ? "link" : "links");
    }
    if (totals.other != 0) {
      std::format_to(std::back_inserter(line), " . {} other", totals.other);
    }
    return line;
  }
} // namespace

FilePreviewView::FilePreviewView(
    luciole::FileIconCache& icons, bool quick, std::function<void()> close, std::function<void()> open,
    std::function<void()> openWith
)
    : m_quick(quick) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  if (quick) {
    // The quick preview is a dialog: same chrome, header, and footer as the others.
    luciole::applyDialogStyle(*this);
    addChild(luciole::dialogHeader("Preview", "eye", std::move(close), &m_heading));
  } else {
    // Docked, the preview is part of the flush content plane.
    setPadding(Style::spaceMd);
    setGap(Style::spaceMd);
    setFill(colorSpecFromRole(ColorRole::Surface));
    addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm, .minHeight = 24.0F},
            ui::glyph({.glyph = "eye", .color = colorSpecFromRole(ColorRole::Primary)}),
            ui::label(
                {.out = &m_heading, .text = "Preview", .fontWeight = FontWeight::SemiBold, .maxLines = 1, .flexGrow = 1}
            ),
            ui::button(
                {.glyph = "x",
                 .glyphSize = Style::baseGlyphSize,
                 .controlHeight = 24.0F,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = "Close preview",
                 .padding = Style::spaceXs,
                 .width = 24.0F,
                 .onClick = std::move(close)}
            )
        )
    );
  }
  auto body = ui::scrollView({.viewportPaddingH = 0, .viewportPaddingV = 0, .fillWidth = true, .flexGrow = 1});
  m_scroll = body.get();
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->setGap(Style::spaceMd);
  auto image = std::make_unique<luciole::FileIcon>(icons, 192);
  m_image = image.get();
  body->content()->addChild(ui::row({.justify = FlexJustify::Center}, std::move(image)));
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  body->content()->addChild(
      ui::label({.out = &m_name, .fontWeight = FontWeight::SemiBold, .maxLines = 3, .configure = literal})
  );
  auto details = std::make_unique<FileMetadataView>();
  m_metadata = details.get();
  Flex* card = nullptr;
  auto detailsCard = luciole::card("Details", &card);
  m_details = detailsCard.get();
  card->addChild(std::move(details));
  body->content()->addChild(std::move(detailsCard));
  body->content()->addChild(
      ui::label({.out = &m_status, .fontSize = Style::fontSizeCaption, .maxLines = 3, .configure = literal})
  );
  body->content()->addChild(ui::label({.out = &m_text, .configure = [](Label& label) {
                                         label.setUseMarkup(false);
                                         label.setFontFamily("monospace");
                                         label.setMaxLines(400);
                                       }}));
  addChild(std::move(body));
  if (quick) {
    addChild(
        ui::row(
            {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
            ui::button(
                {.out = &m_openWith,
                 .text = "Open With…",
                 .variant = ButtonVariant::Ghost,
                 .onClick = std::move(openWith)}
            ),
            ui::button(
                {.out = &m_open,
                 .text = "Open",
                 .variant = ButtonVariant::Primary,
                 .minWidth = 88.0F,
                 .onClick = std::move(open)}
            )
        )
    );
    return;
  }
  // The pane is narrow: both actions share its width rather than huddling at one edge.
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::button(
              {.out = &m_open,
               .text = "Open",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Default,
               .paddingH = Style::spaceSm,
               .flexGrow = 1.0F,
               .onClick = std::move(open)}
          ),
          ui::button(
              {.out = &m_openWith,
               .text = "Open With…",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Outline,
               .tooltip = "Open With…",
               .paddingH = Style::spaceSm,
               .flexGrow = 1.0F,
               .onClick = std::move(openWith)}
          )
      )
  );
}
void FilePreviewView::sync(
    const luciole::PreviewSnapshot& snapshot, float width, float height, std::string_view typeLabel
) {
  const float budget = std::max(40.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
  // Both headers hold a glyph ahead of the title.
  m_heading->setMaxWidth(
      std::max(1.0F, width - Style::spaceMd * 2 - Style::controlHeightSm - Style::spaceSm * 2 - Style::baseGlyphSize)
  );
  for (auto* button : {m_open, m_openWith}) {
    button->setMaxWidth(std::max(40.0F, width - Style::spaceMd * 2));
  }
  for (auto* label : {m_name, m_status, m_text}) {
    label->setMaxWidth(budget);
  }
  const auto& entry = snapshot.entry;
  const std::string location = entry ? entry->location().uri() : "";
  if (location != m_location) {
    m_location = location;
    m_scroll->requestScrollToOffset(0);
  }
  // The quick preview titles itself with the file name; the docked pane names it in the body. The empty-state hint
  // reads as plain secondary text.
  if (m_quick) {
    m_heading->setText(entry ? entry->displayName() : "Preview");
  }
  const bool showName = !m_quick || !entry;
  m_name->setVisible(showName);
  m_name->setParticipatesInLayout(showName);
  m_name->setText(entry ? entry->displayName() : "Select an item to preview");
  m_name->setFontWeight(entry ? FontWeight::SemiBold : FontWeight::Normal);
  m_name->setColor(colorSpecFromRole(entry ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant));
  luciole::MetadataFields metadata;
  luciole::MetadataFields fileFacts;
  if (entry) {
    const auto type = !typeLabel.empty()
        ? std::string(typeLabel)
        : luciole::fileTypeDescription(entry->type, entry->symlink, entry->contentType);
    metadata.push_back({"Type", type});
    if (entry->size && entry->type != luciole::FileType::Directory) {
      const luciole::GString size(g_format_size_full(*entry->size, G_FORMAT_SIZE_IEC_UNITS));
      metadata.push_back({"File size", size.get()});
    }
    if (snapshot.totals.phase != luciole::FolderTotalsPhase::Idle) {
      metadata.push_back({"Contents", folderContentsLine(snapshot.totals)});
    }
    if (entry->modifiedSeconds && *entry->modifiedSeconds <= G_MAXINT64) {
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
          g_date_time_new_from_unix_local(static_cast<gint64>(*entry->modifiedSeconds)), &g_date_time_unref
      );
      if (date) {
        const luciole::GString formatted(g_date_time_format(date.get(), "%Y-%m-%d %H:%M"));
        fileFacts.push_back({"Modified", formatted.get()});
      }
    }
    m_image->bind(luciole::iconSource(entry->icon), colorSpecFromRole(ColorRole::OnSurface));
  }
  const auto* pixels = snapshot.image && snapshot.image->pixels ? &*snapshot.image->pixels : nullptr;
  if (pixels) {
    const auto image = luciole::previewImageLayout(pixels->width, pixels->height, width, height, 1, m_quick);
    m_image->setIconSize(image.width);
    m_image->setPreviewHeight(image.height);
    m_image->setPreparedImage(snapshot.image);
  } else {
    // Without a rendered preview the type icon stands in, also while one loads.
    m_image->setIconSize(kFallbackIconSize);
    m_image->setPreviewHeight(0);
    m_image->setPreparedImage(nullptr);
  }
  // A text preview speaks for itself; the icon would only push it down.
  const bool showImage = entry && (pixels || snapshot.text.empty());
  m_image->parent()->setVisible(showImage);
  m_image->parent()->setParticipatesInLayout(showImage);
  m_details->setVisible(entry.has_value());
  m_details->setParticipatesInLayout(entry.has_value());
  metadata.insert(metadata.end(), snapshot.metadata.begin(), snapshot.metadata.end());
  metadata.insert(metadata.end(), fileFacts.begin(), fileFacts.end());
  // The table sits inside the Details card, which pads it on both sides.
  m_metadata->sync(metadata, std::max(40.0F, budget - Style::spaceMd * 2), true);
  m_status->setText(snapshot.message);
  m_status->setColor(colorSpecFromRole(
      snapshot.status == luciole::PreviewStatus::Error ? ColorRole::Error : ColorRole::OnSurfaceVariant
  ));
  m_status->setVisible(!snapshot.message.empty());
  m_status->setParticipatesInLayout(!snapshot.message.empty());
  m_text->setText(snapshot.text);
  m_text->setVisible(!snapshot.text.empty());
  m_text->setParticipatesInLayout(!snapshot.text.empty());
  m_open->setEnabled(entry.has_value());
  m_openWith->setEnabled(entry && entry->type == luciole::FileType::Regular && !entry->symlink);
}
bool FilePreviewView::showsImage() const { return m_image->parent()->visible(); }
void FilePreviewView::clear(Renderer& renderer) { m_image->releaseFile(renderer); }
