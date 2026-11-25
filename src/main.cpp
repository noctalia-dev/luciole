#include "app/cli.h"
#include "core/application_identity.h"
#include "fs/posix_fd.h"

#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
  luciole::raiseFileDescriptorLimit();
  std::vector<std::string_view> args;
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }
  return luciole::runCli(args);
}
