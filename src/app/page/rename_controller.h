#pragma once

#include "config/state_store.h"
#include "fs/directory_session.h"

#include <memory>
#include <optional>
#include <string>

namespace luciole {
  class PinService;
} // namespace luciole

class NameDialog;
class GlyphPicker;
class OperationJob;
class PageContext;
class Renderer;

// Single-item rename of a file or folder, and renaming a place in the sidebar.
class RenameController {
public:
  RenameController(PageContext& page, luciole::PinService& pins);

  void openFileRename(luciole::DirectoryEntry target);
  void openPlaceRename(const luciole::Bookmark& bookmark);
  void openPlaceIconPicker(const luciole::Bookmark& bookmark);
  // Owned rename job change: sync the editor on the next frame, or report a job whose editor closed.
  void changed(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // BrowserPage::prepareFrame() hook before syncBrowser(): applies completed place edits to their dialogs.
  void syncPlaceEditors();
  void placeIconPointer(float x, float y, bool hover);
  void syncPlaceIconTooltip(Renderer& renderer);

private:
  bool canFileRename(const luciole::DirectoryEntry& entry) const;
  void submitFileRename(std::string name);
  void syncFileRename(const OperationJob& job);
  void submitPlaceRename(std::string label);
  void submitPlaceIcon(std::string iconName);
  bool refreshPlaceIconTarget();

  PageContext& m_page;
  luciole::PinService& m_pins;
  // The editor's job: checks the target, then renames it.
  std::shared_ptr<OperationJob> m_job;
  NameDialog* m_fileRenameEditor = nullptr;
  std::optional<luciole::DirectoryEntry> m_fileRenameTarget;
  bool m_fileRenamePreparing = false;
  bool m_dirty = false;
  NameDialog* m_placeEditor = nullptr;
  GlyphPicker* m_placeIconPicker = nullptr;
  std::optional<luciole::Bookmark> m_placeRenameTarget;
  std::optional<std::string> m_pendingPlaceLabel;
  std::optional<luciole::Bookmark> m_placeIconTarget;
  std::optional<std::string> m_pendingPlaceIcon;
};
