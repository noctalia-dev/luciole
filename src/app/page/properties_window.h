#pragma once

#include "browser/selection_details.h"
#include "thumbnails/preview_metadata.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace luciole {
  class FileIconCache;
  class WindowSurface;
  struct DirectoryEntry;
} // namespace luciole

struct KeyboardEvent;
class AclEditor;
class BulkAclEditor;
class Button;
class DialogWindow;
class Flex;
class Label;
class FileMetadataView;
class FolderTotalsView;
class OperationJob;
class PageContext;
class PermissionEditor;
class PermissionResultsView;
class ScrollView;
class Segmented;

// One Properties window: general facts, and for up to 4096 items the permission and ACL editors with their results.
// PropertiesController owns one per open window.
class PropertiesWindow {
public:
  // `closed` runs once the window closed by itself (its buttons, Escape or the compositor); it destroys this object.
  PropertiesWindow(PageContext& page, luciole::FileIconCache& icons, std::function<void(PropertiesWindow&)> closed);
  ~PropertiesWindow();
  PropertiesWindow(const PropertiesWindow&) = delete;
  PropertiesWindow& operator=(const PropertiesWindow&) = delete;

  // `captureCurrentLocation` reads the permissions of the first target's location instead of its listed entry.
  // False when no window could be shown.
  bool show(const std::vector<luciole::DirectoryEntry>& targets, bool captureCurrentLocation);
  // Closes the window now. Its job is cancelled unless it applies changes for a page that left its window.
  void close();
  // True while `job` reads or applies this window's permissions.
  bool owns(const OperationJob& job) const { return m_job.get() == &job; }
  // This window's job changed: sync on the window's next frame.
  void jobChanged();
  // Operation progress tick: keep syncing while permissions are read or applied.
  void progressTick();
  // BrowserPage::syncBrowser() hook: Apply is enabled only while no file operation runs.
  void updateApply();
  // The page moved to another browser window.
  void setParent(luciole::WindowSurface& parent);
  void requestLayout();

private:
  struct Body;

  // Keyboard navigation in the results list.
  bool resultsKey(const KeyboardEvent& event);
  // The window's frame: syncs what changed, so the window stays current while its page's tab is hidden.
  void frame(float width);
  // Re-wraps the Details table for the window's width.
  void layoutDialog(float width);

  Body buildGeneral(const std::vector<luciole::DirectoryEntry>& targets, bool editable);
  Body buildBody(
      const std::vector<luciole::DirectoryEntry>& targets, bool editable, bool multiple, bool folders, bool withResults
  );
  void showAdvanced(bool advanced, bool focus = false);
  // Swaps the permission body for the per-item outcomes of the last bulk or recursive job.
  void showDetails(bool shown);
  bool canApply() const;
  void sync();
  void syncMetadata();

  PageContext& m_page;
  // The dialog's job: reads the targets, then applies changes.
  std::shared_ptr<OperationJob> m_job;
  luciole::FileIconCache& m_icons;
  PermissionEditor* m_permissionEditor = nullptr;
  AclEditor* m_aclEditor = nullptr;
  BulkAclEditor* m_bulkAclEditor = nullptr;
  Flex *m_permissionBasic = nullptr, *m_permissionAdvanced = nullptr;
  Button *m_permissionAdvancedOpen = nullptr, *m_permissionAdvancedBack = nullptr;
  bool m_aclActive = false;
  PermissionResultsView* m_permissionResults = nullptr;
  Button *m_permissionReport = nullptr, *m_permissionCancel = nullptr, *m_permissionDetails = nullptr;
  bool m_detailsShown = false;
  Label* m_permissionStatus = nullptr;
  Segmented* m_permissionTabs = nullptr;
  ScrollView* m_permissionScroll = nullptr;
  Button* m_permissionSave = nullptr;
  luciole::PreviewMetadataService m_metadata;
  luciole::SelectionDetailsService m_selectionMetadata;
  bool m_multiple = false;
  FileMetadataView* m_metadataView = nullptr;
  FolderTotalsView* m_folderTotals = nullptr;
  // Location, size and similar entry facts that lead the Details table.
  luciole::MetadataFields m_basicFields;
  float m_metadataWidth = 0;
  bool m_metadataDirty = false;
  bool m_dirty = false;
  std::unique_ptr<DialogWindow> m_window;
  std::function<void(PropertiesWindow&)> m_closed;
};
