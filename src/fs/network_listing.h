#pragma once

#include "fs/directory_entries.h"
#include "fs/file_error.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace luciole {
  struct NetworkSnapshot {
    std::vector<DirectoryEntry> entries;
    bool loading = false;
    std::optional<FileError> warning;
  };
  // On-demand Network provider: native WS-Discovery plus optional GVfs DNS-SD.
  // Never enumerates GVfs network:/// or wsdd:///, and never opens advertised shares.
  class NetworkListing {
  public:
    NetworkListing();
    ~NetworkListing();
    NetworkListing(const NetworkListing&) = delete;
    NetworkListing& operator=(const NetworkListing&) = delete;
    void start();
    void refresh();
    void setChangedCallback(std::function<void()> callback);
    const NetworkSnapshot& snapshot() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
