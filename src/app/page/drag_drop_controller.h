#pragma once

#include "browser/directory_model.h"
#include "browser/places.h"
#include "clipboard/text_clipboard.h"
#include "config/state_store.h"
#include "core/timer_manager.h"
#include "fs/location.h"
#include "render/scene/node.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace luciole {
  class FileIcon;
  class FileIconCache;
  class MillerColumns;
  class PinService;
  class ThumbnailService;
  struct FileIconImage;
} // namespace luciole

struct PointerEvent;
class AnimationManager;
class Box;
class Flex;
class Label;
class LocationBarController;
class PageContext;
class Renderer;
class SidebarController;
class TransferController;
class VirtualGridView;
class WaylandConnection;

// Dragging files and places: file drags handed to the compositor, place drags inside the page, the drag icon and
// count, folder and pin drop targets, autoscroll near the edges, and drops that arrive from other windows or pages.
class DragDropController {
public:
  DragDropController(
      PageContext& page, WaylandConnection& connection, luciole::PinService& pins,
      TransferController& transferController, luciole::ThumbnailService& thumbnails, luciole::FileIconCache& icons,
      luciole::MillerColumns& miller, SidebarController& sidebar, LocationBarController& locationBar
  );

  // True while a file or place drag runs.
  bool dragging() const { return m_drag.has_value(); }
  // True while the compositor runs the file drag.
  bool systemDrag() const { return m_systemDrag; }

  // Drops from outside the page, at the pointer position the page tracks.
  void clearExternalDrop();
  const std::vector<luciole::Bookmark>* draggedFolders() const;
  luciole::FileDropTarget externalDrop(float x, float y, const std::vector<luciole::Bookmark>* pins);
  luciole::FileDropTarget externalDestination(const luciole::Location& destination, LayoutRect rect);
  // The folder under (x, y) that a drag from another page drops into.
  std::optional<std::pair<luciole::Location, LayoutRect>> foreignTarget(float x, float y) const;
  void setForeignDrop(std::function<std::optional<std::pair<luciole::Location, LayoutRect>>(float, float)> callback);

  // Remembers the serial of a left press on the page, which starts a compositor drag.
  void trackButton(const PointerEvent& event);
  void startFileDrag(const std::vector<luciole::EntryId>& ids);
  void startPlaceDrag(const luciole::Bookmark& bookmark);
  void cancelDrag();
  // The file view ended its gesture: cancel a file drag the page runs itself.
  void cancelFileDrag();
  // Cancels any drag except one the compositor runs.
  void cancelLocalDrag();
  void updateDrag();
  // Drops at the pointer position on the left button release.
  void finishDrag();
  // Page teardown: stop the autoscroll timers.
  void stop();

  // Scene: the overlay above the page, mounted beside the page root.
  void buildOverlay(AnimationManager& animations);
  void mount(Node& host);
  void unmount();
  // Layout hooks in BrowserPage::prepareFrame(): autoscroll, the drop target highlight and the drag icon.
  void applyScroll();
  void layout(Renderer& renderer);
  // Asks the compositor to re-query drop targets after the next layout.
  void requestDropRefresh() { m_refreshExternalDrop = true; }
  void refreshDrop();

private:
  struct EntryTarget {
    luciole::EntryId id;
    luciole::Location location;
  };
  struct Drag {
    std::vector<EntryTarget> entries;
    std::vector<luciole::Bookmark> bookmarks;
    std::optional<luciole::Location> sourcePin;
    bool foldersOnly = false;
  };
  struct DropTarget {
    luciole::Location location;
    LayoutRect rect;
    std::optional<luciole::EntryId> entry = std::nullopt;
  };

  bool dragCurrent() const;
  bool validDropFolder(const luciole::Location& destination) const;
  std::optional<DropTarget> dropTarget() const;
  std::optional<luciole::PinSlot> pinDropSlot() const;
  VirtualGridView* dragView() const;
  float dragScrollSpeed(bool& places) const;
  void updateExternalScroll();

  PageContext& m_page;
  WaylandConnection& m_connection;
  luciole::PinService& m_pins;
  TransferController& m_transferController;
  luciole::ThumbnailService& m_thumbnails;
  luciole::FileIconCache& m_icons;
  luciole::MillerColumns& m_miller;
  SidebarController& m_sidebar;
  LocationBarController& m_locationBar;
  std::function<std::optional<std::pair<luciole::Location, LayoutRect>>(float, float)> m_foreignDrop;
  std::optional<Drag> m_drag;
  std::shared_ptr<const luciole::FileIconImage> m_dragArtwork;
  std::uint32_t m_fileDragSerial = 0;
  bool m_externalHover = false;
  bool m_externalDraggingHere = false;
  bool m_refreshExternalDrop = false;
  bool m_systemDrag = false;
  Timer m_externalScroll;
  float m_externalScrollDelta = 0;
  bool m_externalScrollInPlaces = false;
  std::optional<luciole::PinSlot> m_externalPinSlot;
  std::optional<DropTarget> m_externalDropTarget;
  Timer m_dragScroll;
  float m_dragScrollDelta = 0;
  bool m_dragScrollInPlaces = false;
  std::optional<DropTarget> m_lastDropTarget;
  Node* m_dragOverlay = nullptr;
  std::unique_ptr<Node> m_detachedDragOverlay;
  luciole::FileIcon* m_dragIcon = nullptr;
  Flex* m_dragCount = nullptr;
  Label* m_dragCountLabel = nullptr;
  Box* m_folderDropHighlight = nullptr;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
