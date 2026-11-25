#pragma once

#include "browser/places.h"
#include "fs/drive_monitor.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"

class Box;
class ProgressBar;

namespace luciole {
  class SidebarTreeView;
  // The action name for removing a drive this way: "Safely Remove", "Eject" or "Unmount"; empty for None.
  std::string_view driveRemovalLabel(DriveRemoval removal);
  // "X free of Y (Z% used)" for a drive's usage; `usage.size` must be nonzero.
  std::string driveSpaceText(const DriveUsage& usage);

  class PlacesSidebar final : public Flex {
  public:
    PlacesSidebar(PinService& pins, DriveMonitor& drives);
    void setTreeEnabled(bool enabled);
    void setShowHidden(bool showHidden);
    void setShowDrives(bool showDrives);
    void setShowNetwork(bool showNetwork);
    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }
    void invalidate();
    void releaseLocation(const Location& root);
    // Updates drive usage bars from the monitor without rebuilding the list.
    void updateDriveUsage();
    void sync(std::optional<Location> current);
    void setTrashFull(bool full);
    void cancelDragCandidate();
    std::optional<PinSlot> dropSlot(float x, float y) const;
    LayoutRect dropLine(PinSlot slot) const;
    void dragScroll(float delta);
    float dragScrollSpeed(float x, float y) const;
    void setOpenCallback(std::function<void(const Location&)> callback);
    void setContextCallback(std::function<void(const Bookmark&, float, float)> callback);
    // Right-click on a separator line: the Places row it sits before, and the pointer's scene position.
    void setSeparatorContextCallback(std::function<void(std::size_t, float, float)> callback) {
      m_separatorContext = std::move(callback);
    }
    // A separator dragged from in front of row `from` and dropped in front of row `to`.
    void setSeparatorMoveCallback(std::function<void(std::size_t from, std::size_t to)> callback) {
      m_separatorMove = std::move(callback);
    }
    void setDragCallback(std::function<void(const Bookmark&)> callback);
    void setRenameCallback(std::function<void(const Bookmark&)> callback);
    // Drive rows: open mounts the drive first when needed; remove unmounts or ejects it.
    void setDriveOpenCallback(std::function<void(const std::string&)> callback);
    void setDriveRemoveCallback(std::function<void(const std::string&)> callback);
    void setDriveContextCallback(std::function<void(const std::string&, float, float)> callback);
    InputArea* focusArea(const Location& location) const;
    std::optional<Bookmark> folderAt(float x, float y) const;
    // Drop targets reserve row edges for pin insertion and reordering; navigation uses the whole row.
    std::optional<Bookmark> dropFolderAt(float x, float y) const;
    std::optional<LayoutRect> folderRect(const Location& location) const;

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  private:
    struct Row {
      Bookmark bookmark;
      std::optional<std::size_t> pinIndex;
      Button* button = nullptr;
      // The separator line drawn above the row, as the row it is saved in front of, its button and the line itself,
      // which can have a heading under it.
      std::optional<std::size_t> separator;
      Button* separatorButton = nullptr;
      Node* separatorLine = nullptr;
    };
    struct SeparatorRow {
      Button* button = nullptr;
      Node* line = nullptr;
    };
    struct DriveRow {
      std::string key;
      std::optional<Location> root;
      std::string label;
      Button* button = nullptr;
      Label* name = nullptr;
      Glyph* eject = nullptr;
      ProgressBar* usage = nullptr;
      bool selected = false;
    };
    void rebuild();
    SeparatorRow addSeparatorRow(std::size_t before);
    // The row a dragged separator at scene `y` would sit in front of, if it can go there.
    std::optional<std::size_t> separatorTarget(float y) const;
    void showSeparatorIndicator(std::optional<std::size_t> target);
    void addRow(const Bookmark& bookmark, const std::string& glyph, std::optional<std::size_t> index);
    void addDriveRow(const Drive& drive);
    std::optional<Bookmark> folderAt(float x, float y, float verticalInset) const;
    // Fits drive names between the glyph and the eject button and places usage bars under them, lifting the name
    // so name and bar center together; true when a name's width budget changed and the rows need another arrange.
    bool layoutDriveRows(Renderer& renderer);
    const Drive* driveAt(const Location& root) const;
    PinService& m_pins;
    DriveMonitor& m_drives;
    SidebarTreeView* m_tree = nullptr;
    bool m_treeEnabled = false, m_showHidden = false, m_showDrives = true, m_showNetwork = true;
    std::function<void()> m_changed;
    ScrollView* m_scroll = nullptr;
    Label* m_heading = nullptr;
    std::vector<Row> m_rows;
    std::vector<DriveRow> m_driveRows;
    std::optional<Location> m_current;
    std::optional<Bookmark> m_dragCandidate;
    float m_pressX = 0;
    float m_pressY = 0;
    bool m_dirty = true;
    bool m_trashFull = false;
    bool m_dragged = false;
    std::function<void(const Location&)> m_open;
    std::function<void(const Bookmark&, float, float)> m_context;
    std::function<void(std::size_t, float, float)> m_separatorContext;
    std::function<void(std::size_t, std::size_t)> m_separatorMove;
    // A separator being dragged within Places: the row it sits in front of, where the press started and the row
    // it would now move in front of.
    struct SeparatorDrag {
      std::size_t from = 0;
      float pressY = 0;
      bool moved = false;
      std::optional<std::size_t> target;
    };
    std::optional<SeparatorDrag> m_separatorDrag;
    Box* m_separatorIndicator = nullptr;
    std::function<void(const Bookmark&)> m_drag;
    std::function<void(const Bookmark&)> m_rename;
    std::function<void(const std::string&)> m_driveOpen;
    std::function<void(const std::string&)> m_driveRemove;
    std::function<void(const std::string&, float, float)> m_driveContext;
  };
} // namespace luciole
