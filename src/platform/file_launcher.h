#pragma once

#include "fs/file_icon_source.h"
#include "fs/location.h"

#include <functional>
#include <memory>
#include <vector>

using GAppInfo = struct _GAppInfo;

namespace luciole {
  struct ApplicationChoice {
    std::string id, name;
    FileIconSource icon;
    std::shared_ptr<GAppInfo> app;
    bool suggested = false, isDefault = false;
  };
  struct ApplicationList {
    std::vector<ApplicationChoice> choices;
    bool truncated = false;
  };
  class FileLauncher {
  public:
    FileLauncher();
    ~FileLauncher();
    FileLauncher(const FileLauncher&) = delete;
    FileLauncher& operator=(const FileLauncher&) = delete;
    using ActivationReply = std::function<void(std::string)>;
    using ActivationProvider = std::function<void(const std::string&, ActivationReply)>;
    void setActivationProvider(ActivationProvider provider);
    void setTerminalCommand(std::string command);
    void open(const Location& location);
    void open(const std::vector<Location>& locations);
    void applications(
        std::string mime, std::function<void(ApplicationList)> callback, bool includeOther = false,
        std::string search = {}
    );
    void applications(
        std::vector<std::string> types, std::function<void(ApplicationList)> callback, bool includeOther = false,
        std::string search = {}
    );
    // Explicit system MIME association change; does not launch a file.
    void setDefault(std::string mime, ApplicationChoice application, std::function<void(std::string)> callback);
    void cancelApplications();
    void openWith(const Location& location, const ApplicationChoice& application);
    void openWith(const std::vector<Location>& locations, const ApplicationChoice& application);
    void setChangedCallback(std::function<void(const std::string&)> callback);
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
