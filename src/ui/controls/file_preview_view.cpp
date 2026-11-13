#include "ui/controls/file_preview_view.h"

#include "fs/gio_ptr.h"
#include "ui/browser/preview_layout.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/file_metadata_view.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

FilePreviewView::FilePreviewView(
    luciole::FileIconCache& icons, std::function<void()> close, std::function<void()> open,
    std::function<void()> openWith
) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setPadding(Style::spaceMd);
  setGap(Style::spaceMd);
  setFill(colorSpecFromRole(ColorRole::Surface));
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .minHeight = 24.0F},
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
  auto body = ui::scrollView({.viewportPaddingH = 0, .viewportPaddingV = 0, .fillWidth = true, .flexGrow = 1});
  m_scroll = body.get();
  body->setReserveScrollbarGutter(true);
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
  body->content()->addChild(std::move(details));
  body->content()->addChild(ui::label({.out = &m_status, .maxLines = 3, .configure = literal}));
  body->content()->addChild(ui::label({.out = &m_text, .configure = [](Label& label) {
                                         label.setUseMarkup(false);
                                         label.setFontFamily("monospace");
                                         label.setMaxLines(400);
                                       }}));
  addChild(std::move(body));
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.out = &m_open,
               .text = "Open",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Ghost,
               .paddingH = Style::spaceSm,
               .onClick = std::move(open)}
          ),
          ui::button(
              {.out = &m_openWith,
               .text = "Open With…",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Ghost,
               .tooltip = "Open With…",
               .paddingH = Style::spaceSm,
               .onClick = std::move(openWith)}
          )
      )
  );
}
void FilePreviewView::sync(
    const luciole::PreviewSnapshot& snapshot, float width, float height, std::string_view typeLabel, bool quick
) {
  setPadding(Style::spaceMd, Style::spaceMd, quick ? Style::spaceMd : Style::borderWidth, Style::spaceMd);
  // Docked, the preview is part of the flush content plane; Quick Look floats as a card.
  if (quick) {
    setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    setRadius(Style::radiusXl);
  } else {
    clearBorder();
    setRadius(0);
  }
  const float budget = std::max(40.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
  m_heading->setMaxWidth(std::max(1.0F, width - Style::spaceMd * 2 - Style::controlHeightSm - Style::spaceSm));
  for (auto* button : {m_open, m_openWith}) {
    button->setMaxWidth(std::max(40.0F, width - Style::spaceMd * 2));
  }
  for (auto* label : {m_name, m_status, m_text}) {
    label->setMaxWidth(budget);
  }
  const auto& entry = snapshot.entry;
  const std::string location = entry ? entry->location.uri() : "";
  if (location != m_location) {
    m_location = location;
    m_scroll->requestScrollToOffset(0);
  }
  // The file name is the card's title; the empty-state hint reads as plain secondary text.
  m_name->setText(entry ? entry->displayName : "Select an item to preview");
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
    if (entry->modifiedSeconds && *entry->modifiedSeconds <= G_MAXINT64) {
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
          g_date_time_new_from_unix_local(static_cast<gint64>(*entry->modifiedSeconds)), &g_date_time_unref
      );
      if (date) {
        const luciole::GString formatted(g_date_time_format(date.get(), "%Y-%m-%d %H:%M"));
        fileFacts.push_back({"Modified", formatted.get()});
      }
    }
    m_image->bind(entry->icon, colorSpecFromRole(ColorRole::OnSurface));
  }
  const auto* pixels = snapshot.image && snapshot.image->pixels ? &*snapshot.image->pixels : nullptr;
  const auto image =
      luciole::previewImageLayout(pixels ? pixels->width : 0, pixels ? pixels->height : 0, width, height, 1, quick);
  m_image->setIconSize(image.width);
  m_image->setPreviewHeight(image.height);
  m_image->setPreparedImage(snapshot.image);
  const bool showImage = entry && snapshot.image && snapshot.image->pixels;
  m_image->parent()->setVisible(showImage);
  m_image->parent()->setParticipatesInLayout(showImage);
  metadata.insert(metadata.end(), snapshot.metadata.begin(), snapshot.metadata.end());
  metadata.insert(metadata.end(), fileFacts.begin(), fileFacts.end());
  m_metadata->sync(metadata, budget, true);
  m_status->setText(snapshot.message);
  m_status->setColor(colorSpecFromRole(
      snapshot.status == luciole::PreviewStatus::Error ? ColorRole::Error : ColorRole::OnSurfaceVariant
  ));
  m_text->setText(snapshot.text);
  m_text->setVisible(!snapshot.text.empty());
  m_text->setParticipatesInLayout(!snapshot.text.empty());
  m_open->setEnabled(entry.has_value());
  m_openWith->setEnabled(entry && entry->type == luciole::FileType::Regular && !entry->symlink);
}
void FilePreviewView::clear(Renderer& renderer) { m_image->releaseFile(renderer); }
