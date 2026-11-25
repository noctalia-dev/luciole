#include "ui/browser/file_view.h"

#include "browser/directory_tree.h"
#include "browser/entry_details.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "fs/entry_access.h"
#include "fs/gio_ptr.h"
#include "model/details_columns.h"
#include "render/animation/animation_manager.h"
#include "ui/browser/file_icon.h"
#include "ui/browser/file_view_metrics.h"
#include "ui/builders.h"
#include "ui/controls/selection_contour.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string>
#include <utility>

namespace {
  constexpr float kHiddenIconOpacity = 0.6F;
  constexpr float kHiddenTextOpacity = 0.7F;
  constexpr float kRevealOffset = 8.0F;

  std::string modifiedTime(std::optional<std::uint64_t> seconds, const char* format) {
    if (!seconds || *seconds > static_cast<std::uint64_t>(G_MAXINT64)) {
      return "Unknown";
    }
    const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
        g_date_time_new_from_unix_local(static_cast<gint64>(*seconds)), &g_date_time_unref
    );
    if (!date) {
      return "Unknown";
    }
    const luciole::GString text(g_date_time_format(date.get(), format));
    return text ? text.get() : "Unknown";
  }

  std::string fileSize(const luciole::DirectoryEntry& entry) {
    std::string size;
    if (entry.type != luciole::FileType::Directory && entry.size) {
      constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
      auto value = static_cast<double>(*entry.size);
      std::size_t unit = 0;
      while (value >= 1024 && unit + 1 < std::size(units)) {
        value /= 1024;
        ++unit;
      }
      size = unit == 0 ? std::format("{} B", *entry.size) : std::format("{:.1f} {}", value, units[unit]);
    } else if (entry.type != luciole::FileType::Directory) {
      size = "Unknown";
    }
    return size;
  }
  // Adapted from Noctalia's FileEntryRow, with URI-backed entries and model selection.
  class FileRow final : public InputArea {
  public:
    FileRow(luciole::FileIconCache& icons, luciole::ThumbnailService& thumbnails, bool miller = false)
        : m_miller(miller) {
      setHitTestVisible(false);
      addChild(ui::box({.out = &m_background, .styleRadius = SelectionContour::kBaseCornerRadius}));
      auto icon = std::make_unique<luciole::FileIcon>(icons, 24.0F, &thumbnails);
      m_icon = icon.get();
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .configure = [](Flex& container) { container.setPadding(0, Style::spaceSm); }},
          ui::row(
              {.out = &m_treePrefix, .justify = FlexJustify::End, .visible = false, .participatesInLayout = false},
              ui::glyph({.out = &m_expander, .glyph = "chevron-right", .glyphSize = 14.0F})
          ),
          std::move(icon),
          ui::column(
              {.out = &m_nameColumn, .align = FlexAlign::Stretch},
              ui::label(
                  {.out = &m_name,
                   .maxLines = 1,
                   .baselineMode = LabelBaselineMode::TextFixedHeight,
                   .configure = [](Label& label) { label.setUseMarkup(false); }}
              ),
              ui::label(
                  {.out = &m_parent, .fontSize = Style::fontSizeCaption, .maxLines = 1, .configure = [](Label& label) {
                     label.setUseMarkup(false);
                   }}
              )
          )
      );
      for (const auto column : luciole::kDefaultDetailsColumnOrder) {
        row->addChild(
            ui::label(
                {.out = &m_details[static_cast<std::size_t>(column)], .maxLines = 1, .configure = [](Label& label) {
                   label.setUseMarkup(false);
                 }}
            )
        );
      }
      row->addChild(ui::glyph({.out = &m_arrow, .glyph = "chevron-right", .glyphSize = 14.0F}));
      addChild(std::move(row));
    }

    void bind(
        const luciole::DirectoryEntry& entry, std::string_view typeLabel, bool selected, bool ring, bool hovered,
        luciole::DetailsColumns columns, luciole::DetailsColumnOrder order, bool cut, bool path, float iconSize,
        luciole::BrowsePresentation presentation, bool readOnly, const luciole::TreeRowInfo& tree,
        std::string_view folderSize
    ) {
      m_tree = tree;
      m_treePrefix->setVisible(tree.enabled);
      m_treePrefix->setParticipatesInLayout(tree.enabled);
      m_expander->setVisible(tree.expandable);
      m_expander->setGlyph(tree.expanded ? "chevron-down" : Style::rtl() ? "chevron-left" : "chevron-right");
      m_expander->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_columns = columns;
      m_columnOrder = order;
      m_icon->setOpacity(cut ? 0.4F : entry.hidden ? kHiddenIconOpacity : 1.0F);
      for (auto* label : {m_name, m_parent}) {
        label->setOpacity(entry.hidden && !selected ? kHiddenTextOpacity : 1.0F);
      }
      const bool directory = entry.type == luciole::FileType::Directory;
      m_arrow->setVisible(m_miller && directory);
      m_arrow->setParticipatesInLayout(m_miller && directory);
      m_arrow->setColor(colorSpecFromRole(path ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
      m_icon->bindFile(entry, colorSpecFromRole(ColorRole::OnSurface));
      m_iconSize = iconSize;
      m_icon->setIconSize(m_iconSize);
      m_icon->setEmblems(presentation.showEmblems && entry.symlink, presentation.showEmblems && readOnly);
      m_name->setText(tree.status.empty() ? entry.displayName() : entry.displayName() + " · " + tree.status);
      m_name->setEllipsize(
          presentation.filenameTruncation == luciole::FilenameTruncation::Middle ? TextEllipsize::Middle
                                                                                 : TextEllipsize::End
      );
      m_parent->setText(entry.parentDisplayName());
      m_parent->setVisible(!entry.parentDisplayName().empty());
      m_parent->setParticipatesInLayout(!entry.parentDisplayName().empty());
      m_parent->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      for (const auto column : luciole::kDefaultDetailsColumnOrder) {
        auto* label = m_details[static_cast<std::size_t>(column)];
        const bool folderSizeCell = directory && !entry.symlink && column == luciole::DetailsColumn::Size;
        label->setText(
            columns.visible(column)
                ? (folderSizeCell ? std::string(folderSize) : luciole::entryColumnText(entry, column, typeLabel))
                : ""
        );
        label->setOpacity(entry.hidden && !selected ? kHiddenTextOpacity : 1.0F);
        label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      }
      m_name->setColor(colorSpecFromRole(path ? ColorRole::Primary : ColorRole::OnSurface));
      m_background->setFill(
          !selected && (hovered || path) ? colorSpecFromRole(ColorRole::SurfaceContainerHigh) : clearColorSpec()
      );
      if (ring) {
        m_background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_background->clearBorder();
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_columnOrder != m_appliedColumnOrder) {
        for (const auto column : m_columnOrder) {
          Node* label = m_details[static_cast<std::size_t>(column)];
          m_row->addChild(m_row->removeChild(label));
        }
        m_appliedColumnOrder = m_columnOrder;
      }
      const auto& columns = m_columns;
      for (const auto column : luciole::kDefaultDetailsColumnOrder) {
        auto* label = m_details[static_cast<std::size_t>(column)];
        label->setVisible(columns.visible(column));
        label->setParticipatesInLayout(columns.visible(column));
        label->setMinWidth(columns.width(column));
        label->setMaxWidth(columns.width(column));
      }
      const float prefix =
          m_tree.enabled ? luciole::treePrefixWidth(m_tree.depth, columns.nameWidth, m_iconSize) : 0.0F;
      m_treePrefix->setMinWidth(prefix);
      m_treePrefix->setMaxWidth(prefix);
      const float nameWidth = m_miller
          ? std::max(32.0F, width() - m_iconSize - 64.0F)
          : std::max(1.0F, columns.nameWidth - m_iconSize - 16.0F - (m_tree.enabled ? prefix + Style::spaceSm : 0.0F));
      m_nameColumn->setMinWidth(nameWidth);
      m_nameColumn->setMaxWidth(nameWidth);
      m_name->setMaxWidth(nameWidth);
      m_parent->setMaxWidth(nameWidth);
      m_background->arrange(renderer, {0, 0, width(), height()});
      m_row->arrange(renderer, {0, 0, width(), height()});
      updateContentSpan(renderer, nameWidth);
    }

  public:
    void setThumbnailVisible(bool visible) { m_icon->setThumbnailVisible(visible); }
    void releaseFile(Renderer& renderer) { m_icon->releaseFile(renderer); }
    bool contentContains(float x) const { return x >= m_contentStart && x <= m_contentEnd; }

  private:
    void updateContentSpan(Renderer& renderer, float nameWidth) {
      if (m_name->text() != m_measuredName
          || m_parent->text() != m_measuredParent
          || nameWidth != m_measuredColumnWidth) {
        const auto textWidth = [&renderer, nameWidth](const Label& label) {
          if (label.text().empty()) {
            return 0.0F;
          }
          return std::min(
              nameWidth,
              renderer
                  .measureText(
                      label.text(), label.fontSize(), label.font(), nameWidth, 1, TextAlign::Start, label.ellipsize()
                  )
                  .width
          );
        };
        m_nameTextWidth = textWidth(*m_name);
        m_parentTextWidth = textWidth(*m_parent);
        m_measuredName = m_name->text();
        m_measuredParent = m_parent->text();
        m_measuredColumnWidth = nameWidth;
      }
      constexpr float kContentSlack = 4.0F;
      const float textWidth = std::max(m_nameTextWidth, m_parent->visible() ? m_parentTextWidth : 0.0F);
      const float columnX = m_row->x() + m_nameColumn->x();
      // Labels draw start-aligned text from their right edge in RTL.
      const float textLeft = Style::rtl() ? columnX + m_nameColumn->width() - textWidth : columnX;
      const float iconLeft = m_row->x() + m_icon->x();
      float start = std::min(iconLeft, textLeft);
      float end = std::max(iconLeft + m_icon->width(), textLeft + textWidth);
      if (m_treePrefix->visible() && m_expander->visible()) {
        const float expanderLeft = m_row->x() + m_treePrefix->x() + m_expander->x();
        start = std::min(start, expanderLeft);
        end = std::max(end, expanderLeft + m_expander->width());
      }
      m_contentStart = start - kContentSlack;
      m_contentEnd = end + kContentSlack;
    }

    Box* m_background = nullptr;
    Flex* m_row = nullptr;
    Flex* m_treePrefix = nullptr;
    Glyph* m_expander = nullptr;
    luciole::TreeRowInfo m_tree;
    Flex* m_nameColumn = nullptr;
    luciole::FileIcon* m_icon = nullptr;
    Label* m_name = nullptr;
    Label* m_parent = nullptr;
    std::array<Label*, luciole::kDetailsColumnCount> m_details{};
    Glyph* m_arrow = nullptr;
    bool m_miller;
    float m_iconSize = 24.0F;
    luciole::DetailsColumns m_columns;
    luciole::DetailsColumnOrder m_columnOrder = luciole::kDefaultDetailsColumnOrder;
    luciole::DetailsColumnOrder m_appliedColumnOrder = luciole::kDefaultDetailsColumnOrder;
    // Row-local x span of the expander, icon and drawn name text; the rest of the row acts as empty space.
    float m_contentStart = 0.0F;
    float m_contentEnd = std::numeric_limits<float>::max();
    std::string m_measuredName;
    std::string m_measuredParent;
    float m_measuredColumnWidth = -1.0F;
    float m_nameTextWidth = 0.0F;
    float m_parentTextWidth = 0.0F;
  };

  // Adapted from Noctalia's FileEntryTile without its picker or thumbnail service.
  class FileTile final : public InputArea {
  public:
    FileTile(luciole::FileIconCache& icons, luciole::ThumbnailService& thumbnails) {
      setHitTestVisible(false);
      addChild(ui::box({.out = &m_background, .styleRadius = SelectionContour::kBaseCornerRadius}));
      m_icon =
          static_cast<luciole::FileIcon*>(addChild(std::make_unique<luciole::FileIcon>(icons, 48.0F, &thumbnails)));
      addChild(
          ui::label(
              {.out = &m_name,
               .fontSize = Style::fontSizeBody,
               .maxLines = 2,
               .textAlign = TextAlign::Center,
               .baselineMode = LabelBaselineMode::TextFixedHeight,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          )
      );
      addChild(
          ui::label(
              {.out = &m_parent,
               .fontSize = Style::fontSizeCaption,
               .maxLines = 1,
               .textAlign = TextAlign::Center,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          )
      );
      for (auto& caption : m_captions) {
        addChild(
            ui::label(
                {.out = &caption,
                 .fontSize = Style::fontSizeCaption,
                 .maxLines = 1,
                 .textAlign = TextAlign::Center,
                 .configure = [](Label& label) { label.setUseMarkup(false); }}
            )
        );
      }
    }

    void bind(
        const luciole::DirectoryEntry& entry, std::string_view typeLabel, bool selected, bool ring, bool hovered,
        float iconSize, bool cut, luciole::BrowsePresentation presentation, bool readOnly
    ) {
      m_icon->setOpacity(cut ? 0.4F : entry.hidden ? kHiddenIconOpacity : 1.0F);
      for (auto* label : {m_name, m_parent, m_captions[0], m_captions[1], m_captions[2]}) {
        label->setOpacity(entry.hidden && !selected ? kHiddenTextOpacity : 1.0F);
      }
      m_presentation = presentation;
      m_iconSize = iconSize;
      m_icon->setIconSize(iconSize);
      m_icon->bindFile(entry, colorSpecFromRole(ColorRole::OnSurface));
      m_icon->setEmblems(presentation.showEmblems && entry.symlink, presentation.showEmblems && readOnly);
      const std::array captionText{
          presentation.captions.size ? fileSize(entry) : std::string{},
          presentation.captions.type ? std::string(typeLabel) : std::string{},
          presentation.captions.modified ? modifiedTime(entry.modifiedSeconds, "%Y-%m-%d") : std::string{}
      };
      for (std::size_t i = 0; i < m_captions.size(); ++i) {
        auto* caption = m_captions[i];
        caption->setText(captionText[i]);
        caption->setVisible(!captionText[i].empty());
        caption->setParticipatesInLayout(!captionText[i].empty());
        caption->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      }
      m_name->setText(entry.displayName());
      m_name->setEllipsize(
          presentation.filenameTruncation == luciole::FilenameTruncation::Middle ? TextEllipsize::Middle
                                                                                 : TextEllipsize::End
      );
      m_parent->setText(entry.parentDisplayName());
      m_parent->setMaxWidth(std::max(0.0F, width() - Style::spaceSm * 2));
      m_parent->setVisible(!entry.parentDisplayName().empty());
      m_parent->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_name->setMaxWidth(std::max(0.0F, width() - Style::spaceSm * 2));
      m_name->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_background->setFill(
          !selected && hovered ? colorSpecFromRole(ColorRole::SurfaceContainerHigh) : clearColorSpec()
      );
      if (ring) {
        m_background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_background->clearBorder();
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      const auto metrics = luciole::gridMetrics(m_iconSize, m_presentation, m_parent->visible());
      const float labelWidth = std::max(1.0F, width() - metrics.inset * 2);
      m_background->arrange(renderer, {0, 0, width(), height()});
      m_icon->arrange(renderer, {(width() - m_iconSize) * 0.5F, metrics.inset, m_iconSize, m_iconSize});
      m_name->setMaxWidth(labelWidth);
      LayoutConstraints nameConstraints;
      nameConstraints.setExactWidth(labelWidth);
      const float nameHeight = std::clamp(m_name->Node::measure(renderer, nameConstraints).height, 1.0F, 40.0F);
      m_name->arrange(renderer, {metrics.inset, metrics.nameTop, labelWidth, nameHeight});
      float y = metrics.nameTop + nameHeight;
      for (auto* caption : m_captions) {
        if (caption->visible()) {
          caption->setMaxWidth(labelWidth);
          caption->arrange(renderer, {metrics.inset, y, labelWidth, 18.0F});
          y += 18.0F;
        }
      }
      if (m_parent->visible()) {
        m_parent->setMaxWidth(labelWidth);
        m_parent->arrange(renderer, {metrics.inset, y, labelWidth, 20.0F});
      }
    }

  public:
    void setThumbnailVisible(bool visible) { m_icon->setThumbnailVisible(visible); }
    void releaseFile(Renderer& renderer) { m_icon->releaseFile(renderer); }

  private:
    Box* m_background = nullptr;
    luciole::FileIcon* m_icon = nullptr;
    Label* m_name = nullptr;
    Label* m_parent = nullptr;
    float m_iconSize = 48.0F;
    luciole::BrowsePresentation m_presentation;
    std::array<Label*, 3> m_captions{};
  };
} // namespace

luciole::FileViewAdapter::FileViewAdapter(
    BrowserController& browser, FileIconCache& icons, ThumbnailService& thumbnails, ViewMode mode,
    std::function<std::uint32_t()> modifiers, std::function<bool()> focused
)
    : m_browser(browser), m_icons(icons), m_thumbnails(thumbnails), m_mode(mode), m_modifiers(std::move(modifiers)),
      m_focused(std::move(focused)) {}

std::size_t luciole::FileViewAdapter::itemCount() const { return m_browser.model().rows().size(); }
std::unique_ptr<Node> luciole::FileViewAdapter::createTile() {
  if (m_mode == ViewMode::Grid) {
    return std::make_unique<FileTile>(m_icons, m_thumbnails);
  }
  return std::make_unique<FileRow>(m_icons, m_thumbnails, m_mode == ViewMode::Miller);
}

std::unique_ptr<Node> luciole::FileViewAdapter::createDecoration() { return std::make_unique<SelectionContour>(); }
void luciole::FileViewAdapter::bindDecoration(Node& decoration, const VirtualGridDecorationLayout& layout) {
  const auto& model = m_browser.model();
  std::vector<std::uint8_t> selected(((layout.itemCount + layout.columns - 1) / layout.columns) * layout.columns, 0);
  for (std::size_t i = 0; i < layout.itemCount && layout.firstIndex + i < model.rows().size(); ++i) {
    const auto col = i % layout.columns;
    const auto visual = (i / layout.columns) * layout.columns + (layout.rtl ? layout.columns - col - 1 : col);
    selected[visual] =
        m_columnActive && model.selection().selected.contains(model.rows()[layout.firstIndex + i]) ? 1 : 0;
  }
  decoration.setVisible(std::ranges::any_of(selected, [](auto value) { return value != 0; }));
  static_cast<SelectionContour&>(decoration)
      .bind(
          std::move(selected), layout.columns, layout.cellWidth + layout.columnGap, layout.cellHeight + layout.rowGap
      );
}

std::string luciole::FileViewAdapter::itemTooltip(std::size_t index) const {
  const auto& model = m_browser.model();
  if (m_mode == ViewMode::Grid || index >= model.rows().size()) {
    return {};
  }
  const auto id = model.rows()[index];
  const auto* entry = model.entry(id);
  return entry != nullptr ? std::format("{}\n{}", entry->displayName(), model.typeLabel(id)) : std::string{};
}

void luciole::FileViewAdapter::setTileVisible(Node& tile, bool visible) {
  if (m_mode == ViewMode::Grid) {
    static_cast<FileTile&>(tile).setThumbnailVisible(visible);
  } else {
    static_cast<FileRow&>(tile).setThumbnailVisible(visible);
  }
  auto& state = m_tiles[&tile];
  state.visible = visible;
  // shortcut: the grid reports visibility before rebinding, so a recycled slot may mark its previous entry revealed.
  if (visible && state.entry) {
    reveal(tile, *state.entry);
  }
}
void luciole::FileViewAdapter::releaseTile(Node& tile, Renderer& renderer) {
  m_tiles.erase(&tile);
  if (m_mode == ViewMode::Grid) {
    static_cast<FileTile&>(tile).releaseFile(renderer);
  } else {
    static_cast<FileRow&>(tile).releaseFile(renderer);
  }
}
void luciole::FileViewAdapter::reveal(Node& tile, EntryId id) {
  if (m_mode == ViewMode::Miller || tile.animationManager() == nullptr) {
    return;
  }
  if (m_revealVisit != m_browser.visitSerial()) {
    m_revealVisit = m_browser.visitSerial();
    m_revealed.clear();
  }
  if (!m_revealed.insert(id).second) {
    return;
  }
  auto apply = [&tile](float progress) {
    tile.setOpacity(progress);
    tile.setTranslate(0.0F, kRevealOffset * (1.0F - progress));
  };
  apply(0.0F); // The manager only calls the setter from the next tick.
  tile.animationManager()->animate(0.0F, 1.0F, Style::animNormal, Easing::EaseOutCubic, apply, {}, &tile);
}
void luciole::FileViewAdapter::bindTile(Node& tile, std::size_t index, bool, bool hovered) {
  const auto& model = m_browser.model();
  if (index >= model.rows().size()) {
    return;
  }
  const auto id = model.rows()[index];
  auto& state = m_tiles[&tile];
  if (state.entry != id) {
    // A recycled slot may still be fading in its previous entry.
    if (tile.animationManager() != nullptr) {
      tile.animationManager()->cancelForOwner(&tile);
    }
    tile.setOpacity(1.0F);
    tile.setTranslate(0.0F, 0.0F);
    state.entry = id;
    if (state.visible) {
      reveal(tile, id);
    }
  }
  if (m_mode == ViewMode::List) {
    std::uint8_t details = 0;
    for (const auto column : kDefaultDetailsColumnOrder) {
      if (m_columns.visible(column)) {
        details |= entryColumnDetails(column);
      }
    }
    model.requestDetails(id, details);
  }
  const auto* entry = model.entry(id);
  if (entry != nullptr) {
    const bool readOnly = luciole::entryAccess(*entry).write == false || m_readOnly == true;
    const bool selected = m_columnActive && model.selection().selected.contains(id);
    // The keyboard cursor stays visible inside a multi-selection; a lone selected item shows only its fill.
    const auto& selection = model.selection();
    const bool ring = m_focused() && selection.focused == id && (!selected || selection.selected.size() != 1);
    if (m_mode == ViewMode::Grid) {
      static_cast<FileTile&>(tile).bind(
          *entry, model.typeLabel(id), selected, ring, hovered, m_gridIconSize, m_cut && m_cut(entry->location()),
          m_presentation, readOnly
      );
    } else {
      std::string folderSize;
      const bool sizeColumnVisible = m_columns.visible(luciole::DetailsColumn::Size);
      if (entry->type == luciole::FileType::Directory && !entry->symlink && m_folderSizes && sizeColumnVisible) {
        m_browser.requestFolderSize(id);
        if (const auto* totals = m_browser.folderSize(id)) {
          folderSize = entryFolderSizeText(*totals);
        }
      }
      static_cast<FileRow&>(tile).bind(
          *entry, model.typeLabel(id), selected, ring, hovered, m_columns, m_columnOrder,
          m_cut && m_cut(entry->location()), m_pathEntry == id && !m_columnActive,
          m_mode == ViewMode::Miller ? millerRowIconSize(m_presentation, m_rowIconSize)
                                     : fileRowIconSize(m_rowIconSize),
          m_presentation, readOnly, m_mode == ViewMode::List ? m_browser.treeRow(id) : TreeRowInfo{}, folderSize
      );
    }
  }
}

bool luciole::FileViewAdapter::onPointerPress(std::size_t index, float x, float, float width, float) {
  if (index >= itemCount()) {
    return true;
  }
  const auto modifiers = m_modifiers();
  const auto id = m_browser.model().rows()[index];
  const auto tree = m_mode == ViewMode::List ? m_browser.treeRow(id) : TreeRowInfo{};
  const float startX = Style::rtl() ? width - x : x;
  const float prefix = treePrefixWidth(tree.depth, m_columns.nameWidth, fileRowIconSize(m_rowIconSize));
  if (tree.expandable
      && modifiers == 0
      && startX >= Style::spaceSm + prefix - 20.0F
      && startX < Style::spaceSm + prefix) {
    onPointerCancel();
    m_expanderPress = id;
    return true;
  }
  m_expanderPress.reset();
  m_pressed = id;
  m_dragged = false;
  m_pressModifiers = modifiers;
  m_activationClick = modifiers == 0 || modifiers == KeyMod::Alt;
  const bool inspectColumn = m_mode == ViewMode::Miller && modifiers == KeyMod::Alt;
  m_collapseSelectionOnClick =
      m_activationClick && !inspectColumn && m_browser.model().selection().selected.contains(*m_pressed);
  m_toggleSelectionOnClick = modifiers == KeyMod::Ctrl && m_browser.model().selection().selected.contains(*m_pressed);
  const auto mode = (modifiers & KeyMod::Shift) != 0
      ? ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::AddRange : SelectionMode::Range)
      : ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::Toggle : SelectionMode::Replace);
  // Miller follows folder selection immediately. Alt activation inspects its explicit target without changing
  // that path; an actual drag still selects its source before publication.
  if (!inspectColumn && !m_collapseSelectionOnClick && !m_toggleSelectionOnClick) {
    (void)m_browser.model().select(*m_pressed, mode);
  }
  return true;
}

bool luciole::FileViewAdapter::onPointerDrag(std::optional<std::size_t>, float, float, float, float) {
  if (m_expanderPress) {
    m_expanderPress.reset();
    m_pressed.reset();
    return false;
  }
  const bool first = !m_dragged;
  m_dragged = true;
  if (first && m_drag && m_pressed && m_browser.model().entry(*m_pressed) != nullptr) {
    if (m_mode == ViewMode::Miller
        && m_pressModifiers == KeyMod::Alt
        && !m_browser.model().selection().selected.contains(*m_pressed)) {
      m_browser.model().select(*m_pressed);
    }
    m_drag(m_browser.model().selectedIds());
  }
  return false;
}

VirtualGridAdapter::PointerReleaseResult luciole::FileViewAdapter::onPointerRelease(std::optional<std::size_t> index) {
  if (const auto expander = std::exchange(m_expanderPress, std::nullopt)) {
    m_lastClick.reset();
    if (index && *index < itemCount() && m_browser.model().rows()[*index] == *expander) {
      (void)m_browser.toggleFolder(*expander);
    }
    return {.rebind = true, .activateIndex = std::nullopt};
  }
  m_areaSelection = false;
  m_areaRows.clear();
  m_areaBaseline.clear();
  const auto pressed = std::exchange(m_pressed, std::nullopt);
  const bool dragged = std::exchange(m_dragged, false);
  if (!pressed || !index || *index >= itemCount() || m_browser.model().rows()[*index] != *pressed || dragged) {
    m_lastClick.reset();
    return {};
  }
  if (m_toggleSelectionOnClick) {
    m_lastClick.reset();
    (void)m_browser.model().select(*pressed, SelectionMode::Toggle);
    return {.rebind = true, .activateIndex = std::nullopt};
  }
  if (!m_activationClick || m_modifiers() != m_pressModifiers) {
    m_lastClick.reset();
    return {};
  }
  const auto now = std::chrono::steady_clock::now();
  if (m_collapseSelectionOnClick) {
    m_browser.model().select(*pressed);
  }
  const bool twice = m_lastClick == pressed
      && m_lastClickModifiers == m_pressModifiers
      && now - m_lastClickTime < std::chrono::milliseconds(400);
  m_lastClick = twice ? std::nullopt : pressed;
  m_lastClickModifiers = m_pressModifiers;
  m_lastClickTime = now;
  const auto* entry = m_browser.model().entry(*pressed);
  const bool columnFolder =
      m_mode == ViewMode::Miller && m_pressModifiers == 0 && entry && entry->type == luciole::FileType::Directory;
  return {
      .rebind = true,
      .activateIndex =
          (twice || (!columnFolder && m_activationMode == ActivationMode::SingleClick)) ? index : std::nullopt
  };
}

void luciole::FileViewAdapter::onPointerCancel() {
  const bool dragged = std::exchange(m_dragged, false);
  m_pressed.reset();
  m_expanderPress.reset();
  m_lastClick.reset();
  m_areaSelection = false;
  m_areaRows.clear();
  m_areaBaseline.clear();
  if (dragged && m_cancelDrag) {
    m_cancelDrag();
  }
}

void luciole::FileViewAdapter::setContextCallback(std::function<void(std::optional<EntryId>, float, float)> callback) {
  m_context = std::move(callback);
}
void luciole::FileViewAdapter::setDragCallback(
    std::function<void(const std::vector<EntryId>&)> start, std::function<void()> cancel
) {
  m_drag = std::move(start);
  m_cancelDrag = std::move(cancel);
}
void luciole::FileViewAdapter::onSecondaryActivate(std::size_t index, float x, float y) {
  auto& model = m_browser.model();
  if (index >= model.rows().size()) {
    return;
  }
  const auto id = model.rows()[index];
  if (!model.selection().selected.contains(id)) {
    model.select(id);
  } else {
    model.setFocus(id);
  }
  if (m_context) {
    m_context(id, x, y);
  }
}
void luciole::FileViewAdapter::onBackgroundSecondaryActivate(float x, float y) {
  if (m_context) {
    m_context(std::nullopt, x, y);
  }
}

bool luciole::FileViewAdapter::onBackgroundPointerPress() {
  onPointerCancel();
  auto& model = m_browser.model();
  m_areaRows = model.rows();
  m_areaGeneration = model.snapshot().generation;
  const auto modifiers = m_modifiers();
  m_areaToggle = (modifiers & KeyMod::Ctrl) != 0 && (modifiers & KeyMod::Shift) == 0;
  if ((modifiers & (KeyMod::Ctrl | KeyMod::Shift)) != 0) {
    m_areaBaseline = model.selection().selected;
  } else {
    model.clearSelection();
  }
  m_areaSelection = true;
  return true;
}

bool luciole::FileViewAdapter::onAreaSelection(const std::vector<std::size_t>& indices) {
  auto& model = m_browser.model();
  if (!areaSelectionCurrent()) {
    return false;
  }
  auto selected = m_areaBaseline;
  for (const auto index : indices) {
    if (index >= m_areaRows.size()) {
      return false;
    }
    const auto id = m_areaRows[index];
    if (!m_areaToggle || !selected.erase(id)) {
      selected.insert(id);
    }
  }
  std::vector<EntryId> ids;
  ids.reserve(selected.size());
  for (const auto id : m_areaRows) {
    if (selected.contains(id)) {
      ids.push_back(id);
    }
  }
  return model.replaceSelection(ids);
}

bool luciole::FileViewAdapter::areaSelectionCurrent() const {
  const auto& model = m_browser.model();
  return m_areaSelection && model.snapshot().generation == m_areaGeneration && model.rows() == m_areaRows;
}

bool luciole::FileViewAdapter::itemHitTest(std::size_t index, float x, float y, float width, float height) const {
  const auto& model = m_browser.model();
  if (index >= model.rows().size()) {
    return true;
  }
  const auto id = model.rows()[index];
  // A selected item is draggable wherever its selection color is drawn.
  if (m_columnActive && model.selection().selected.contains(id)) {
    return true;
  }
  if (m_mode != ViewMode::Grid) {
    for (const auto& [tile, state] : m_tiles) {
      if (state.entry == id) {
        return static_cast<const FileRow*>(tile)->contentContains(x);
      }
    }
    return true;
  }
  const auto metrics = gridMetrics(m_gridIconSize, m_presentation, false);
  const float iconLeft = (width - m_gridIconSize) * 0.5F;
  const bool icon = x >= iconLeft - 4
      && x <= iconLeft + m_gridIconSize + 4
      && y >= metrics.inset - 4
      && y <= metrics.inset + m_gridIconSize + 4;
  const bool label = x >= metrics.inset && x <= width - metrics.inset && y >= metrics.nameTop && y <= height - 4;
  return icon || label;
}

void luciole::FileViewAdapter::onActivate(std::size_t index) {
  Logger("file-launch").debug("File-view activation received");
  if (index < itemCount()) {
    if (m_activate) {
      m_activate(m_browser.model().rows()[index]);
    }
  }
}
