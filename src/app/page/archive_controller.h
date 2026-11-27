#pragma once

#include "fs/location.h"
#include "render/scene/node.h"
#include "ui/controls/archive_view.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  class FileIconCache;
  class FileLauncher;
  struct DirectoryEntry;
} // namespace luciole

struct KeyboardEvent;
class OperationJob;
class PageContext;
class Renderer;
class WaylandConnection;

// Archives: the compress and extract dialogs, extracting next to the archive, and the full-page archive browser.
class ArchiveController {
public:
  ArchiveController(
      PageContext& page, luciole::FileLauncher& launcher, luciole::FileIconCache& icons, WaylandConnection& connection
  );
  ~ArchiveController();

  void compress(const std::vector<luciole::DirectoryEntry>& targets);
  void extractTo(std::vector<luciole::Location> archives);
  void extractHere(std::vector<luciole::Location> archives);
  void browse(const luciole::Location& archive);
  // Owned archive job change: opens an extracted member, reports a finished operation and syncs the archive
  // browser on the next frame.
  void changed(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // Removes the archive browser now.
  void close();
  // True while the archive browser overlay is shown.
  bool open() const { return m_view != nullptr; }
  // Keyboard hook in BrowserPage::keyboard(): handles every key while the archive browser is shown.
  bool key(const KeyboardEvent& event);
  // Layout hook in BrowserPage::prepareFrame().
  void layout(Renderer& renderer, LayoutRect bounds);

private:
  luciole::Location containingFolder(const luciole::Location& source) const;
  luciole::Location destination(const luciole::Location& source) const;
  void openNested(luciole::ArchiveMember member);
  void visit(std::size_t index);
  void openDialog(std::vector<luciole::Location> sources, std::vector<std::string> selected, bool compress);
  bool
  extract(const luciole::Location& source, const luciole::Location& destination, std::vector<std::string> selected);
  // The job the archive browser shows: the newest running archive job of the page, else its own.
  OperationJob* shown() const;
  void deferClose();
  void sync();
  void syncPassword();
  struct PasswordDialog {
    std::uint64_t job, request;
  };
  std::optional<PasswordDialog> m_passwordDialog;

  PageContext& m_page;
  // The archive browser's job: reads the catalog and opens members. Compress and extract jobs are separate.
  std::shared_ptr<OperationJob> m_browser;
  std::shared_ptr<OperationJob> m_rootBrowser;
  std::vector<std::shared_ptr<OperationJob>> m_retiredBrowsers;
  struct Frame {
    std::shared_ptr<OperationJob> job;
    luciole::Location source;
    std::optional<ArchiveView::Position> position;
  };
  std::vector<Frame> m_frames;
  std::optional<luciole::ArchiveMember> m_pendingMember;
  bool m_restorePosition = false;
  luciole::FileLauncher& m_launcher;
  luciole::FileIconCache& m_icons;
  WaylandConnection& m_connection;
  ArchiveView* m_view = nullptr;
  bool m_wanted = false;
  bool m_dirty = false;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
