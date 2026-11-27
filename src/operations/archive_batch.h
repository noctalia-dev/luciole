#pragma once

#include "fs/cancel_token.h"
#include "operations/archive_service.h"

#include <filesystem>
#include <glib.h>
#include <set>

namespace luciole {
  // Serial batch coordinator. The child retains the ordinary archive worker, password and publication contracts.
  struct ArchiveBatch : std::enable_shared_from_this<ArchiveBatch> {
    struct Task {
      Location source, destination;
      std::filesystem::path parent;
      std::string name, stamp;
      std::optional<FileError> error;
    };
    struct Plan {
      std::vector<Task> tasks;
      std::vector<std::filesystem::path> sources;
      std::set<std::filesystem::path> reserved;
    };
    ArchiveBatch(std::unique_ptr<ArchiveService> child, std::function<void()> callback);
    ~ArchiveBatch();
    void start(std::vector<Location> sources, const std::optional<Location>& parent);
    void cancel(bool shutdown = false);
    ArchivePhase phase() const;
    std::optional<Location> source() const;
    std::unique_ptr<ArchiveService> worker;
    std::function<void()> changed;
    std::vector<TransferResult> results;
    std::optional<FileError> error;
    Plan plan;
    std::size_t index = 0;
    bool running = true;

  private:
    void notify();
    void schedule();
    void advance();
    void childChanged();
    void record(TransferResult result);
    CancelToken m_cancel;
    bool m_preparing = true, m_active = false, m_stopping = false;
    std::set<std::filesystem::path> m_published;
    GSource* m_next = nullptr;
  };
} // namespace luciole
