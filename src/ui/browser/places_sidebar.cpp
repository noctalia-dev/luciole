#include "ui/browser/places_sidebar.h"

#include "browser/location_presentation.h"
#include "config/keybinds.h"
#include "fs/gio_ptr.h"
#include "model/directory_options.h"
#include "ui/browser/sidebar_tree_view.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/list_drag.h"
#include "ui/controls/side_list_metrics.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <gio/gio.h>
#include <string>

namespace {
  constexpr float kUsageBarHeight = 3.0F;
  constexpr float kUsageWarningFraction = 0.9F;
  // Gap between the font's descent line and a usage bar, so descenders never reach the bar.
  constexpr float kUsageBarGap = 2.0F;

  // Colors a drive row's usage bar for its selection, and shows the bar only for a known usage.
  void applyDriveState(ProgressBar& bar, const std::optional<luciole::DriveUsage>& usage, bool selected) {
    bar.setVisible(usage.has_value());
    if (!usage) {
      return;
    }
    const auto fraction = static_cast<float>(static_cast<double>(usage->used) / static_cast<double>(usage->size));
    bar.setProgress(fraction);
    const auto fill = fraction >= kUsageWarningFraction ? ColorRole::Error
        : selected                                      ? ColorRole::OnPrimary
                                                        : ColorRole::Primary;
    bar.setFillColor(colorSpecFromRole(fill));
    bar.setTrackColor(colorSpecFromRole(selected ? ColorRole::PrimaryContainer : ColorRole::SurfaceContainerHighest));
  }
  // Hover text for a drive row: where it is mounted and how full it is. Usage
  // arrives after the rows are built, so this is recomputed whenever the usage
  // refresh lands. The row already shows the name.
  std::string driveTooltip(const luciole::Drive& drive) {
    if (!drive.root) {
      return "Not mounted";
    }
    // Edit text covers both native paths and URIs, so network mounts keep an address line.
    std::string text = luciole::locationEditText(*drive.root);
    if (drive.usage && drive.usage->size > 0) {
      if (!text.empty()) {
        text += '\n';
      }
      text += luciole::driveSpaceText(*drive.usage);
    }
    return text;
  }
} // namespace

std::string_view luciole::driveRemovalLabel(DriveRemoval removal) {
  switch (removal) {
  case DriveRemoval::PowerOff:
    return "Safely Remove";
  case DriveRemoval::Eject:
    return "Eject";
  case DriveRemoval::Unmount:
    return "Unmount";
  case DriveRemoval::None:
    break;
  }
  return {};
}

std::string luciole::driveSpaceText(const DriveUsage& usage) {
  const auto used = std::min(usage.used, usage.size);
  const GString freeText(g_format_size_full(usage.size - used, G_FORMAT_SIZE_IEC_UNITS));
  const GString sizeText(g_format_size_full(usage.size, G_FORMAT_SIZE_IEC_UNITS));
  const auto percent = std::llround(100.0 * static_cast<double>(used) / static_cast<double>(usage.size));
  return std::format(
      "{} free of {} ({}% used)", freeText ? freeText.get() : "?", sizeText ? sizeText.get() : "?", percent
  );
}

luciole::PlacesSidebar::PlacesSidebar(PinService& pins, DriveMonitor& drives) : m_pins(pins), m_drives(drives) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setMinWidth(static_cast<float>(kDefaultSidebarWidth));
  setMaxWidth(static_cast<float>(kDefaultSidebarWidth));
  // Rows sit on the window chrome. Their right inset is the 8px resize-handle gap before the content
  // (kWorkspaceInset), so the left inset matches it and nothing else pads the rows horizontally.
  setPadding(luciole::kSideListPadding, 0.0F, luciole::kSideListPadding, Style::spaceSm);
  addChild(
      ui::scrollView(
          {.out = &m_scroll,
           .viewportPaddingH = 0.0F,
           .viewportPaddingV = luciole::kSideListViewportPaddingV,
           .fillWidth = true,
           .flexGrow = 1.0F}
      )
  );
  m_scroll->content()->setAlign(FlexAlign::Stretch);
  m_scroll->content()->setGap(Style::spaceXs);
}
void luciole::PlacesSidebar::releaseLocation(const Location& root) {
  if (m_tree) {
    m_tree->releaseLocation(root);
  }
}

void luciole::PlacesSidebar::setOpenCallback(std::function<void(const Location&)> callback) {
  m_open = std::move(callback);
}
void luciole::PlacesSidebar::setContextCallback(std::function<void(const Bookmark&, float, float)> callback) {
  m_context = std::move(callback);
}
void luciole::PlacesSidebar::setDragCallback(std::function<void(const Bookmark&)> callback) {
  m_drag = std::move(callback);
}
void luciole::PlacesSidebar::setRenameCallback(std::function<void(const Bookmark&)> callback) {
  m_rename = std::move(callback);
}
void luciole::PlacesSidebar::setDriveOpenCallback(std::function<void(const std::string&)> callback) {
  m_driveOpen = std::move(callback);
}
void luciole::PlacesSidebar::setDriveRemoveCallback(std::function<void(const std::string&)> callback) {
  m_driveRemove = std::move(callback);
}
void luciole::PlacesSidebar::setDriveContextCallback(std::function<void(const std::string&, float, float)> callback) {
  m_driveContext = std::move(callback);
}
const luciole::Drive* luciole::PlacesSidebar::driveAt(const Location& root) const {
  if (!m_showDrives || m_pins.contains(root)) {
    return nullptr;
  }
  const auto& drives = m_drives.drives();
  const auto found = std::ranges::find_if(drives, [&](const Drive& drive) {
    const auto& target = drive.activation ? drive.activation : drive.root;
    return target == root;
  });
  return found == drives.end() ? nullptr : &*found;
}
InputArea* luciole::PlacesSidebar::focusArea(const Location& location) const {
  if (m_tree) {
    return m_tree->focusArea(location);
  }
  const auto found = std::ranges::find_if(m_rows, [&](const auto& row) { return row.bookmark.location == location; });
  if (found != m_rows.end()) {
    return found->button->inputArea();
  }
  const auto drive = std::ranges::find(m_driveRows, std::optional(location), &DriveRow::root);
  return drive == m_driveRows.end() ? nullptr : drive->button->inputArea();
}
void luciole::PlacesSidebar::invalidate() {
  m_dirty = true;
  markLayoutDirty();
}
void luciole::PlacesSidebar::updateDriveUsage() {
  if (m_tree) {
    return;
  }
  for (const auto& row : m_driveRows) {
    if (const auto* drive = m_drives.find(row.key); drive != nullptr && drive->root == row.root) {
      applyDriveState(*row.usage, drive->usage, row.selected);
      row.button->setTooltip(driveTooltip(*drive));
    }
  }
  markLayoutDirty();
}
std::optional<luciole::Bookmark> luciole::PlacesSidebar::folderAt(float x, float y) const {
  return folderAt(x, y, 0.0F);
}
std::optional<luciole::Bookmark> luciole::PlacesSidebar::dropFolderAt(float x, float y) const {
  return folderAt(x, y, 8.0F);
}
std::optional<luciole::Bookmark> luciole::PlacesSidebar::folderAt(float x, float y, float verticalInset) const {
  if (m_tree) {
    return m_tree->folderAt(x, y);
  }
  if (!containsScenePoint(x, y)) {
    return std::nullopt;
  }
  const auto hit = [&](const Button* button) {
    float localX = 0, localY = 0;
    return Node::mapFromScene(button, x, y, localX, localY)
        && localX >= 0
        && localX < button->width()
        && localY >= verticalInset
        && localY < button->height() - verticalInset;
  };
  for (const auto& row : m_rows) {
    if (hit(row.button)) {
      return row.bookmark;
    }
  }
  for (const auto& row : m_driveRows) {
    if (row.root && hit(row.button)) {
      return Bookmark{row.label, *row.root, {}};
    }
  }
  return std::nullopt;
}
std::optional<LayoutRect> luciole::PlacesSidebar::folderRect(const Location& location) const {
  if (m_tree) {
    return m_tree->folderRect(location);
  }
  const auto rect = [this](const Button* button) {
    float x = 0, y = 0;
    Node::absolutePosition(button, x, y);
    float left = 0, top = 0;
    Node::absolutePosition(this, left, top);
    const float bottom = std::min(y + button->height(), top + height());
    const float clippedTop = std::max(y, top);
    return LayoutRect{x, clippedTop, button->width(), std::max(0.0F, bottom - clippedTop)};
  };
  for (const auto& row : m_rows) {
    if (row.bookmark.location == location) {
      return rect(row.button);
    }
  }
  for (const auto& row : m_driveRows) {
    if (row.root == location) {
      return rect(row.button);
    }
  }
  return std::nullopt;
}
void luciole::PlacesSidebar::cancelDragCandidate() {
  if (m_tree) {
    m_tree->cancelDragCandidate();
  }
  m_dragCandidate.reset();
  m_dragged = false;
}
void luciole::PlacesSidebar::sync(std::optional<Location> current) {
  m_current = std::move(current);
  if (m_tree) {
    m_tree->sync(m_current, m_showHidden);
  }
  bool highlighted = false;
  for (auto& row : m_rows) {
    const bool selected = !highlighted
        && (m_current == row.bookmark.location
            || (row.bookmark.location == Location::trash() && m_current && m_current->isTrash()));
    row.button->setSelected(selected);
    highlighted = highlighted || selected;
  }
  for (auto& row : m_driveRows) {
    const bool selected = !highlighted && row.root && m_current == row.root;
    row.button->setSelected(selected);
    row.selected = selected;
    if (const auto* drive = m_drives.find(row.key); drive != nullptr && drive->root == row.root) {
      applyDriveState(*row.usage, drive->usage, selected);
    }
    highlighted = highlighted || selected;
  }
  markLayoutDirty();
}
void luciole::PlacesSidebar::setTrashFull(bool full) {
  if (std::exchange(m_trashFull, full) != full) {
    invalidate();
  }
}
void luciole::PlacesSidebar::addDriveRow(const Drive& drive) {
  auto button = ui::button(
      {.glyph = drive.glyph,
       .glyphSize = luciole::kSideListGlyphSize,
       .controlHeight = luciole::kSideListRowHeight,
       .contentAlign = ButtonContentAlign::Start,
       .variant = ButtonVariant::Ghost,
       .paddingH = luciole::kSideListRowPaddingH,
       .gap = luciole::kSideListRowGap}
  );
  auto* raw = button.get();
  raw->setDirection(FlexDirection::Horizontal);
  // The row's own label, so long names lose their middle as in GNOME Files; Button ellipsizes its label's end.
  auto* name = static_cast<Label*>(raw->addChild(
      ui::label({.text = drive.label, .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F})
  ));
  name->setUseMarkup(false);
  raw->setTooltip(driveTooltip(drive));
  raw->inputArea()->setTabFocusKey("drive:" + drive.key);
  raw->inputArea()->setOnKeyDown([this, key = drive.key, raw](const InputArea::KeyData& event) {
    if (!event.pressed) {
      return;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::Validate, event.sym, event.modifiers)) {
      if (m_driveOpen) {
        m_driveOpen(key);
      }
    } else if (luciole::keybindMatches(luciole::KeybindCommand::ContextMenu, event.sym, event.modifiers)) {
      float x = 0, y = 0;
      Node::absolutePosition(raw, x, y);
      if (m_driveContext) {
        m_driveContext(key, x + raw->width() * 0.5F, y + raw->height() * 0.5F);
      }
    }
  });
  raw->setOnRightClickWithPointer([this, key = drive.key](float x, float y, std::uint32_t, std::uint32_t) {
    if (m_driveContext) {
      m_driveContext(key, x, y);
    }
  });
  raw->inputArea()->setOnFocusGain([this, raw] {
    raw->setKeyboardFocusHint(true);
    m_scroll->requestScrollIntoView(raw);
  });
  raw->inputArea()->setOnFocusLoss([raw] { raw->setKeyboardFocusHint(false); });
  // The eject glyph is part of the row: one input area keeps hover steady, and a press over the glyph's end of
  // the row removes the drive instead of opening it.
  Glyph* eject = nullptr;
  if (drive.removal != DriveRemoval::None) {
    eject = static_cast<Glyph*>(
        raw->addChild(ui::glyph({.glyph = "player-eject", .glyphSize = luciole::kSideListGlyphSize}))
    );
  }
  struct EjectPointer {
    bool over = false;
    bool pressed = false;
  };
  const auto pointer = std::make_shared<EjectPointer>();
  const auto overEject = [raw, eject](float x) { return eject != nullptr && x >= eject->x() - raw->gap() * 0.5F; };
  raw->setOnPointerMotion([this, raw, pointer, overEject, key = drive.key,
                           ejectTooltip = std::string(driveRemovalLabel(drive.removal))](float x, float) {
    if (const bool over = overEject(x); over != pointer->over) {
      pointer->over = over;
      if (over) {
        raw->setTooltip(ejectTooltip);
      } else if (const auto* current = m_drives.find(key); current != nullptr) {
        raw->setTooltip(driveTooltip(*current));
      }
    }
  });
  raw->setOnLeave([this, raw, pointer, key = drive.key] {
    *pointer = {};
    if (const auto* current = m_drives.find(key); current != nullptr) {
      raw->setTooltip(driveTooltip(*current));
    }
  });
  raw->setOnPress([pointer, overEject](float x, float, bool pressed) {
    if (pressed) {
      pointer->pressed = overEject(x);
    }
  });
  raw->setOnClick([this, key = drive.key, pointer] {
    const bool remove = std::exchange(pointer->pressed, false) && pointer->over;
    const auto& callback = remove ? m_driveRemove : m_driveOpen;
    if (callback) {
      callback(key);
    }
  });
  // Laid out by hand in doArrange(): under the name, from its start to the eject glyph.
  auto* usage = static_cast<ProgressBar*>(raw->addChild(
      ui::progressBar({.track = colorSpecFromRole(ColorRole::SurfaceContainerHighest), .height = kUsageBarHeight})
  ));
  usage->setParticipatesInLayout(false);
  applyDriveState(*usage, drive.usage, false);
  m_driveRows.push_back(
      {.key = drive.key,
       .root = drive.root,
       .label = drive.label,
       .button = raw,
       .name = name,
       .eject = eject,
       .usage = usage}
  );
  m_scroll->content()->addChild(std::move(button));
}
Button* luciole::PlacesSidebar::addSeparatorRow(std::size_t before) {
  // A short ghost button around the line, so it highlights on hover, can be right-clicked and dragged.
  auto button = ui::button(
      {.controlHeight = 13.0F,
       .contentAlign = ButtonContentAlign::Start,
       .variant = ButtonVariant::Ghost,
       .paddingH = luciole::kSideListRowPaddingH}
  );
  auto* raw = button.get();
  raw->setDirection(FlexDirection::Horizontal);
  raw->setAlign(FlexAlign::Center);
  raw->setTooltip("Drag to move, or right-click for options");
  raw->addChild(ui::separator({.orientation = SeparatorOrientation::HorizontalRule, .flexGrow = 1.0F}));
  raw->setOnRightClickWithPointer([this, before](float x, float y, std::uint32_t, std::uint32_t) {
    cancelDragCandidate();
    if (m_separatorContext) {
      m_separatorContext(before, x, y);
    }
  });
  // While held, the button's own pressed state marks the separator being dragged.
  raw->setOnPress([this, before](float, float localY, bool pressed) {
    if (pressed) {
      m_separatorDrag = SeparatorDrag{.from = before, .pressY = localY, .moved = false, .target = std::nullopt};
      return;
    }
    auto drag = std::exchange(m_separatorDrag, std::nullopt);
    showSeparatorIndicator(std::nullopt);
    if (drag && drag->moved && drag->target && m_separatorMove) {
      m_separatorMove(drag->from, *drag->target);
    }
  });
  raw->setOnPointerMotion([this, raw](float, float localY) {
    if (!m_separatorDrag) {
      return;
    }
    auto& drag = *m_separatorDrag;
    drag.moved = drag.moved || std::abs(localY - drag.pressY) >= Style::dragStartThreshold;
    if (!drag.moved) {
      return;
    }
    float x = 0, y = 0;
    Node::absolutePosition(raw, x, y);
    drag.target = separatorTarget(y + localY);
    showSeparatorIndicator(drag.target);
  });
  raw->inputArea()->setOnCancel([this] {
    m_separatorDrag.reset();
    showSeparatorIndicator(std::nullopt);
  });
  m_scroll->content()->addChild(std::move(button));
  return raw;
}
std::optional<std::size_t> luciole::PlacesSidebar::separatorTarget(float y) const {
  // The first row whose upper half is below the pointer; past the last row there is nothing to separate.
  std::optional<std::size_t> target;
  for (const auto& row : m_rows) {
    float rowX = 0, rowY = 0;
    Node::absolutePosition(row.button, rowX, rowY);
    if (row.pinIndex && y < rowY + row.button->height() * 0.5F) {
      target = row.pinIndex;
      break;
    }
  }
  // Its own gap has a separator too: the one being dragged.
  return target && SeparatorGaps(m_pins, m_showNetwork).free(*target) ? target : std::nullopt;
}
void luciole::PlacesSidebar::showSeparatorIndicator(std::optional<std::size_t> target) {
  if (m_separatorIndicator == nullptr) {
    return;
  }
  const auto row = target ? std::ranges::find(m_rows, target, &Row::pinIndex) : m_rows.end();
  if (row == m_rows.end()) {
    m_separatorIndicator->setVisible(false);
    return;
  }
  // The line the separator would become, drawn in the gap above its new row.
  m_separatorIndicator->setPosition(Style::spaceSm, row->button->y() - 1.5F);
  m_separatorIndicator->setFrameSize(std::max(1.0F, m_scroll->content()->width() - 2.0F * Style::spaceSm), 3.0F);
  m_separatorIndicator->setVisible(true);
}
void luciole::PlacesSidebar::addRow(
    const Bookmark& bookmark, const std::string& glyph, std::optional<std::size_t> index
) {
  auto button = ui::button(
      {.text = bookmark.label,
       .glyph = glyph,
       .glyphSize = luciole::kSideListGlyphSize,
       .controlHeight = luciole::kSideListRowHeight,
       .contentAlign = ButtonContentAlign::Start,
       .variant = ButtonVariant::Ghost,
       .paddingH = luciole::kSideListRowPaddingH,
       .gap = luciole::kSideListRowGap,
       .onClick = [this, location = bookmark.location] {
         if (m_open) {
           m_open(location);
         }
       }}
  );
  auto* raw = button.get();
  raw->setDirection(FlexDirection::Horizontal);
  raw->label()->setUseMarkup(false);
  raw->setTooltip(locationEditText(bookmark.location));
  raw->inputArea()->setTabFocusKey("place:" + bookmark.location.uri());
  raw->inputArea()->setOnKeyDown([this, bookmark, raw](const InputArea::KeyData& key) {
    if (!key.pressed) {
      return;
    }
    if (luciole::keybindMatches(luciole::KeybindCommand::Validate, key.sym, key.modifiers)) {
      if (m_open) {
        m_open(bookmark.location);
      }
    } else if (luciole::keybindMatches(luciole::KeybindCommand::Rename, key.sym, key.modifiers)) {
      if (m_rename && !bookmark.location.isTrash() && bookmark.location != Location::network()) {
        m_rename(bookmark);
      }
    } else if (luciole::keybindMatches(luciole::KeybindCommand::ContextMenu, key.sym, key.modifiers)) {
      float x = 0, y = 0;
      Node::absolutePosition(raw, x, y);
      if (m_context) {
        m_context(bookmark, x + raw->width() * 0.5F, y + raw->height() * 0.5F);
      }
    }
  });
  raw->setOnRightClickWithPointer([this, bookmark](float x, float y, std::uint32_t, std::uint32_t) {
    cancelDragCandidate();
    if (m_context) {
      m_context(bookmark, x, y);
    }
  });
  if (index) {
    raw->inputArea()->setOnPress([this, bookmark, raw](const InputArea::PointerData& pointer) {
      raw->setPressedVisual(pointer.pressed);
      if (pointer.button == BTN_LEFT && pointer.pressed) {
        m_dragCandidate = bookmark;
        m_pressX = pointer.sceneX;
        m_pressY = pointer.sceneY;
        m_dragged = false;
      } else if (!pointer.pressed) {
        cancelDragCandidate();
      }
    });
    raw->setOnPointerMotion([this, raw](float x, float y) {
      float originX = 0, originY = 0;
      Node::absolutePosition(raw, originX, originY);
      if (m_dragCandidate
          && !m_dragged
          && std::hypot(originX + x - m_pressX, originY + y - m_pressY) >= Style::dragStartThreshold) {
        m_dragged = true;
        const auto bookmark = *m_dragCandidate;
        if (m_drag) {
          m_drag(bookmark);
        }
      }
    });
    raw->inputArea()->setOnCancel([this, raw] {
      raw->setPressedVisual(false);
      cancelDragCandidate();
    });
  }
  raw->inputArea()->setOnFocusGain([this, raw] {
    raw->setKeyboardFocusHint(true);
    m_scroll->requestScrollIntoView(raw);
  });
  raw->inputArea()->setOnFocusLoss([raw] { raw->setKeyboardFocusHint(false); });
  m_rows.push_back(
      {.bookmark = bookmark, .pinIndex = index, .button = raw, .separator = {}, .separatorButton = nullptr}
  );
  m_scroll->content()->addChild(std::move(button));
}
void luciole::PlacesSidebar::setTreeEnabled(bool enabled) {
  if (std::exchange(m_treeEnabled, enabled) != enabled) {
    invalidate();
  }
}
void luciole::PlacesSidebar::setShowHidden(bool showHidden) {
  if (std::exchange(m_showHidden, showHidden) != showHidden && m_tree) {
    m_tree->sync(m_current, showHidden);
  }
}
void luciole::PlacesSidebar::setShowDrives(bool showDrives) {
  if (std::exchange(m_showDrives, showDrives) != showDrives) {
    invalidate();
  }
}
void luciole::PlacesSidebar::setShowNetwork(bool showNetwork) {
  if (std::exchange(m_showNetwork, showNetwork) != showNetwork) {
    invalidate();
  }
}
void luciole::PlacesSidebar::rebuild() {
  if (m_treeEnabled) {
    m_scroll->setVisible(false);
    m_scroll->setParticipatesInLayout(false);
    if (!m_tree) {
      auto tree = std::make_unique<SidebarTreeView>();
      m_tree = tree.get();
      tree->open = [this](const Location& location) {
        if (m_open) {
          m_open(location);
        }
      };
      tree->context = [this](const Bookmark& bookmark, float x, float y) {
        if (const auto* drive = driveAt(bookmark.location)) {
          if (m_driveContext) {
            m_driveContext(drive->key, x, y);
          }
        } else if (m_context) {
          m_context(bookmark, x, y);
        }
      };
      tree->drag = [this](const Bookmark& bookmark) {
        if (m_drag) {
          m_drag(bookmark);
        }
      };
      tree->rename = [this](const Bookmark& bookmark) {
        if (m_rename) {
          m_rename(bookmark);
        }
      };
      tree->changed = [this] {
        if (m_changed) {
          m_changed();
        }
      };
      addChild(std::move(tree));
    }
    // Explicit expansion can activate an idle system automount through DirectorySession.
    std::vector<Drive> drives;
    if (m_showDrives) {
      std::ranges::copy_if(m_drives.drives(), std::back_inserter(drives), [this](const Drive& drive) {
        const auto& target = drive.activation ? drive.activation : drive.root;
        return target && !m_pins.contains(*target);
      });
    }
    m_tree->setRoots(m_pins, drives, m_showNetwork, m_trashFull);
    m_tree->sync(m_current, m_showHidden);
    return;
  }
  if (m_tree) {
    removeChild(m_tree);
    m_tree = nullptr;
  }
  m_scroll->setVisible(true);
  m_scroll->setParticipatesInLayout(true);
  cancelDragCandidate();
  m_rows.clear();
  m_driveRows.clear();
  auto* content = m_scroll->content();
  while (!content->children().empty()) {
    content->removeChild(content->children().back().get());
  }
  const auto heading = [&](const std::string& text, Label** out = nullptr) {
    content->addChild(
        ui::label(
            {.out = out,
             .text = text,
             .fontSize = Style::fontSizeCaption,
             .fontWeight = FontWeight::SemiBold,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .height = 24.0F}
        )
    );
  };
  heading("Places", &m_heading);
  // Where a dragged separator would land; drawn over the rows, outside the layout.
  m_separatorDrag.reset();
  content->addChild(makeListDropIndicator(1.0F, &m_separatorIndicator));
  const auto& pins = m_pins.bookmarks();
  const SeparatorGaps gaps(m_pins, m_showNetwork);
  for (std::size_t i = 0; i < pins.size(); ++i) {
    if (!placeShown(pins[i], m_showNetwork)) {
      continue;
    }
    const auto separator = gaps.separatorAbove(i);
    auto* line = separator ? addSeparatorRow(*separator) : nullptr;
    const auto known = std::ranges::find_if(m_pins.userPlaces(), [&](const auto& place) {
      return place.bookmark.location == pins[i].location;
    });
    addRow(
        pins[i],
        !pins[i].iconName.empty()                ? pins[i].iconName
            : pins[i].location.isTrash()         ? (m_trashFull ? "trash-filled" : "trash")
            : known == m_pins.userPlaces().end() ? "folder"
                                                 : known->glyphName,
        i
    );
    m_rows.back().separator = separator;
    m_rows.back().separatorButton = line;
  }
  if (m_showDrives && !m_drives.drives().empty()) {
    content->addChild(ui::column({.align = FlexAlign::Stretch, .paddingV = Style::spaceXs}, ui::separator({})));
    heading("Drives");
    for (const auto& drive : m_drives.drives()) {
      addDriveRow(drive);
    }
    m_drives.refreshUsage();
  }
  sync(m_current);
}
LayoutSize luciole::PlacesSidebar::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  if (m_dirty) {
    m_dirty = false;
    rebuild();
  }
  if (m_tree) {
    m_tree->prepare();
  }
  return Flex::doMeasure(renderer, constraints);
}
void luciole::PlacesSidebar::doArrange(Renderer& renderer, const LayoutRect& rect) {
  if (m_dirty) {
    m_dirty = false;
    rebuild();
  }
  if (m_tree) {
    m_tree->prepare();
  }
  Flex::doArrange(renderer, rect);
  if (layoutDriveRows(renderer)) {
    Flex::doArrange(renderer, rect);
    layoutDriveRows(renderer);
  }
}
bool luciole::PlacesSidebar::layoutDriveRows(Renderer& renderer) {
  bool changed = false;
  for (const auto& row : m_driveRows) {
    auto* button = row.button;
    float end = button->width() - button->paddingRight();
    if (row.eject != nullptr) {
      end -= row.eject->width() + button->gap();
    }
    const float start = row.name->x();
    const float budget = std::ceil(std::max(1.0F, end - start));
    if (row.name->maxWidth() != budget) {
      row.name->setMaxWidth(budget);
      changed = true;
    }
    if (!row.usage->visible()) {
      continue;
    }
    // Font metrics, not the string's ink, so every drive's name and bar sit at the same height.
    const auto font = renderer.measureFont(row.name->fontSize(), row.name->font());
    const float barOffset = font.bottom + kUsageBarGap;
    const float baseline = std::round((button->height() + font.capHeight - barOffset - kUsageBarHeight) * 0.5F);
    row.name->setPosition(row.name->x(), baseline - row.name->baselineOffset());
    row.usage->setPosition(start, baseline + std::round(barOffset));
    row.usage->setSize(std::max(0.0F, end - start), kUsageBarHeight);
  }
  return changed;
}
std::optional<luciole::PinSlot> luciole::PlacesSidebar::dropSlot(float x, float y) const {
  if (m_tree) {
    // The tree draws no separators: a drop goes above the line of its gap.
    const auto slot = m_tree->dropSlot(x, y);
    return slot ? std::optional(PinSlot{.before = *slot}) : std::nullopt;
  }
  float localX = 0, localY = 0;
  if (!Node::mapFromScene(this, x, y, localX, localY)
      || localX < 0
      || localY < 0
      || localX >= width()
      || localY >= height()) {
    return std::nullopt;
  }
  // A gap with a separator line has two slots, split at the line's middle: above it, in front of the row it is saved
  // in front of, and below it, in front of the row under it.
  const auto above = [y](const Button* button) {
    float buttonX = 0, buttonY = 0;
    Node::absolutePosition(button, buttonX, buttonY);
    return y < buttonY + button->height() * 0.5F;
  };
  for (const auto& row : m_rows) {
    if (!row.pinIndex) {
      continue;
    }
    if (row.separatorButton != nullptr && above(row.separatorButton)) {
      return PinSlot{.before = *row.separator};
    }
    if (above(row.button)) {
      return PinSlot{.before = *row.pinIndex, .belowSeparator = row.separatorButton != nullptr};
    }
  }
  return PinSlot{.before = m_pins.bookmarks().size()};
}
LayoutRect luciole::PlacesSidebar::dropLine(PinSlot slot) const {
  if (m_tree) {
    return m_tree->dropLine(slot.before);
  }
  float x = 0, y = 0;
  Node::absolutePosition(this, x, y);
  const float top = y;
  float line = top + height() - 8.0F;
  if (m_heading != nullptr) {
    float headingX = 0;
    Node::absolutePosition(m_heading, headingX, line);
    line += m_heading->height() + 2.0F;
  }
  for (const auto& row : m_rows) {
    if (!row.pinIndex) {
      continue;
    }
    float rowX = 0, rowY = 0;
    Node::absolutePosition(row.button, rowX, rowY);
    line = rowY - 2.0F;
    if (*row.pinIndex >= slot.before) {
      // Above the separator line for a drop above it.
      if (row.separatorButton != nullptr && !slot.belowSeparator) {
        Node::absolutePosition(row.separatorButton, rowX, rowY);
        line = rowY - 2.0F;
      }
      break;
    }
    line = rowY + row.button->height() + 2.0F;
  }
  return {x + 8.0F, std::clamp(line, top + 2.0F, top + height() - 3.0F), width() - 16.0F, 2.0F};
}
void luciole::PlacesSidebar::dragScroll(float delta) {
  if (m_tree) {
    m_tree->dragScroll(delta);
    return;
  }
  m_scroll->requestScrollToOffset(m_scroll->scrollOffset() + delta);
}
float luciole::PlacesSidebar::dragScrollSpeed(float x, float y) const {
  if (m_tree) {
    return m_tree->dragScrollSpeed(x, y);
  }
  float localX = 0, localY = 0;
  if (!Node::mapFromScene(this, x, y, localX, localY) || localX < 0 || localX >= width()) {
    return 0;
  }
  if (localY < 28 && m_scroll->scrollOffset() > 0) {
    return -6;
  }
  if (localY > height() - 28 && m_scroll->scrollOffset() < m_scroll->maxScrollOffset()) {
    return 6;
  }
  return 0;
}
