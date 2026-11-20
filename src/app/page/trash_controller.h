#pragma once

#include "fs/directory_session.h"

#include <memory>
#include <optional>
#include <vector>

class Button;
class Flex;
class Label;
class OperationJob;
class PageContext;
struct KeyboardEvent;

// Trash, permanent deletion and reporting restored items.
class TrashController {
public:
  explicit TrashController(PageContext& page);

  void trashItems(const std::vector<luciole::DirectoryEntry>& targets);
  void restore(const std::vector<luciole::DirectoryEntry>& targets);
  void deleteItems(const std::vector<luciole::DirectoryEntry>& targets);
  // Empties Trash, or permanently deletes `selection`, after the user confirms.
  void openEmptyTrash(std::optional<std::vector<luciole::DirectoryEntry>> selection = std::nullopt);
  // With no focused control, Enter cancels the confirmation. Left/Right move focus between its buttons.
  bool key(const KeyboardEvent& event);
  // Owned job changes: sync the confirmation dialog, or report a finished job.
  void trashChanged(OperationJob& job);
  void emptyTrashChanged(OperationJob& job);
  void restoreChanged(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame() and pumpJobs().
  void onFrame();

private:
  bool canTrashItems(const std::vector<luciole::DirectoryEntry>& targets) const;
  void confirmEmptyTrash();
  void syncEmptyTrash();

  PageContext& m_page;
  // The deletion the confirmation dialog shows, until it is confirmed or the dialog closes.
  std::shared_ptr<OperationJob> m_emptyTrash;
  bool m_emptyTrashDirty = false;
  Label* m_emptyTrashMessage = nullptr;
  Label* m_emptyTrashSize = nullptr;
  Button* m_emptyTrashConfirm = nullptr;
  Flex* m_emptyTrashButtonRow = nullptr;
};
