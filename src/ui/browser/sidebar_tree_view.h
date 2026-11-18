#pragma once

#include "browser/directory_tree.h"
#include "browser/places.h"
#include "fs/drive_monitor.h"
#include "ui/controls/flex.h"

class VirtualGridView;
class InputArea;
namespace luciole {
  // The optional virtualized folder hierarchy, rooted in the existing Places destinations.
  class SidebarTreeView final : public Flex {
  public:
    SidebarTreeView();
    ~SidebarTreeView() override;
    // Pins first, then mounted drives; only pins take part in reordering.
    void setRoots(const PinService& pins, const std::vector<Drive>& drives, bool showNetwork, bool trashFull);
    void sync(std::optional<Location> current, bool showHidden);
    void prepare();
    void releaseLocation(const Location& root) { m_tree.releaseLocation(root); }
    InputArea* focusArea(const Location& location);
    std::optional<Bookmark> folderAt(float x, float y) const;
    std::optional<LayoutRect> folderRect(const Location& location) const;
    std::optional<std::size_t> dropSlot(float x, float y) const;
    LayoutRect dropLine(std::size_t slot) const;
    void dragScroll(float delta);
    float dragScrollSpeed(float x, float y) const;
    void cancelDragCandidate();
    std::function<void(const Location&)> open;
    std::function<void(const Bookmark&, float, float)> context;
    std::function<void(const Bookmark&)> drag, rename;
    std::function<void()> changed;

  private:
    class Adapter;
    friend class Adapter;
    bool key(std::uint32_t sym, std::uint32_t modifiers);
    std::optional<Bookmark> bookmark(EntryId id) const;
    DirectoryModel m_model;
    DirectoryTree m_tree{m_model, true};
    std::unique_ptr<Adapter> m_adapter;
    VirtualGridView* m_grid = nullptr;
    std::optional<Location> m_current;
    std::map<std::string, EntryId> m_rootIds;
    std::map<EntryId, std::size_t> m_pinIndices;
    std::map<EntryId, std::string> m_glyphs;
    EntryId m_nextRoot = 1;
    std::size_t m_pinCount = 0;
    bool m_dirty = true, m_reveal = false;
  };
} // namespace luciole
