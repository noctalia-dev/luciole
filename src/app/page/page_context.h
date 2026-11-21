#pragma once

#include "app/workspace_command.h"
#include "browser/action_registry.h"
#include "fs/location.h"
#include "model/directory_options.h"
#include "render/scene/node.h"
#include "ui/controls/context_menu.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  class BrowserController;
  class WaylandTextClipboard;
  class WindowSurface;
  struct BrowserSettings;
  struct DirectoryEntry;
} // namespace luciole

class ActivePopup;
class ContextMenuControl;
class DialogDropdown;
class Flex;
class InputDispatcher;
class MillerView;
class PageJobs;
class PopupSurface;
class VirtualGridView;

// Whether a click outside a popup closes it. Menus and popovers use OutsideClick; forms keep Explicit so typed input
// is never lost to a stray click.
enum class PopupDismiss : std::uint8_t { Explicit, OutsideClick };

// Dialog width tiers: Small for one-field prompts and confirmations, Medium for forms and inspectors, Large for
// multi-pane editors. A dialog is never wider than the window minus the dialog margin on each side.
enum class DialogWidth : std::uint8_t { Small, Medium, Large };

// The part of BrowserPage that feature controllers use. BrowserPage implements it.
//
// A controller (`page/<feature>_controller.{h,cpp}`) owns one feature of the page:
// - its widget pointers, dirty flag and feature state;
// - references to the page-local services it drives, which BrowserPage owns and passes to its constructor;
// - its open*/submit* entry points and sync(), which applies the service snapshot to its widgets.
//
// File jobs belong to the application's OperationHub, one service instance per job. A controller starts one
// with jobs().create<Service>() and observes the jobs the page owns, which include jobs taken over from a closed
// page: BrowserPage::jobChanged() hands every owned job change to the controller of its kind. A job that only
// feeds a dialog is created with JobScope::Page and released with jobs().follow() once it applies changes.
//
// Frame order: a service's changed callback (BrowserPage constructor or jobChanged()) calls the controller's
// markDirty(). BrowserPage::prepareFrame() calls onFrame() at the feature's fixed slot in the dirty -> sync
// sequence, and onFrame() runs sync() only when the flag was set. Keep that slot so syncs run in the same order
// every frame.
//
// Dialogs: show the root with showDialog(). If popupSurface() exists afterwards, keep the widget pointers and
// call activePopup().bind(kind, onClose). onClose is the only teardown path: closePopup() runs it whatever
// closed the popup, and it cancels the dialog's job and drops the pointers. When the page left its window
// (!jobs().attached()), a job that already applies changes keeps running for the page that takes it over.
// Never size a dialog root yourself: the page sets its width from the tier and its height from its content.
//
// Input, layout and scene hooks that the page cannot route generically yet are small controller methods the
// page calls at the original spot.
class PageContext {
public:
  virtual luciole::BrowserController& browser() = 0;
  virtual luciole::ActionRequest requestFor(const std::vector<luciole::DirectoryEntry>& entries) const = 0;
  virtual luciole::ActionState
  actionState(luciole::BrowserAction action, const luciole::ActionRequest& request) const = 0;
  // True while every target still matches the current folder listing.
  virtual bool targetsCurrent(const std::vector<luciole::DirectoryEntry>& targets) = 0;
  // True while the request's folder, targets or selection for `scope` still match the page.
  virtual bool requestCurrent(const luciole::ActionRequest& request, luciole::ActionScope scope) const = 0;
  // True while a job the page owns runs or waits (see PageJobs::busy()), or a paste waits for the clipboard;
  // actions that start a job are disabled meanwhile.
  virtual bool operationsBusy() const = 0;
  // Titles of the file jobs running anywhere in the application.
  virtual std::vector<std::string> runningJobTitles() const = 0;
  // True while the page is shown in its window.
  virtual bool visible() const = 0;
  // Shows `message` in the status bar.
  virtual void actionError(std::string message) = 0;
  // Replaces the status bar message without scheduling a page sync, for code that runs before one anyway.
  virtual void setActionMessage(std::string message) = 0;
  // Selects `targets` once the current folder reloads.
  virtual void revealOnReload(std::vector<luciole::Location> targets) = 0;
  // The file jobs this page starts and owns.
  virtual PageJobs& jobs() = 0;
  // Cancels a file or place drag in progress.
  virtual void cancelDrag() = 0;
  virtual luciole::WaylandTextClipboard& clipboard() = 0;
  virtual luciole::WindowSurface& surface() = 0;
  // Input of the window the page is bound to; shared by every page and the tab strip on that surface.
  virtual InputDispatcher& dispatcher() = 0;
  // Gives keyboard focus back to the file list, if there is one and the page is the active pane.
  virtual void focusFileList() = 0;
  // Focuses the sidebar entry for `location` once the page is active and no popup or overlay is open.
  virtual void restorePlaceFocus(luciole::Location location) = 0;
  // Closes the active dialog, drags and location editing before a full-page overlay opens.
  virtual void prepareOverlay() = 0;
  // Hosts full-page overlays.
  virtual Node& sceneRoot() = 0;

  // The page's saved browser settings, which Settings edits.
  virtual const luciole::BrowserSettings& settings() const = 0;
  // Applies changed settings to the page; `force` reapplies fields that did not change.
  virtual void applySettings(const luciole::BrowserSettings& settings, bool force) = 0;
  // True while the page is the focused pane of its window.
  virtual bool active() const = 0;
  // True while Settings or the archive browser covers the page.
  virtual bool overlay() const = 0;
  // True while thumbnail work is paused for this page.
  virtual bool thumbnailsPaused() const = 0;

  // The current folder, and the selected items when `withTargets`.
  virtual luciole::ActionRequest actionRequest(bool withTargets) const = 0;
  // Runs `action` on the current selection, or on the folder for folder-scoped actions.
  virtual void dispatchAction(luciole::BrowserAction action) = 0;
  virtual void dispatchAction(luciole::BrowserAction action, const luciole::ActionRequest& request) = 0;
  // True while the window shows another pane next to this one.
  virtual bool hasOtherPane() const = 0;
  // True while the page chooses files for a FileChooser portal request: no launching, menus, drags or jobs.
  virtual bool choosing() const = 0;
  // Sends `command` to the workspace, if it listens; `pending` becomes the request a pane transfer uses.
  virtual void requestWorkspace(
      WorkspaceCommand command, std::optional<luciole::Location> location, std::optional<luciole::ActionRequest> pending
  ) = 0;
  // Scene rectangle of the view options button.
  virtual LayoutRect menuAnchor() const = 0;
  // The toolbar's parent folder button.
  virtual Node& upButton() = 0;
  // Syncs the page to its browser on the next frame.
  virtual void requestSync() = 0;
  // Last pointer position on the page surface.
  virtual float pointerX() const = 0;
  virtual float pointerY() const = 0;

  // File view.
  // The focused file grid; null before the scene is built.
  virtual VirtualGridView* fileList() = 0;
  // The pane that holds the file grid or the Miller columns.
  virtual Flex& filePane() = 0;
  virtual MillerView& millerView() = 0;
  virtual luciole::ViewMode viewMode() const = 0;
  virtual float gridIconSize() const = 0;
  virtual float rowIconSize() const = 0;
  // Keeps the first visible item in view across the next layout change.
  virtual void captureViewAnchor() = 0;
  // Shows `mode` without saving it anywhere.
  virtual void showViewMode(luciole::ViewMode mode) = 0;
  // The visible order changed: show the beginning of the new order.
  virtual void sortChanged() = 0;
  // Cancels file pointer gestures, drops the view anchor and scrolls the file list to the top.
  virtual void resetFileView() = 0;

  // Replaces the active popup with a menu or transient popover anchored at `anchor`.
  virtual void showPopup(
      std::unique_ptr<Flex> root, ContextMenuControl* menu, LayoutRect anchor, float width, float height, bool alignLeft
  ) = 0;
  // Replaces the active popup with a context menu. Activating an entry closes it and calls `action` with the entry id
  // on the next main loop iteration; dismissing it without activation drops the callback.
  virtual void
  showContextMenu(std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor) = 0;
  // Shows `root` as a modal dialog centered on the window, with a drop shadow, and focuses its first control.
  // Its height follows its content, capped by the window. While it stays open its top edge stays put, so
  // switching to a shorter or taller page or tab only moves the bottom edge.
  // `dismiss` OutsideClick closes it on a click outside, like a popover.
  virtual void
  showDialog(std::unique_ptr<Flex> root, DialogWidth width, PopupDismiss dismiss = PopupDismiss::Explicit) = 0;
  // The content width a dialog of tier `width` gets in the window now; for content laid out at a fixed width.
  virtual float dialogWidth(DialogWidth width) const = 0;
  // Closes the active dialog on the next main loop iteration.
  virtual void dismissPopup() = 0;
  // Closes the active dialog now.
  virtual void closePopup() = 0;
  // Null while no popup is shown.
  virtual PopupSurface* popupSurface() = 0;
  virtual InputDispatcher& popupDispatcher() = 0;
  virtual ActivePopup& activePopup() = 0;
  // Select dropdown host for dialog roots: setPopupContext(&dialogDropdown()).
  virtual DialogDropdown& dialogDropdown() = 0;
  // The context menu on the popup; null while the popup shows a dialog or nothing.
  virtual ContextMenuControl* contextMenu() = 0;
  // False while the workspace cannot show a popup for this page.
  virtual bool popupAvailable() = 0;
  // Serial for a new popup grab: the last press, else the last input.
  virtual std::uint32_t popupSerial() = 0;
  // Tallest a context menu popup may be: the height of the window's output, or of the window while that is unknown,
  // less a margin. Popups may extend past the window.
  virtual float popupMaxHeight() const = 0;

protected:
  PageContext() = default;
  PageContext(const PageContext&) = delete;
  PageContext& operator=(const PageContext&) = delete;
  ~PageContext() = default;
};

// Scene rectangle of `node`.
inline LayoutRect sceneRect(const Node& node) {
  float x = 0, y = 0;
  Node::absolutePosition(&node, x, y);
  return {x, y, node.width(), node.height()};
}
