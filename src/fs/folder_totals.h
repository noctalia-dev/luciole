#pragma once

#include "fs/directory_session.h"

#include <span>

namespace luciole {
  enum class FolderTotalsPhase : std::uint8_t { Idle, Running, Complete, Cancelled, Unsupported };
  struct FolderTotalsSnapshot {
    FolderTotalsPhase phase = FolderTotalsPhase::Idle;
    std::uint64_t bytes = 0, files = 0, folders = 0, links = 0, other = 0;
    std::uint64_t unavailable = 0, unsupported = 0, changed = 0;
    bool limited = false, overflow = false, cancelRequested = false;
    std::string error;
    bool operator==(const FolderTotalsSnapshot&) const = default;
    bool partial() const {
      return unavailable || unsupported || changed || limited || overflow || phase == FolderTotalsPhase::Cancelled;
    }
  };
  // Read-only native traversal started by the preview pane, the Properties dialog and the list view's bound rows.
  // One worker per instance, bounded progress delivery, no link following or content reads.
  // An injected network-filesystem check refuses the whole start before any traversal.
  // A new start supersedes a running traversal; the superseded result never publishes.
  class FolderTotalsService {
  public:
    FolderTotalsService();
    ~FolderTotalsService();
    FolderTotalsService(const FolderTotalsService&) = delete;
    FolderTotalsService& operator=(const FolderTotalsService&) = delete;
    using NetworkFilesystemCheck = std::function<bool(const std::string& path)>;
    bool start(std::span<const DirectoryEntry> entries);
    void cancel();
    void clear();
    bool busy() const;
    const FolderTotalsSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> changed);
    void setNetworkFilesystemCheck(NetworkFilesystemCheck check);
    void setSameDeviceOnly(bool enabled);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
