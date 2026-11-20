#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/location.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  enum class IoLane : std::uint8_t;
  struct ProgramData;
  struct PreparedProgram {
    std::string name, command;
    bool desktop = false;
    std::shared_ptr<const ProgramData> data;
  };
  struct ScriptListing {
    std::vector<PreparedProgram> items;
    std::optional<FileError> error;
    bool truncated = false;
  };
  // Worker-only filesystem inspection and launching. Preparation never executes anything.
  PreparedProgram prepareProgram(const Location& location, const CancelToken& token);
  void launchProgram(
      const PreparedProgram& program, const Location& workingDirectory, const std::vector<Location>& arguments,
      bool terminal, const std::string& activationToken, const CancelToken& token,
      const std::string& terminalCommand = {}
  );
  Location scriptsDirectory();
  IoLane scriptsLane();
  ScriptListing listScripts(const CancelToken& token);
  Location ensureScriptsDirectory(const CancelToken& token);
} // namespace luciole
