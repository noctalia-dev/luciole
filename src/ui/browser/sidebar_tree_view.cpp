#include "ui/browser/sidebar_tree_view.h"

#include "browser/location_presentation.h"
#include "config/keybinds.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/side_list_metrics.h"
#include "ui/controls/virtual_grid_view.h"

#include <algorithm>

namespace {
  class TreeButton final : public Button {
  public:
    TreeButton() {
      setVariant(ButtonVariant::Ghost);
      setContentAlign(ButtonContentAlign::Start);
      setControlHeight(luciole::kSideListRowHeight);
      setGlyph("folder");
      setText(" ");
      label()->setUseMarkup(false);
      inputArea()->setHitTestVisible(false);
      auto prefix = ui::row(
          {.out = &m_prefix, .justify = FlexJustify::End},
          ui::glyph({.out = &m_arrow, .glyph = "chevron-right", .glyphSize = 14})
      );
      insertChildAt(0, std::move(prefix));
    }
    void bind(
        const luciole::TreeRowInfo& info, const std::string& text, const std::string& glyph, bool selected,
        bool hovered, float width
    ) {
      const float prefix = std::min(static_cast<float>(info.depth) * 14.0F, std::max(0.0F, width - 110.0F)) + 18.0F;
      m_prefix->setMinWidth(prefix);
      m_prefix->setMaxWidth(prefix);
      m_arrow->setVisible(info.expandable);
      m_arrow->setGlyph(info.expanded ? "chevron-down" : Style::rtl() ? "chevron-left" : "chevron-right");
      m_arrow->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimary : ColorRole::OnSurfaceVariant));
      setGlyph(glyph);
      setText(info.status.empty() ? text : text + " · " + info.status);
      setSelected(selected);
      setHoveredVisual(hovered);
    }

  private:
    Flex* m_prefix = nullptr;
    Glyph* m_arrow = nullptr;
  };
} // namespace

class luciole::SidebarTreeView::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(SidebarTreeView& owner) : m_owner(owner) {}
  std::size_t itemCount() const override { return m_owner.m_model.rows().size(); }
  std::unique_ptr<Node> createTile() override { return std::make_unique<TreeButton>(); }
  void bindTile(Node& tile, std::size_t index, bool, bool hovered) override {
    const auto id = m_owner.m_model.rows()[index];
    const auto* entry = m_owner.m_model.entry(id);
    const auto glyph = m_owner.m_glyphs.find(id);
    auto& button = static_cast<TreeButton&>(tile);
    button.bind(
        m_owner.m_tree.row(id), entry->displayName(), glyph == m_owner.m_glyphs.end() ? "folder" : glyph->second,
        m_owner.m_current == entry->navigationLocation(), hovered, m_owner.width()
    );
    button.setKeyboardFocusHint(m_owner.m_grid->focusArea()->focused() && m_owner.m_model.selection().focused == id);
  }
  std::string itemTooltip(std::size_t index) const override {
    const auto id = m_owner.m_model.rows()[index];
    const auto info = m_owner.m_tree.row(id);
    return locationEditText(m_owner.m_model.entry(id)->location()) + (info.status.empty() ? "" : "\n" + info.status);
  }
  bool onPointerPress(std::size_t index, float x, float, float width, float) override {
    m_pressed = m_owner.m_model.rows()[index];
    const auto info = m_owner.m_tree.row(*m_pressed);
    const float prefix = std::min(static_cast<float>(info.depth) * 14.0F, std::max(0.0F, width - 110.0F)) + 18.0F;
    const float leading = Style::rtl() ? width - x : x;
    m_expand = info.expandable && leading < prefix + Style::spaceMd;
    m_dragged = false;
    m_owner.m_model.select(*m_pressed);
    return true;
  }
  bool onPointerDrag(std::optional<std::size_t>, float, float, float, float) override {
    if (!m_dragged && m_pressed && !m_expand && m_owner.m_pinIndices.contains(*m_pressed) && m_owner.drag) {
      m_dragged = true;
      if (const auto bookmark = m_owner.bookmark(*m_pressed)) {
        m_owner.drag(*bookmark);
      }
    }
    return false;
  }
  PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override {
    const auto pressed = std::exchange(m_pressed, std::nullopt);
    if (!pressed || m_dragged || !index || *index >= itemCount() || m_owner.m_model.rows()[*index] != *pressed) {
      return {};
    }
    return {.rebind = true, .activateIndex = index};
  }
  void onActivate(std::size_t index) override {
    const auto id = m_owner.m_model.rows()[index];
    if (m_expand) {
      (void)m_owner.m_tree.toggle(id);
    } else if (m_owner.open) {
      const auto target = m_owner.m_model.entry(id)->navigationLocation();
      m_owner.open(target);
    }
  }
  void onPointerCancel() override {
    m_pressed.reset();
    m_dragged = false;
  }
  void onSecondaryActivate(std::size_t index, float x, float y) override {
    const auto bookmark = m_owner.bookmark(m_owner.m_model.rows()[index]);
    if (bookmark && m_owner.context) {
      m_owner.context(*bookmark, x, y);
    }
  }

private:
  SidebarTreeView& m_owner;
  std::optional<EntryId> m_pressed;
  bool m_expand = false, m_dragged = false;
};

luciole::SidebarTreeView::SidebarTreeView() : m_adapter(std::make_unique<Adapter>(*this)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFlexGrow(1);
  setPadding(8);
  addChild(
      ui::label(
          {.text = "Folders",
           .fontSize = Style::fontSizeCaption,
           .fontWeight = FontWeight::SemiBold,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .height = 24}
      )
  );
  auto grid = std::make_unique<VirtualGridView>();
  m_grid = grid.get();
  grid->setFlexGrow(1);
  grid->setColumns(1);
  grid->setSquareCells(false);
  grid->setCellHeight(luciole::kSideListRowHeight);
  grid->setRowGap(4);
  grid->setAdapter(m_adapter.get());
  grid->focusArea()->setTabFocusKey("sidebar-tree");
  grid->focusArea()->setRetainsFocusOnPointerRelease(true);
  grid->scrollView().setReserveScrollbarGutter(true);
  grid->focusArea()->setOnKeyDown([this](const InputArea::KeyData& event) {
    if (event.pressed && !event.preedit && key(event.sym, event.modifiers)) {
      markLayoutDirty();
      if (changed) {
        changed();
      }
    }
  });
  grid->focusArea()->setOnFocusGain([this] {
    m_dirty = true;
    markLayoutDirty();
    if (changed) {
      changed();
    }
  });
  grid->focusArea()->setOnFocusLoss([this] {
    m_dirty = true;
    markLayoutDirty();
    if (changed) {
      changed();
    }
  });
  addChild(std::move(grid));
  m_model.setChangedCallback([this] {
    m_dirty = true;
    markLayoutDirty();
    if (changed) {
      changed();
    }
  });
  m_tree.setChangedCallback([this] {
    m_dirty = true;
    markLayoutDirty();
    if (changed) {
      changed();
    }
  });
}
luciole::SidebarTreeView::~SidebarTreeView() {
  m_grid->focusArea()->setOnFocusGain({});
  m_grid->focusArea()->setOnFocusLoss({});
  m_grid->focusArea()->setOnKeyDown({});
  m_grid->setAdapter(nullptr);
  m_model.setChangedCallback({});
  m_tree.setChangedCallback({});
}
void luciole::SidebarTreeView::setRoots(
    const PinService& pins, const std::vector<Drive>& drives, bool showNetwork, bool trashFull
) {
  DirectorySnapshot snapshot;
  snapshot.status = DirectoryStatus::Ready;
  std::vector<DirectoryEntry> entries;
  std::map<std::string, EntryId> roots;
  m_pinIndices.clear();
  m_glyphs.clear();
  m_pinCount = pins.bookmarks().size();
  for (std::size_t i = 0; i < pins.bookmarks().size(); ++i) {
    const auto& pin = pins.bookmarks()[i];
    if (!placeShown(pin, showNetwork)) {
      continue;
    }
    const auto prior = m_rootIds.find(pin.location.uri());
    const auto id = prior == m_rootIds.end() ? m_nextRoot++ : prior->second;
    roots.emplace(pin.location.uri(), id);
    m_pinIndices.emplace(id, i);
    const auto known = std::ranges::find_if(pins.userPlaces(), [&](const auto& place) {
      return place.bookmark.location == pin.location;
    });
    m_glyphs.emplace(
        id,
        !pin.iconName.empty()                  ? pin.iconName
            : pin.location.isTrash()           ? (trashFull ? "trash-filled" : "trash")
            : known == pins.userPlaces().end() ? "folder"
                                               : known->glyphName
    );
    DirectoryEntry entry{
        .id = id,
        .place = pin.location,
        .name = {},
        .type = FileType::Directory,
        .size = {},
        .modifiedSeconds = {},
    };
    entry.setDisplayName(pin.label);
    entries.push_back(std::move(entry));
  }
  for (const auto& drive : drives) {
    const auto& target = drive.activation ? *drive.activation : *drive.root;
    const auto& uri = target.uri();
    if (roots.contains(uri)) {
      continue;
    }
    const auto prior = m_rootIds.find(uri);
    const auto id = prior == m_rootIds.end() ? m_nextRoot++ : prior->second;
    roots.emplace(uri, id);
    m_glyphs.emplace(id, drive.glyph);
    DirectoryEntry entry{
        .id = id,
        .place = target,
        .name = {},
        .type = FileType::Directory,
        .size = {},
        .modifiedSeconds = {},
    };
    entry.setDisplayName(drive.label);
    entries.push_back(std::move(entry));
  }
  snapshot.entries = std::move(entries);
  m_rootIds = std::move(roots);
  m_tree.setRoot(std::move(snapshot));
}
void luciole::SidebarTreeView::sync(std::optional<Location> current, bool showHidden) {
  if (m_current == current && m_model.filter().showHidden == showHidden) {
    return;
  }
  m_current = std::move(current);
  m_model.setFilter({.showHidden = showHidden, .text = {}});
  m_dirty = true;
  markLayoutDirty();
}
void luciole::SidebarTreeView::prepare() {
  if (!std::exchange(m_dirty, false)) {
    return;
  }
  m_grid->notifyDataChanged();
  const auto focused = m_model.selection().focused;
  const auto found = focused ? std::ranges::find(m_model.rows(), *focused) : m_model.rows().end();
  m_grid->setSelectedIndex(
      found == m_model.rows().end() ? std::nullopt : std::optional<std::size_t>(found - m_model.rows().begin()),
      std::exchange(m_reveal, false)
  );
}
std::optional<luciole::Bookmark> luciole::SidebarTreeView::bookmark(EntryId id) const {
  const auto* entry = m_model.entry(id);
  return entry ? std::optional(Bookmark{entry->displayName(), entry->navigationLocation(), {}}) : std::nullopt;
}
InputArea* luciole::SidebarTreeView::focusArea(const Location& location) {
  const auto found = std::ranges::find_if(m_model.rows(), [&](EntryId id) {
    return m_model.entry(id)->navigationLocation() == location;
  });
  if (found == m_model.rows().end()) {
    return nullptr;
  }
  m_reveal = true;
  m_model.select(*found);
  return m_grid->focusArea();
}
std::optional<luciole::Bookmark> luciole::SidebarTreeView::folderAt(float x, float y) const {
  const auto index = m_grid->itemAtScene(x, y);
  return index ? bookmark(m_model.rows()[*index]) : std::nullopt;
}
std::optional<LayoutRect> luciole::SidebarTreeView::folderRect(const Location& location) const {
  for (std::size_t i = 0; i < m_model.rows().size(); ++i) {
    if (m_model.entry(m_model.rows()[i])->navigationLocation() == location) {
      if (const auto rect = m_grid->itemRect(i)) {
        return rect;
      }
    }
  }
  return std::nullopt;
}
std::optional<std::size_t> luciole::SidebarTreeView::dropSlot(float x, float y) const {
  if (!containsScenePoint(x, y) || folderAt(x, y)) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < m_model.rows().size(); ++i) {
    const auto id = m_model.rows()[i];
    if (m_pinIndices.contains(id)) {
      if (const auto rect = m_grid->itemRect(i); rect && y < rect->y + rect->height * 0.5F) {
        return m_pinIndices.at(id);
      }
    }
  }
  return m_pinCount;
}
LayoutRect luciole::SidebarTreeView::dropLine(std::size_t slot) const {
  float x = 0, y = 0;
  Node::absolutePosition(this, x, y);
  for (std::size_t i = 0; i < m_model.rows().size(); ++i) {
    const auto found = m_pinIndices.find(m_model.rows()[i]);
    if (found != m_pinIndices.end() && found->second >= slot) {
      if (const auto rect = m_grid->itemRect(i)) {
        return {rect->x, rect->y - 2, rect->width, 2};
      }
    }
  }
  float line = y;
  for (std::size_t i = 0; i < m_model.rows().size(); ++i) {
    if (m_pinIndices.contains(m_model.rows()[i])) {
      if (const auto rect = m_grid->itemRect(i)) {
        line = std::max(line, rect->y + rect->height + 2);
      }
    }
  }
  return {x, std::min(line, y + height() - 2), width(), 2};
}
void luciole::SidebarTreeView::dragScroll(float delta) { m_grid->scrollDuringDrag(delta); }
float luciole::SidebarTreeView::dragScrollSpeed(float x, float y) const {
  if (!containsScenePoint(x, y)) {
    return 0;
  }
  float localX = 0, localY = 0;
  Node::mapFromScene(this, x, y, localX, localY);
  return localY < 28 ? -6 : localY > height() - 28 ? 6 : 0;
}
void luciole::SidebarTreeView::cancelDragCandidate() { m_grid->cancelPointerGesture(); }
bool luciole::SidebarTreeView::key(std::uint32_t sym, std::uint32_t modifiers) {
  using K = luciole::KeybindCommand;
  if (m_model.rows().empty()) {
    return false;
  }
  auto id = m_model.selection().focused.value_or(m_model.rows().front());
  const auto bookmark = this->bookmark(id);
  if (luciole::keybindMatches(K::Validate, sym, modifiers)) {
    if (open && bookmark) {
      open(bookmark->location);
    }
    return true;
  }
  if (luciole::keybindMatches(luciole::KeybindCommand::ContextMenu, sym, modifiers)) {
    if (context && bookmark) {
      const auto rect = folderRect(bookmark->location);
      if (rect) {
        context(*bookmark, rect->x + rect->width / 2, rect->y + rect->height / 2);
      }
    }
    return true;
  }
  if (luciole::keybindMatches(luciole::KeybindCommand::Rename, sym, modifiers) && m_pinIndices.contains(id)) {
    if (rename && bookmark && bookmark->location != Location::network()) {
      rename(*bookmark);
    }
    return true;
  }
  const auto match = luciole::keybindBestMatch(luciole::keybindMovementCommands(), sym, modifiers);
  if (!match) {
    return false;
  }
  const auto info = m_tree.row(id);
  if (match && (match->command == K::MoveLeft || match->command == K::MoveRight)) {
    if ((match->command == K::MoveRight) != Style::rtl()) {
      if (!info.expanded) {
        (void)m_tree.expand(id);
      } else {
        const auto child =
            std::ranges::find_if(m_model.rows(), [&](EntryId row) { return m_model.treeParent(row) == id; });
        if (child != m_model.rows().end()) {
          id = *child;
        }
      }
    } else if (info.expanded) {
      (void)m_tree.collapse(id);
    } else if (const auto parent = m_model.treeParent(id)) {
      id = *parent;
    }
  } else if (match) {
    auto index = static_cast<std::ptrdiff_t>(std::ranges::find(m_model.rows(), id) - m_model.rows().begin());
    switch (match->command) {
    case K::MoveDown:
      ++index;
      break;
    case K::MoveUp:
      --index;
      break;
    case K::MoveFirst:
      index = 0;
      break;
    case K::MoveLast:
      index = static_cast<std::ptrdiff_t>(m_model.rows().size()) - 1;
      break;
    case K::MovePageDown:
      index += static_cast<std::ptrdiff_t>(m_grid->pageItemStride());
      break;
    case K::MovePageUp:
      index -= static_cast<std::ptrdiff_t>(m_grid->pageItemStride());
      break;
    default:
      return false;
    }
    index = std::clamp<std::ptrdiff_t>(index, 0, static_cast<std::ptrdiff_t>(m_model.rows().size()) - 1);
    id = m_model.rows()[static_cast<std::size_t>(index)];
  }
  m_reveal = true;
  m_model.select(id);
  return true;
}
