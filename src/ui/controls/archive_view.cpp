#include "ui/controls/archive_view.h"

#include "core/input/key_symbols.h"
#include "fs/gio_ptr.h"
#include "ui/browser/file_error_text.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace {
  class ArchiveRow final : public Flex {
  public:
    explicit ArchiveRow(luciole::FileIconCache& icons) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(10);
      setPadding(6, 12);
      setStyleRadius(Style::radiusSm);
      auto icon = std::make_unique<luciole::FileIcon>(icons, 24);
      m_icon = icon.get();
      addChild(std::move(icon));
      addChild(ui::label({.out = &m_name, .maxLines = 1, .flexGrow = 1, .configure = [](Label& text) {
                            text.setUseMarkup(false);
                          }}));
      addChild(
          ui::label(
              {.out = &m_size,
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .minWidth = 90,
               .maxWidth = 90,
               .maxLines = 1,
               .textAlign = TextAlign::End}
          )
      );
      setHitTestVisible(false);
    }
    void bind(const luciole::ArchiveEntry& entry, bool selected, bool hovered) {
      m_name->setText(entry.displayName);
      m_name->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_icon->bind(entry.icon, colorSpecFromRole(ColorRole::OnSurface));
      std::string size;
      if (entry.kind == luciole::ArchiveEntryKind::Directory) {
        size = "Folder";
      } else if (
          entry.kind == luciole::ArchiveEntryKind::Symlink || entry.kind == luciole::ArchiveEntryKind::Hardlink
      ) {
        size = "Link";
      } else if (entry.size) {
        const luciole::GString value(g_format_size_full(*entry.size, G_FORMAT_SIZE_IEC_UNITS));
        size = value.get();
      }
      m_size->setText(size);
      setFill(
          selected      ? colorSpecFromRole(ColorRole::PrimaryContainer)
              : hovered ? colorSpecFromRole(ColorRole::SurfaceContainerHigh)
                        : clearColorSpec()
      );
    }

  private:
    luciole::FileIcon* m_icon;
    Label* m_name = nullptr;
    Label* m_size = nullptr;
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
    std::function<void(std::vector<std::string>)> extract, std::function<void(const std::string&)> open,
    std::function<void()> reload, std::function<void()> cancel
)
    : m_adapter(std::make_unique<Adapter>(*this, icons)), m_modifiers(std::move(modifiers)),
      m_extract(std::move(extract)), m_open(std::move(open)) {
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
          {.align = FlexAlign::Center, .gap = Style::spaceMd, .padding = Style::spaceMd},
          ui::label(
              {.out = &m_title,
               .text = "Archive",
               .fontSize = Style::fontSizeHeader,
               .fontWeight = FontWeight::SemiBold,
               .maxLines = 1,
               .flexGrow = 1,
               .configure = [](Label& text) { text.setUseMarkup(false); }}
          ),
          ui::label(
              {.text = "Read only",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          ),
          ui::button(
              {.glyph = "x",
               .variant = ButtonVariant::Default,
               .tooltip = "Close archive",
               .minWidth = Style::controlHeight,
               .maxWidth = Style::controlHeight,
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
               .variant = ButtonVariant::Default,
               .tooltip = "Parent archive folder",
               .onClick = [this] { up(); }}
          ),
          ui::label(
              {.out = &m_path,
               .text = "/",
               .maxLines = 1,
               .flexGrow = 1,
               .configure = [](Label& text) { text.setUseMarkup(false); }}
          ),
          ui::button({.out = &m_reload, .glyph = "refresh", .tooltip = "Reload archive", .onClick = std::move(reload)})
      )
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingV = Style::spaceSm, .paddingH = Style::spaceMd},
          ui::button({.out = &m_extractAll, .text = "Extract All…", .onClick = [this] { m_extract({}); }}),
          ui::button(
              {.out = &m_extractSelected,
               .text = "Extract Selected…",
               .onClick = [this] { m_extract(std::vector<std::string>(m_selection.begin(), m_selection.end())); }}
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
  addChild(
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
  selectionChanged();
}
ArchiveView::~ArchiveView() { m_grid->setAdapter(nullptr); }
void ArchiveView::sync(
    std::shared_ptr<const luciole::ArchiveCatalog> catalog, luciole::ArchivePhase phase, std::size_t items,
    std::uint64_t bytes, const std::optional<luciole::FileError>& error
) {
  m_busy = phase != luciole::ArchivePhase::Idle;
  if (catalog != m_catalog) {
    m_catalog = std::move(catalog);
    m_parent.clear();
    m_selection.clear();
    m_focus.reset();
    m_anchor.reset();
    rebuild();
  }
  if (m_catalog) {
    m_title->setText(m_catalog->source.displayName().value_or("Archive"));
  }
  const luciole::GString size(g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS));
  m_status->setText(
      error        ? luciole::fileErrorText(*error)
          : m_busy ? std::format("Processing {} entries · {}", items, size.get())
                   : std::format("{} item{}", m_rows.size(), m_rows.size() == 1 ? "" : "s")
  );
  m_status->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  m_cancel->setVisible(m_busy);
  m_cancel->setParticipatesInLayout(m_busy);
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
    std::ranges::sort(m_rows, [this](auto left, auto right) {
      const auto& a = m_catalog->entries[left];
      const auto& b = m_catalog->entries[right];
      if ((a.kind == luciole::ArchiveEntryKind::Directory) != (b.kind == luciole::ArchiveEntryKind::Directory)) {
        return a.kind == luciole::ArchiveEntryKind::Directory;
      }
      const int order = g_utf8_collate(a.displayName.c_str(), b.displayName.c_str());
      return order == 0 ? a.path < b.path : order < 0;
    });
  }
  m_path->setText(m_parent.empty() ? "/" : m_parent);
  m_up->setEnabled(!m_parent.empty());
  m_grid->setSelectedIndex(std::nullopt);
  m_grid->notifyDataChanged();
  m_grid->scrollView().requestScrollToOffset(0);
  selectionChanged();
}
void ArchiveView::selectionChanged() {
  m_extractAll->setEnabled(m_catalog && !m_busy);
  m_extractSelected->setEnabled(m_catalog && !m_busy && !m_selection.empty());
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
    m_parent = entry.path;
    m_selection.clear();
    m_focus.reset();
    m_anchor.reset();
    rebuild();
  } else if (entry.kind != luciole::ArchiveEntryKind::Symlink) {
    m_open(entry.path);
  }
}
void ArchiveView::up() {
  if (m_parent.empty()) {
    return;
  }
  const auto slash = m_parent.rfind('/');
  m_parent = slash == std::string::npos ? "" : m_parent.substr(0, slash);
  m_selection.clear();
  m_focus.reset();
  m_anchor.reset();
  rebuild();
}
bool ArchiveView::key(std::uint32_t symbol, std::uint32_t modifiers) {
  if (symbol == XKB_KEY_a && modifiers == KeyMod::Ctrl) {
    m_selection.clear();
    if (m_catalog) {
      for (auto index : m_rows) {
        m_selection.insert(m_catalog->entries[index].path);
      }
    }
    selectionChanged();
    return true;
  }
  if (KeySymbol::isBackspace(symbol) || (symbol == XKB_KEY_Up && modifiers == KeyMod::Alt)) {
    up();
    return true;
  }
  if (KeySymbol::isEnter(symbol)) {
    if (m_focus) {
      activate(*m_focus);
    }
    return true;
  }
  if (!m_rows.empty()
      && (KeySymbol::isUp(symbol) || KeySymbol::isDown(symbol) || symbol == XKB_KEY_Home || symbol == XKB_KEY_End)) {
    std::size_t index = m_focus.value_or(0);
    if (symbol == XKB_KEY_Home) {
      index = 0;
    } else if (symbol == XKB_KEY_End) {
      index = m_rows.size() - 1;
    } else if (KeySymbol::isUp(symbol)) {
      if (index) {
        --index;
      }
    } else {
      index = std::min(index + 1, m_rows.size() - 1);
    }
    choose(index, modifiers);
    return true;
  }
  return false;
}
InputArea* ArchiveView::focusArea() const { return m_grid->focusArea(); }
void ArchiveView::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  m_shield->arrange(renderer, {0, 0, width(), height()});
}
