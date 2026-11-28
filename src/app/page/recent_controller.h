#pragma once

#include "fs/directory_entries.h"

#include <memory>
#include <string>
#include <vector>

class PageContext;
namespace luciole {
  class RecentFiles;
}

// Recent: removing items from the shared recent list, clearing it, and recording the files Luciole opens.
class RecentController {
public:
  RecentController(PageContext& page, luciole::RecentFiles& recent, bool remember);

  // Forgets the files `items` stand for; the files themselves stay.
  void remove(const std::vector<luciole::DirectoryEntry>& items);
  // Clears the whole list after the user confirms.
  void openClear();
  // Luciole opened or chose `file`; only local files are recorded, and nothing while remembering is off.
  void recordUse(const luciole::Location& file, std::string contentType = {});
  void setRemember(bool remember) { m_remember = remember; }
  bool busy() const;

private:
  void finished(const std::string& error);

  PageContext& m_page;
  luciole::RecentFiles& m_recent;
  bool m_remember = true;
  // Expires with this controller, so edits that finish later report nothing.
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
