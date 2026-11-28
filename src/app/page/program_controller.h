#pragma once

#include "browser/action_registry.h"
#include "fs/cancel_token.h"
#include "platform/program_launcher.h"

#include <cstddef>
#include <functional>
#include <memory>

class PageContext;
class SubmenuController;
class WaylandConnection;

class ProgramController {
public:
  ProgramController(PageContext& page, SubmenuController& submenus, WaylandConnection& connection);
  ~ProgramController();
  void open(const luciole::ActionRequest& request, std::function<void()> openDocument);
  void run(const luciole::ActionRequest& request, bool terminal);
  void scripts(const luciole::ActionRequest& request);
  void closeScripts();
  void cancel();
  bool busy() const { return m_pending != 0; }

private:
  void confirm(const luciole::ActionRequest& request, luciole::PreparedProgram program);
  void launch(const luciole::ActionRequest& request, luciole::PreparedProgram program, bool terminal, bool script);
  void showScripts(const luciole::ActionRequest& request, luciole::ScriptListing listing, bool loading);
  void openScriptsFolder(const luciole::ActionRequest& request);
  bool current(const luciole::ActionRequest& request) const;
  // The file a target names: a Recent item's own file, so it runs as from that file's folder.
  luciole::Location fileOf(const luciole::ActionTarget& target) const;
  PageContext& m_page;
  SubmenuController& m_submenus;
  WaylandConnection& m_connection;
  luciole::CancelToken m_listing, m_launch;
  std::size_t m_pending = 0;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
