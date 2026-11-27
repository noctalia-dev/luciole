#include "ui/controls/file_preview_view.h"

#include "browser/location_presentation.h"
#include "fs/gio_ptr.h"
#include "i18n/i18n.h"
#include "ui/browser/preview_layout.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/card_style.h"
#include "ui/controls/file_metadata_view.h"
#include "ui/controls/folder_totals_view.h"
#include "ui/controls/label.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/size_format.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

namespace {
  // Type icon shown on the stage when the file has neither an image nor a text preview, also while one loads.
  constexpr float kFallbackIconSize = 128.0F;
  constexpr float kStatusPillHeight = 20.0F;

  // One compact contents line for a selected folder; Properties keeps the detailed inspection notes.
  std::string folderContentsLine(const luciole::FolderTotalsSnapshot& totals) {
    const char* phase = totals.phase == luciole::FolderTotalsPhase::Running ? "Calculating… · "
        : totals.partial()                                                  ? "Partial result · "
                                                                            : "";
    return std::format("{}{} · {}", phase, folderTotalsSizeText(totals), folderTotalsCountsText(totals));
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
                 .followsCloseButtons = true,
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
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  // Text previews read as a page on the stage, scrolling inside it so the facts below stay put.
  auto text = ui::scrollView(
      {.out = &m_textScroll,
       .viewportPaddingH = Style::spaceSm,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .flexGrow = 1}
  );
  // Caption size, like the Details values: more lines fit on the page.
  text->content()->addChild(
      ui::label({.out = &m_text, .fontSize = Style::fontSizeCaption, .configure = [](Label& label) {
                   label.setUseMarkup(false);
                   label.setFontFamily("monospace");
                   label.setMaxLines(400);
                 }})
  );
  // Full content width, styled like the Details card below so their edges line up.
  auto page = ui::column(
      {.out = &m_page,
       .align = FlexAlign::Stretch,
       .fillWidth = true,
       .clipChildren = true,
       .configure =
           [](Flex& page) {
             page.setCardStyle();
             page.setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
             // The page holds the vertical inset; the viewport clips at its own edge, so lines stop short of the
             // border.
             page.setPadding(Style::spaceSm, 0.0F);
           }},
      std::move(text)
  );
  // Every file shows in the same fixed stage, so stepping through files never moves the name or Details.
  body->content()->addChild(
      ui::column(
          {.out = &m_stage, .align = FlexAlign::Center, .justify = FlexJustify::Center}, std::move(image),
          std::move(page)
      )
  );
  body->content()->addChild(
      ui::label({.out = &m_name, .fontWeight = FontWeight::SemiBold, .maxLines = 3, .configure = literal})
  );
  auto details = std::make_unique<FileMetadataView>();
  m_metadata = details.get();
  Flex* card = nullptr;
  // A short preview status ("Truncated", "File changed") rides in the Details heading, taking no row of its own.
  auto detailsCard = luciole::card(
      "Details", &card,
      ui::row(
          {.out = &m_statusPill,
           .align = FlexAlign::Center,
           .paddingH = Style::spaceSm,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
           .radius = kStatusPillHeight / 2.0F,
           .height = kStatusPillHeight},
          ui::label({.out = &m_status, .fontSize = Style::fontSizeCaption, .maxLines = 1, .configure = literal})
      )
  );
  m_details = detailsCard.get();
  card->addChild(std::move(details));
  body->content()->addChild(std::move(detailsCard));
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
  m_name->setMaxWidth(budget);
  // Half the card: the "Details" heading keeps its room beside the badge.
  m_status->setMaxWidth(std::max(40.0F, budget / 2.0F - Style::spaceSm * 2));
  const auto stage = luciole::previewStage(width, height, m_quick);
  // The stage holds the image box plus room for its shadow; the text page fills all of it.
  const float stageHeight = stage.height + luciole::kPreviewImageInset * 2;
  m_stage->setMinHeight(stageHeight);
  m_stage->setMaxHeight(stageHeight);
  m_page->setMinHeight(stageHeight);
  m_page->setMaxHeight(stageHeight);
  m_text->setMaxWidth(std::max(40.0F, budget - Style::spaceSm * 2 - Style::scrollbarWidth - Style::scrollbarGap));
  const auto& entry = snapshot.entry;
  const std::string location = entry ? entry->location().uri() : "";
  if (location != m_location) {
    m_location = location;
    m_scroll->requestScrollToOffset(0);
    m_textScroll->requestScrollToOffset(0);
  }
  // The quick preview titles itself with the file name; the docked pane names it in the body. The empty-state hint
  // reads as plain secondary text.
  if (m_quick) {
    m_heading->setText(entry ? entry->displayName() : i18n::text("Preview"));
  }
  const bool showName = !m_quick || !entry;
  m_name->setVisible(showName);
  m_name->setParticipatesInLayout(showName);
  m_name->setText(entry ? entry->displayName() : i18n::text("Select an item to preview"));
  m_name->setFontWeight(entry ? FontWeight::SemiBold : FontWeight::Normal);
  m_name->setColor(colorSpecFromRole(entry ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant));
  luciole::MetadataFields metadata;
  luciole::MetadataFields fileFacts;
  if (entry) {
    const auto type = !typeLabel.empty()
        ? std::string(typeLabel)
        : luciole::fileTypeDescription(entry->type, entry->symlink, entry->contentType);
    metadata.push_back({"Type", i18n::text(type)});
    if (entry->size && entry->type != luciole::FileType::Directory) {
      metadata.push_back({"File Size", luciole::formatSizeWithLocalizedUnits(*entry->size)});
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
    fileFacts.push_back(
        {.label = "Location", .value = luciole::locationEditText(entry->location()), .singleLine = true}
    );
    m_image->bind(luciole::iconSource(entry->icon), colorSpecFromRole(ColorRole::OnSurface));
  }
  const auto* pixels = snapshot.image && snapshot.image->pixels ? &*snapshot.image->pixels : nullptr;
  if (pixels) {
    // The image box is the whole stage; FileIcon contains the picture in it and rounds the fitted rect.
    m_image->setIconSize(stage.width);
    m_image->setPreviewHeight(stage.height);
    m_image->setPreparedImage(snapshot.image);
  } else {
    // Shrinks with a short stage, like the image and text page, instead of overflowing it.
    m_image->setIconSize(std::min(kFallbackIconSize, std::max(16.0F, stage.height)));
    m_image->setPreviewHeight(0);
    m_image->setPreparedImage(nullptr);
  }
  const bool page = !pixels && !snapshot.text.empty();
  m_stage->setVisible(entry.has_value());
  m_stage->setParticipatesInLayout(entry.has_value());
  m_image->setVisible(!page);
  m_image->setParticipatesInLayout(!page);
  m_page->setVisible(page);
  m_page->setParticipatesInLayout(page);
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
  const bool status = entry.has_value() && !snapshot.message.empty();
  m_statusPill->setVisible(status);
  m_statusPill->setParticipatesInLayout(status);
  m_text->setText(snapshot.text);
  m_open->setEnabled(entry.has_value());
  m_openWith->setEnabled(entry && entry->type == luciole::FileType::Regular && !entry->symlink);
}
bool FilePreviewView::showsImage() const { return m_stage->visible() && m_image->visible(); }
void FilePreviewView::clear(Renderer& renderer) { m_image->releaseFile(renderer); }
