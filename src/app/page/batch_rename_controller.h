#pragma once

#include "fs/location.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace luciole {
  struct BatchRenameSnapshot;
  struct DirectoryEntry;
} // namespace luciole

class BatchRenameDialog;
class OperationJob;
class PageContext;
class Renderer;
class VirtualGridView;

// Batch rename dialog: preview, apply and report a rename of several items, then select the results.
class BatchRenameController {
public:
  explicit BatchRenameController(PageContext& page);

  void open(const std::vector<luciole::DirectoryEntry>& targets);
  // Owned batch rename job change: sync the dialog on the next frame, or report a job whose dialog closed.
  void changed(OperationJob& job);
  // Operation progress tick: keep syncing while the rename applies.
  void progressTick();
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();

  // Menu popup hooks.
  void pointer(float x, float y, bool hover);
  void syncTooltip(Renderer& renderer);
  // File list hook in BrowserPage::syncBrowser(): select the renamed items once the folder reloads.
  void revealRenamed(VirtualGridView& list);

private:
  void sync();
  void report(const OperationJob& job);

  PageContext& m_page;
  // The dialog's job: previews, then applies the rename.
  std::shared_ptr<OperationJob> m_job;
  BatchRenameDialog* m_dialog = nullptr;
  bool m_dirty = false;
  bool m_reported = false;
  std::vector<luciole::Location> m_reveal;
  std::uint64_t m_revealContext = 0;
};
