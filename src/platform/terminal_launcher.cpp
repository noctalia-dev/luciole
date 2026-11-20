#include "platform/terminal_launcher.h"

#include "fs/network_info.h"
#include "fs/network_job.h"
#include "platform/terminal_command.h"

struct luciole::TerminalLauncher::State {
  CancelToken token;
  std::string command;
  bool initialized = false, available = false;
};
luciole::TerminalLauncher::TerminalLauncher() : m_state(std::make_shared<State>()) { setCommand({}); }
luciole::TerminalLauncher::~TerminalLauncher() { m_state->token.cancel(); }
void luciole::TerminalLauncher::setCommand(std::string command) {
  if (m_state->initialized && m_state->command == command) {
    return;
  }
  m_state->command = std::move(command);
  m_state->initialized = true;
  try {
    m_state->available = !terminalArguments(m_state->command, false).empty();
  } catch (const FileErrorException&) {
    m_state->available = false;
  }
}
bool luciole::TerminalLauncher::available() const { return m_state->available; }
void luciole::TerminalLauncher::open(const Location& folder, std::function<void(std::string)> completed) {
  const auto token = m_state->token;
  const auto lane = operationLane(folder);
  runIoJob<bool>(
      lane, token,
      [folder, token, lane, command = m_state->command, available = m_state->available] {
        if (!available) {
          throwFileError(FileErrorKind::Unavailable, "No terminal is available. Check Settings > General > Terminal.");
        }
        if (!folder.isNative()) {
          throwFileError(FileErrorKind::Unsupported, "Open Terminal Here requires a native folder.");
        }
        auto request = filesystemRequest("program", folder);
        request["method"] = "terminal";
        request["command"] = command;
        (void)NetworkProcess::acquire(lane)->call(std::move(request), token);
        return true;
      },
      [token, completed = std::move(completed)](JobResult<bool> result) {
        if (!token.cancelled()) {
          completed(result ? std::string{} : result.error().message);
        }
      }
  );
}
