#pragma once

#include "browser/browser_controller.h"
#include "model/details_columns.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/controls/virtual_grid_view.h"

#include <chrono>

namespace luciole {
  class FileViewAdapter final : public VirtualGridAdapter {
  public:
    FileViewAdapter(
        BrowserController& browser, FileIconCache& icons, ThumbnailService& thumbnails, ViewMode mode,
        std::function<std::uint32_t()> modifiers, std::function<bool()> focused
    );
    std::size_t itemCount() const override;
    std::string itemTooltip(std::size_t index) const override;
    [[nodiscard]] std::unique_ptr<Node> createTile() override;
    std::unique_ptr<Node> createDecoration() override;
    void bindDecoration(Node& decoration, const VirtualGridDecorationLayout& layout) override;
    void setTileVisible(Node& tile, bool visible) override;
    void releaseTile(Node& tile, Renderer& renderer) override;
    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override;
    bool onPointerPress(std::size_t index, float, float, float, float) override;
    bool onPointerDrag(std::optional<std::size_t>, float, float, float, float) override;
    PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override;
    void onPointerCancel() override;
    bool onBackgroundPointerPress() override;
    bool onAreaSelection(const std::vector<std::size_t>& indices) override;
    bool areaSelectionCurrent() const override;
    bool itemHitTest(std::size_t index, float x, float y, float width, float height) const override;
    void onActivate(std::size_t index) override;
    void onSecondaryActivate(std::size_t index, float x, float y) override;
    void onBackgroundSecondaryActivate(float x, float y) override;
    void setContextCallback(std::function<void(std::optional<EntryId>, float, float)> callback);
    void setDragCallback(std::function<void(const std::vector<EntryId>&)> start, std::function<void()> cancel);
    void setActivateCallback(std::function<void(EntryId)> callback) { m_activate = std::move(callback); }
    void setCutPredicate(std::function<bool(const Location&)> predicate) { m_cut = std::move(predicate); }
    // List rows only; other adapters stay dormant.
    void setFolderSizeEnabled(bool enabled) { m_folderSizes = enabled; }
    void setActivationMode(ActivationMode mode) { m_activationMode = mode; }
    void setPresentation(BrowsePresentation presentation, std::optional<bool> readOnly) {
      m_presentation = presentation;
      m_readOnly = readOnly;
    }
    void setGridIconSize(float size) { m_gridIconSize = size; }
    // List and Columns rows; 0 keeps the view's default size.
    void setRowIconSize(int size) { m_rowIconSize = size; }
    void setDetailsColumns(DetailsColumns columns) { m_columns = columns; }
    void setDetailsColumnOrder(DetailsColumnOrder order) { m_columnOrder = order; }
    void setColumnActive(bool active) { m_columnActive = active; }
    void setPathEntry(std::optional<EntryId> entry) { m_pathEntry = entry; }

  private:
    struct TileState {
      std::optional<EntryId> entry;
      bool visible = false;
    };
    void reveal(Node& tile, EntryId id);

    BrowserController& m_browser;
    FileIconCache& m_icons;
    ThumbnailService& m_thumbnails;
    const ViewMode m_mode;
    bool m_columnActive = true;
    std::optional<EntryId> m_pathEntry;
    std::function<std::uint32_t()> m_modifiers;
    std::function<bool()> m_focused;
    std::function<bool(const Location&)> m_cut;
    std::optional<EntryId> m_pressed;
    std::optional<EntryId> m_expanderPress;
    std::optional<EntryId> m_lastClick;
    std::uint32_t m_pressModifiers = 0, m_lastClickModifiers = 0;
    std::chrono::steady_clock::time_point m_lastClickTime;
    std::function<void(EntryId)> m_activate;
    ActivationMode m_activationMode = ActivationMode::DoubleClick;
    bool m_dragged = false;
    bool m_activationClick = false;
    bool m_collapseSelectionOnClick = false;
    bool m_toggleSelectionOnClick = false;
    std::function<void(std::optional<EntryId>, float, float)> m_context;
    std::function<void(const std::vector<EntryId>&)> m_drag;
    std::function<void()> m_cancelDrag;
    BrowsePresentation m_presentation;
    std::optional<bool> m_readOnly;
    bool m_folderSizes = false;
    float m_gridIconSize = static_cast<float>(kDefaultGridIconSize);
    int m_rowIconSize = 0;
    DetailsColumns m_columns;
    DetailsColumnOrder m_columnOrder = kDefaultDetailsColumnOrder;
    std::vector<EntryId> m_areaRows;
    std::unordered_set<EntryId> m_areaBaseline;
    std::uint64_t m_areaGeneration = 0;
    bool m_areaSelection = false;
    bool m_areaToggle = false;
    std::unordered_map<const Node*, TileState> m_tiles;
    std::unordered_set<EntryId> m_revealed; // Entries that already faded in during this visit.
    std::uint64_t m_revealVisit = 0;
  };
} // namespace luciole
