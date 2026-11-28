#pragma once

#include "browser/miller_columns.h"
#include "ui/browser/file_view.h"
#include "ui/controls/box.h"
#include "ui/controls/scrollbar.h"

#include <map>

class Label;
class TextInputClient;
class MillerView final : public Flex {
public:
  MillerView(luciole::MillerColumns& columns, luciole::FileIconCache& icons, luciole::ThumbnailService& thumbnails);
  ~MillerView() override;
  void synchronize();
  void loadVisible();
  void suspend();
  void revealFocused();
  void scrollBy(float delta) { scroll(m_offset + delta); }
  bool resizing() const { return m_resizing.has_value(); }
  VirtualGridView* focusedView() const;
  std::optional<std::size_t> columnAt(float x, float y) const;
  VirtualGridView* view(std::size_t index) const;
  void setRenamingEntry(std::optional<luciole::EntryId> entry);
  std::optional<LayoutRect> entryNameRect(luciole::EntryId id, float singleLineHeight = 0.0F) const;
  LayoutRect viewportRect() const;
  std::function<std::uint32_t()> modifiers;
  std::function<bool()> keyboardFocus;
  std::function<bool(const luciole::Location&)> cut;
  std::function<void(std::size_t)> focus;
  std::function<void(luciole::EntryId)> activate;
  std::function<void(std::optional<luciole::EntryId>, float, float)> context;
  std::function<void(const std::vector<luciole::EntryId>&)> drag;
  std::function<void()> cancelDrag;
  std::function<void(int)> resized;
  std::function<void()> requestUpdate;
  TextInputClient* textInputClient = nullptr;
  luciole::BrowsePresentation presentation;
  int iconSize = 0; // 0 keeps the density's size.
  luciole::ActivationMode activationMode = luciole::ActivationMode::DoubleClick;

private:
  struct ColumnView {
    std::unique_ptr<luciole::FileViewAdapter> adapter;
    Flex* root;
    VirtualGridView* grid;
    Label* notice;
    InputArea* divider;
    Box* line;
    std::optional<luciole::EntryId> path = std::nullopt;
    float rowHeight = 36.0F;
  };
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void scroll(float offset);
  std::size_t indexOf(std::uint64_t id) const;
  luciole::MillerColumns& m_columns;
  luciole::FileIconCache& m_icons;
  luciole::ThumbnailService& m_thumbnails;
  Box* m_viewport;
  InputArea* m_wheel;
  Scrollbar* m_scrollbar;
  std::map<std::uint64_t, ColumnView> m_views;
  float m_offset = 0, m_extent = 0;
  bool m_reveal = true;
  std::optional<std::size_t> m_revealColumn;
  std::optional<std::uint64_t> m_resizing;
  float m_resizeStart = 0, m_resizeWidth = 0;
  float m_resizePreferredWidth = 0;
  bool m_resizeFixedWidth = false;
  float m_layoutWidth = 0;
  std::optional<luciole::EntryId> m_renamingEntry;
};
