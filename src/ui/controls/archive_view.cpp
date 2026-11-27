#include "ui/controls/archive_view.h"

#include "browser/entry_details.h"
#include "config/keybinds.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "fs/gio_ptr.h"
#include "i18n/i18n.h"
#include "ui/browser/file_error_text.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/size_format.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <utility>

namespace {
  // The browser's default details widths, so the archive list lines up with folder lists.
  constexpr float kTypeWidth = luciole::kDefaultDetailsColumnWidths[1];
  constexpr float kSizeWidth = luciole::kDefaultDetailsColumnWidths[2];
  constexpr float kModifiedWidth = luciole::kDefaultDetailsColumnWidths[3];
  constexpr float kRowPadding = 12;
  constexpr float kRowGap = 10;
  std::string sizeText(std::uint64_t bytes) { return luciole::formatSizeWithLocalizedUnits(bytes); }
  class ArchiveRow final : public Flex {
  public:
    explicit ArchiveRow(luciole::FileIconCache& icons) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(kRowGap);
      setPadding(6, kRowPadding);
      setStyleRadius(Style::radiusSm);
      auto icon = std::make_unique<luciole::FileIcon>(icons, 24);
      m_icon = icon.get();
      addChild(std::move(icon));
      addChild(ui::label({.out = &m_name, .maxLines = 1, .flexGrow = 1, .configure = [](Label& text) {
                            text.setUseMarkup(false);
                          }}));
      for (auto [out, width] :
           {std::pair{&m_type, kTypeWidth}, std::pair{&m_size, kSizeWidth}, std::pair{&m_modified, kModifiedWidth}}) {
        addChild(
            ui::label(
                {.out = out,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                 .minWidth = width,
                 .maxWidth = width,
                 .maxLines = 1,
                 .configure = [](Label& text) { text.setUseMarkup(false); }}
            )
        );
      }
      setHitTestVisible(false);
    }
    void bind(const luciole::ArchiveEntry& entry, bool selected, bool hovered) {
      m_name->setText(entry.displayName);
      m_name->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_icon->bind(entry.icon, colorSpecFromRole(ColorRole::OnSurface));
      std::string size;
      if (entry.kind == luciole::ArchiveEntryKind::File) {
        size = entry.size ? sizeText(*entry.size) : "Unknown";
      }
      m_type->setText(entry.type);
      m_size->setText(size);
      m_modified->setText(entry.modifiedSeconds ? luciole::metadataTime(entry.modifiedSeconds) : "");
      setFill(
          selected      ? colorSpecFromRole(ColorRole::PrimaryContainer)
              : hovered ? colorSpecFromRole(ColorRole::SurfaceContainerHigh)
                        : clearColorSpec()
      );
    }

  private:
    luciole::FileIcon* m_icon;
    Label* m_name = nullptr;
    Label* m_type = nullptr;
    Label* m_size = nullptr;
    Label* m_modified = nullptr;
  };
} // namespace
class ArchiveView::Adapter final : public VirtualGridAdapter {
public:
  Adapter(ArchiveView& owner, luciole::FileIconCache& icons) : m_owner(owner), m_icons(icons) {}
  std::size_t itemCount() const override { return m_owner.m_rows.size(); }
  std::unique_ptr<Node> createTile() override { return std::make_unique<ArchiveRow>(m_icons); }
  void bindTile(Node& tile, std::size_t index, bool, bool hovered) override {
    const auto& entry = m_owner.m_catalog->entries[m_owner.m_rows[index]];
    static_cast<ArchiveRow&>(tile).bind(entry, m_owner.m_selection.contains(entry.path), hovered);
  }
  bool onPointerPress(std::size_t index, float, float, float, float) override {
    m_pressed = index;
    m_plain = m_owner.m_modifiers() == 0;
    m_owner.choose(index, m_owner.m_modifiers());
    return true;
  }
  PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override {
    const auto pressed = std::exchange(m_pressed, std::nullopt);
    if (!m_plain || !pressed || index != pressed) {
      m_last.reset();
      return {};
    }
    const auto now = std::chrono::steady_clock::now();
    const bool twice = m_last == index && now - m_time < std::chrono::milliseconds(400);
    m_last = twice ? std::nullopt : index;
    m_time = now;
    return {.rebind = true, .activateIndex = twice ? index : std::nullopt};
  }
  bool onBackgroundPointerPress() override {
    m_owner.m_selection.clear();
    m_owner.m_focus.reset();
    m_owner.m_anchor.reset();
    m_owner.m_grid->setSelectedIndex(std::nullopt, false);
    m_owner.selectionChanged();
    return true;
  }
  void onPointerCancel() override {
    m_pressed.reset();
    m_last.reset();
  }
  void onActivate(std::size_t index) override { m_owner.activate(index); }

private:
  ArchiveView& m_owner;
  luciole::FileIconCache& m_icons;
  std::optional<std::size_t> m_pressed, m_last;
  std::chrono::steady_clock::time_point m_time;
  bool m_plain = false;
};
ArchiveView::ArchiveView(
    luciole::FileIconCache& icons, std::function<std::uint32_t()> modifiers, std::function<void()> close,
    std::function<void(std::vector<std::string>)> extract, std::function<void(std::vector<std::string>)> extractHere,
    std::function<void(const std::string&)> open, std::function<void()> reload, std::function<void()> cancel
)
    : m_adapter(std::make_unique<Adapter>(*this, icons)), m_modifiers(std::move(modifiers)),
      m_extract(std::move(extract)), m_extractHere(std::move(extractHere)), m_open(std::move(open)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFill(colorSpecFromRole(ColorRole::Surface));
  auto shield = std::make_unique<InputArea>();
  shield->setParticipatesInLayout(false);
  shield->setZIndex(-1);
  m_shield = shield.get();
  addChild(std::move(shield));
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .padding = Style::spaceMd},
          ui::column(
              {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .flexGrow = 1},
              ui::label(
                  {.out = &m_title,
                   .text = "Archive",
                   .fontSize = Style::fontSizeHeader,
                   .fontWeight = FontWeight::SemiBold,
                   .maxLines = 1,
                   .configure = [](Label& text) { text.setUseMarkup(false); }}
              ),
              ui::label(
                  {.out = &m_info,
                   .fontSize = Style::fontSizeCaption,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                   .maxLines = 1,
                   .configure = [](Label& text) { text.setUseMarkup(false); }}
              )
          ),
          ui::button(
              {.out = &m_reload,
               .glyph = "refresh",
               .glyphSize = Style::baseGlyphSize,
               .controlHeight = 24.0F,
               .variant = ButtonVariant::Ghost,
               .tooltip = "Reload archive",
               .padding = Style::spaceXs,
               .width = 24.0F,
               .onClick = std::move(reload)}
          ),
          ui::button(
              {.glyph = "x",
               .glyphSize = Style::baseGlyphSize,
               .controlHeight = 24.0F,
               .variant = ButtonVariant::Ghost,
               .tooltip = "Close archive",
               .followsCloseButtons = true,
               .padding = Style::spaceXs,
               .width = 24.0F,
               .onClick = std::move(close)}
          )
      )
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = Style::spaceSm, .paddingH = Style::spaceMd},
          ui::button(
              {.out = &m_up,
               .glyph = "arrow-up",
               .variant = ButtonVariant::Ghost,
               .tooltip = "Parent archive folder",
               .onClick = [this] { up(); }}
          ),
          ui::row(
              {.align = FlexAlign::Center,
               .paddingH = Style::spaceXs,
               .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
               .styleRadius = Style::radiusMd,
               .minWidth = 80.0F,
               .clipChildren = true,
               .height = Style::controlHeight,
               .flexGrow = 1.0F},
              ui::row({.out = &m_crumbs, .align = FlexAlign::Center})
          ),
          ui::button(
              {.out = &m_extractTo,
               .text = "Extract…",
               .glyph = "file-export",
               .variant = ButtonVariant::Primary,
               .tooltip = "Choose where to extract",
               .onClick = [this] { m_extract(selected()); }}
          ),
          ui::button(
              {.out = &m_extractHereButton,
               .text = "Extract Here",
               .tooltip = "Extract into a folder beside the archive",
               .onClick = [this] { m_extractHere(selected()); }}
          ),
          ui::button({.out = &m_openButton, .text = "Open", .onClick = [this] {
                        if (m_selection.size() == 1 && m_catalog) {
                          const auto found = std::ranges::find_if(m_rows, [this](auto index) {
                            return m_catalog->entries[index].path == *m_selection.begin();
                          });
                          if (found != m_rows.end()) {
                            activate(static_cast<std::size_t>(found - m_rows.begin()));
                          }
                        }
                      }})
      )
  );
  auto header = ui::row({.align = FlexAlign::Center, .gap = kRowGap, .paddingH = kRowPadding});
  constexpr std::array<std::pair<const char*, float>, 4> columns{
      {{"Name", 0.0F}, {"Type", kTypeWidth}, {"Size", kSizeWidth}, {"Modified", kModifiedWidth}}
  };
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const auto [text, width] = columns[i];
    const auto column = static_cast<SortColumn>(i);
    const auto fixed = width > 0 ? std::optional(width) : std::nullopt;
    header->addChild(
        ui::button(
            {.out = &m_headers[i],
             .text = text,
             .glyph = "arrow-up",
             .controlHeight = Style::controlHeightSm,
             .contentAlign = ButtonContentAlign::Start,
             .variant = ButtonVariant::Ghost,
             .tooltip = std::format("Sort by {}; click again to reverse", text),
             .minWidth = fixed,
             .maxWidth = fixed,
             .paddingH = Style::spaceSm,
             .flexGrow = fixed ? 0.0F : 1.0F,
             .onClick = [this, column] { sortBy(column, m_sort == column && !m_descending); }}
        )
    );
  }
  // Inset like the toolbar above so the list does not run into the window edges.
  addChild(
      ui::column(
          {.out = &m_headerRow, .align = FlexAlign::Stretch, .paddingH = Style::spaceMd, .flexGrow = 1},
          std::move(header), ui::separator({}),
          ui::virtualGridView(
              {.out = &m_grid,
               .columns = 1,
               .cellHeight = 40,
               .squareCells = false,
               .rowGap = 2,
               .overscanRows = 2,
               .adapter = m_adapter.get(),
               .flexGrow = 1}
          )
      )
  );
  auto cardIcon = std::make_unique<luciole::FileIcon>(icons, 64);
  m_cardIcon = cardIcon.get();
  addChild(
      ui::column(
          {.out = &m_card,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .gap = Style::spaceSm,
           .padding = Style::spaceLg,
           .flexGrow = 1,
           .visible = false,
           .participatesInLayout = false},
          std::move(cardIcon),
          ui::label(
              {.out = &m_cardName,
               .fontWeight = FontWeight::SemiBold,
               .maxLines = 1,
               .configure = [](Label& text) { text.setUseMarkup(false); }}
          ),
          ui::label(
              {.text = "Decompresses to this single file. Its size is known only after extraction.",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 2,
               .textAlign = TextAlign::Center}
          )
      )
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .padding = Style::spaceMd},
          ui::label(
              {.out = &m_status,
               .text = "Reading archive…",
               .maxLines = 2,
               .flexGrow = 1,
               .configure = [](Label& text) { text.setUseMarkup(false); }}
          ),
          ui::button({.out = &m_cancel, .text = "Cancel", .onClick = std::move(cancel)})
      )
  );
  m_grid->focusArea()->setRetainsFocusOnPointerRelease(true);
  sortBy(SortColumn::Name, false);
}
ArchiveView::~ArchiveView() { m_grid->setAdapter(nullptr); }
void ArchiveView::sync(
    std::shared_ptr<const luciole::ArchiveCatalog> catalog, luciole::ArchivePhase phase, std::size_t items,
    std::uint64_t bytes, const std::optional<luciole::FileError>& error
) {
  m_busy = phase != luciole::ArchivePhase::Idle;
  // A reload drops the catalog until it is read again; keep showing the old one meanwhile.
  if (catalog != m_catalog && (catalog || !m_busy)) {
    const bool same = catalog && m_catalog && catalog->source == m_catalog->source;
    m_catalog = std::move(catalog);
    const auto exists = [this](const std::string& path) {
      return std::ranges::contains(m_catalog->entries, path, &luciole::ArchiveEntry::path);
    };
    if (!same || (!m_parent.empty() && !exists(m_parent))) {
      m_parent.clear();
    }
    if (same) {
      std::erase_if(m_selection, [&](const std::string& path) { return !exists(path); });
    } else {
      m_selection.clear();
    }
    m_focus.reset();
    m_anchor.reset();
    rebuild();
    syncInfo();
  }
  const std::string size = luciole::formatSizeWithLocalizedUnits(bytes);
  m_status->setText(
      error        ? luciole::fileErrorText(*error)
          : m_busy ? std::format("Processing {} entries · {}", items, size)
                   : statusText()
  );
  m_status->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  // Keeps its space while hidden so a quick reload does not resize the page.
  m_cancel->setVisible(m_busy);
  m_reload->setEnabled(!m_busy);
  selectionChanged();
}
void ArchiveView::rebuild() {
  m_grid->cancelPointerGesture();
  m_adapter->onPointerCancel();
  m_rows.clear();
  if (m_catalog) {
    for (std::size_t i = 0; i < m_catalog->entries.size(); ++i) {
      if (m_catalog->entries[i].parent == m_parent) {
        m_rows.push_back(i);
      }
    }
  }
  if (m_catalog) {
    const auto& entries = m_catalog->entries;
    std::ranges::sort(m_rows, [this, &entries](auto left, auto right) {
      const auto& a = entries[left];
      const auto& b = entries[right];
      if ((a.kind == luciole::ArchiveEntryKind::Directory) != (b.kind == luciole::ArchiveEntryKind::Directory)) {
        return a.kind == luciole::ArchiveEntryKind::Directory;
      }
      if (m_sort == SortColumn::Type && a.type != b.type) {
        const int typeOrder = g_utf8_collate(a.type.c_str(), b.type.c_str());
        if (typeOrder != 0) {
          return m_descending ? typeOrder > 0 : typeOrder < 0;
        }
      }
      const auto key = [this](const luciole::ArchiveEntry& entry) {
        return m_sort == SortColumn::Size ? entry.size.value_or(0) : entry.modifiedSeconds.value_or(0);
      };
      if ((m_sort == SortColumn::Size || m_sort == SortColumn::Modified) && key(a) != key(b)) {
        return m_descending ? key(a) > key(b) : key(a) < key(b);
      }
      const int order = g_utf8_collate(a.displayName.c_str(), b.displayName.c_str());
      if (order != 0) {
        return m_descending && m_sort == SortColumn::Name ? order > 0 : order < 0;
      }
      return a.path < b.path;
    });
  }
  rebuildCrumbs();
  if (!m_busy) {
    m_status->setText(statusText());
  }
  m_up->setEnabled(!m_parent.empty());
  m_grid->setSelectedIndex(std::nullopt);
  m_grid->notifyDataChanged();
  m_grid->scrollView().requestScrollToOffset(0);
  selectionChanged();
}
void ArchiveView::sortBy(SortColumn column, bool descending) {
  m_sort = column;
  m_descending = descending;
  for (std::size_t i = 0; i < m_headers.size(); ++i) {
    const bool active = static_cast<SortColumn>(i) == m_sort;
    m_headers[i]->setGlyph(m_descending ? "arrow-down" : "arrow-up");
    m_headers[i]->glyph()->setVisible(active);
    m_headers[i]->glyph()->setParticipatesInLayout(active);
  }
  rebuild();
}
std::string ArchiveView::statusText() const {
  return std::format("{} {}", m_rows.size(), i18n::trp("item", m_rows.size()));
}
void ArchiveView::openFolder(const std::string& path) {
  m_parent = path;
  m_selection.clear();
  m_focus.reset();
  m_anchor.reset();
  rebuild();
}
void ArchiveView::rebuildCrumbs() {
  while (!m_crumbs->children().empty()) {
    m_crumbs->removeChild(m_crumbs->children().back().get());
  }
  if (!m_catalog) {
    return;
  }
  std::vector<std::pair<std::string, std::string>> crumbs{
      {m_catalog->source.displayName().value_or("Archive"), std::string()}
  };
  for (std::size_t start = 0; start < m_parent.size();) {
    const auto slash = std::min(m_parent.find('/', start), m_parent.size());
    crumbs.emplace_back(m_parent.substr(start, slash - start), m_parent.substr(0, slash));
    start = slash + 1;
  }
  const std::weak_ptr<int> lifetime = m_lifetime;
  for (std::size_t i = 0; i < crumbs.size(); ++i) {
    if (i != 0) {
      m_crumbs->addChild(ui::label({.text = "/", .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
    }
    m_crumbs->addChild(
        ui::button(
            {.text = crumbs[i].first,
             .controlHeight = Style::controlHeightSm,
             .contentAlign = ButtonContentAlign::Start,
             .variant = ButtonVariant::Ghost,
             .tooltip = crumbs[i].first,
             .maxWidth = 160.0F,
             .paddingH = Style::spaceSm,
             // Deferred: rebuilding the breadcrumbs destroys the clicked button.
             .onClick =
                 [this, lifetime, target = crumbs[i].second] {
                   DeferredCall::callLater([this, lifetime, target] {
                     if (!lifetime.expired()) {
                       openFolder(target);
                     }
                   });
                 },
             .configure = [](Button& button) { button.label()->setUseMarkup(false); }}
        )
    );
  }
}
void ArchiveView::syncInfo() {
  const bool single = m_catalog && m_catalog->single && m_catalog->entries.size() == 1;
  m_headerRow->setVisible(!single);
  m_headerRow->setParticipatesInLayout(!single);
  m_card->setVisible(single);
  m_card->setParticipatesInLayout(single);
  if (!m_catalog) {
    m_info->setText("");
    return;
  }
  m_title->setText(m_catalog->source.displayName().value_or(i18n::text("Archive")));
  std::vector<std::string> parts;
  if (single) {
    const auto& entry = m_catalog->entries.front();
    m_cardIcon->bind(entry.icon, colorSpecFromRole(ColorRole::OnSurface));
    m_cardName->setText(entry.displayName);
    m_selection = {entry.path};
    m_focus = 0;
    parts.push_back(std::format("{} compressed file", m_catalog->compression));
  } else {
    parts.push_back(
        m_catalog->compression.empty() ? m_catalog->format
                                       : std::format("{} · {}", m_catalog->format, m_catalog->compression)
    );
  }
  if (m_catalog->archiveBytes > 0) {
    parts.push_back(sizeText(m_catalog->archiveBytes));
  }
  if (!single) {
    const auto folders =
        std::ranges::count(m_catalog->entries, luciole::ArchiveEntryKind::Directory, &luciole::ArchiveEntry::kind);
    const auto files = static_cast<std::ptrdiff_t>(m_catalog->entries.size()) - folders;
    parts.push_back(std::format("{} {}", files, i18n::trp("file", files)));
    if (folders > 0) {
      parts.push_back(std::format("{} {}", folders, i18n::trp("folder", folders)));
    }
    parts.push_back(std::format("{} unpacked", sizeText(m_catalog->expandedBytes)));
  }
  if (m_catalog->encrypted) {
    parts.emplace_back("Encrypted");
  }
  parts.emplace_back("Read only");
  std::string info;
  for (const auto& part : parts) {
    info += info.empty() ? part : " · " + part;
  }
  m_info->setText(info);
  rebuildCrumbs();
  selectionChanged();
}
std::vector<std::string> ArchiveView::selected() const {
  if (m_catalog && m_catalog->single) {
    return {};
  }
  return {m_selection.begin(), m_selection.end()};
}
void ArchiveView::selectionChanged() {
  const bool whole = m_selection.empty() || (m_catalog && m_catalog->single);
  m_extractTo->setText(whole ? i18n::text("Extract…") : i18n::text("Extract Selected…"));
  m_extractHereButton->setText(whole ? i18n::text("Extract Here") : i18n::text("Extract Selected Here"));
  m_extractTo->setEnabled(m_catalog && !m_busy);
  m_extractHereButton->setEnabled(m_catalog && !m_busy);
  bool canOpen = false;
  if (m_catalog && m_selection.size() == 1) {
    const auto found = std::ranges::find(m_catalog->entries, *m_selection.begin(), &luciole::ArchiveEntry::path);
    canOpen = found != m_catalog->entries.end() && found->kind != luciole::ArchiveEntryKind::Symlink;
  }
  m_openButton->setEnabled(canOpen && !m_busy);
  m_grid->notifyDataChanged();
  markLayoutDirty();
}
void ArchiveView::choose(std::size_t index, std::uint32_t modifiers) {
  if (index >= m_rows.size() || !m_catalog) {
    return;
  }
  const auto& path = m_catalog->entries[m_rows[index]].path;
  if ((modifiers & KeyMod::Shift) != 0 && m_anchor) {
    if ((modifiers & KeyMod::Ctrl) == 0) {
      m_selection.clear();
    }
    for (auto i = std::min(index, *m_anchor); i <= std::max(index, *m_anchor) && i < m_rows.size(); ++i) {
      m_selection.insert(m_catalog->entries[m_rows[i]].path);
    }
  } else if ((modifiers & KeyMod::Ctrl) != 0) {
    if (!m_selection.erase(path)) {
      m_selection.insert(path);
    }
    m_anchor = index;
  } else {
    m_selection = {path};
    m_anchor = index;
  }
  m_focus = index;
  m_grid->setSelectedIndex(index);
  selectionChanged();
}
void ArchiveView::activate(std::size_t index) {
  if (m_busy || !m_catalog || index >= m_rows.size()) {
    return;
  }
  const auto& entry = m_catalog->entries[m_rows[index]];
  if (entry.kind == luciole::ArchiveEntryKind::Directory) {
    openFolder(entry.path);
  } else if (entry.kind != luciole::ArchiveEntryKind::Symlink) {
    m_open(entry.path);
  }
}
void ArchiveView::up() {
  if (m_parent.empty()) {
    return;
  }
  const auto slash = m_parent.rfind('/');
  openFolder(slash == std::string::npos ? "" : m_parent.substr(0, slash));
}
bool ArchiveView::key(std::uint32_t symbol, std::uint32_t modifiers) {
  if (luciole::keybindMatches(luciole::KeybindCommand::SelectAll, symbol, modifiers)) {
    m_selection.clear();
    if (m_catalog) {
      for (auto index : m_rows) {
        m_selection.insert(m_catalog->entries[index].path);
      }
    }
    selectionChanged();
    return true;
  }
  if (luciole::keybindMatches(luciole::KeybindCommand::ParentFolder, symbol, modifiers)
      || luciole::keybindMatches(luciole::KeybindCommand::Up, symbol, modifiers)) {
    up();
    return true;
  }
  if (luciole::keybindMatches(luciole::KeybindCommand::Open, symbol, modifiers)) {
    if (m_focus) {
      activate(*m_focus);
    }
    return true;
  }
  using K = luciole::KeybindCommand;
  constexpr std::array navigationCommands{K::MoveUp,   K::MoveDown,   K::MoveFirst,
                                          K::MoveLast, K::MovePageUp, K::MovePageDown};
  const auto match = luciole::keybindBestMatch(navigationCommands, symbol, modifiers);
  if (!m_rows.empty() && match) {
    std::size_t index = m_focus.value_or(0);
    const std::size_t stride = m_grid ? m_grid->pageItemStride() : 1;
    switch (match->command) {
    case K::MoveFirst:
      index = 0;
      break;
    case K::MoveLast:
      index = m_rows.size() - 1;
      break;
    case K::MovePageDown:
      index = std::min(index + stride, m_rows.size() - 1);
      break;
    case K::MovePageUp:
      index = index > stride ? index - stride : 0;
      break;
    case K::MoveUp:
      if (index) {
        --index;
      }
      break;
    default:
      index = std::min(index + 1, m_rows.size() - 1);
      break;
    }
    choose(index, match->extraModifiers);
    return true;
  }
  return false;
}
InputArea* ArchiveView::focusArea() const { return m_grid->focusArea(); }
void ArchiveView::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  m_shield->arrange(renderer, {0, 0, width(), height()});
}
