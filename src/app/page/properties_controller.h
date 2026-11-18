#pragma once

#include "browser/selection_details.h"
#include "fs/cancel_token.h"
#include "thumbnails/preview_metadata.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace luciole {
  class FileIcon;
  class FileIconCache;
  struct DirectoryEntry;
} // namespace luciole

struct KeyboardEvent;
class AclEditor;
class BulkAclEditor;
class Button;
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

// The Properties dialog: general facts, and for up to 4096 items the permission and ACL editors with their results.
class PropertiesController {
public:
  PropertiesController(PageContext& page, luciole::FileIconCache& icons);

  // `captureCurrentLocation` reads the permissions of the first target's location instead of its listed entry.
  void show(const std::vector<luciole::DirectoryEntry>& targets, bool captureCurrentLocation = false);
  // Owned permission job change: sync the dialog on the next frame, or report a job whose dialog closed.
  void changed(OperationJob& job);
  // Operation progress tick: keep syncing while permissions are read or applied.
  void progressTick();
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // BrowserPage::syncBrowser() hook: Apply is enabled only while no file operation runs.
  void updateApply();
  // Keyboard hook in BrowserPage::keyboard() while a popup is shown: keyboard navigation in the results list.
  bool resultsKey(const KeyboardEvent& event);
  // Dialog layout hook with the dialog's width: re-wraps the Details table.
  void layoutDialog(float width);

private:
  struct Body;

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
  void close();

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
  // The header icon, and the GIO lookup that fills it for entries without an icon.
  luciole::FileIcon* m_identityIcon = nullptr;
  luciole::CancelToken m_iconQuery;
  // Location, size and similar entry facts that lead the Details table.
  luciole::MetadataFields m_basicFields;
  float m_metadataWidth = 0;
  bool m_metadataDirty = false;
  bool m_dirty = false;
};
