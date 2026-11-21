#pragma once

#include "config/folder_view_store.h"
#include "fs/location.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace luciole {
  // The per-folder sort and view mode every page reads, loaded and saved on a serial background worker. Changes
  // apply in memory at once; until the first load finishes every folder reads as having none.
  class FolderViewService {
  public:
    explicit FolderViewService(std::filesystem::path path, bool enabled = false);
    ~FolderViewService();
    FolderViewService(const FolderViewService&) = delete;
    FolderViewService& operator=(const FolderViewService&) = delete;

    void setEnabled(bool enabled);
    bool enabled() const;
    // nullopt while per-folder views are off; otherwise the folder's own settings, empty when it has none.
    std::optional<FolderView> lookup(const Location& location) const;
    void setSort(const Location& location, SortOptions sort);
    void setMode(const Location& location, ViewMode mode);
    void reset(const Location& location);
    bool busy() const;
    // The last load or save problem, or empty.
    const std::string& error() const;
    // Runs after a load, an enable change or an edit, so pages can apply the current folder's settings again.
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
