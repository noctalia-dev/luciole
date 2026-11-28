#pragma once

#include "fs/location.h"

#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  // One change to the shared recent list.
  struct RecentEdit {
    enum class Kind : std::uint8_t { Use, Forget, Clear };
    Kind kind = Kind::Use;
    std::string uri;         // The file, for Use and Forget.
    std::string contentType; // Use only.
  };

  // Applies edits in order to recently-used.xbel text (nullopt when the file is missing). `now` stamps used files.
  // Text that is not a bookmark file is an error, so a list this cannot read is never overwritten; Clear replaces it
  // anyway, since emptying is what was asked. nullopt when nothing changes.
  std::expected<std::optional<std::string>, std::string>
  editRecentList(const std::optional<std::string>& current, const std::vector<RecentEdit>& edits, std::int64_t now);

  // The recently used files list desktop applications share ($XDG_DATA_HOME/recently-used.xbel). Edits are written
  // one batch at a time on a worker: read, edit, then replace the file atomically, as GTK's own writers do. Main loop
  // only.
  class RecentFiles {
  public:
    using Done = std::function<void(std::string error)>; // Empty when the edit was saved.
    explicit RecentFiles(std::filesystem::path file = defaultFile());
    ~RecentFiles();
    RecentFiles(const RecentFiles&) = delete;
    RecentFiles& operator=(const RecentFiles&) = delete;

    static std::filesystem::path defaultFile();
    // Records that Luciole opened or chose a local file; anything else is ignored. An empty type is guessed from the
    // name.
    void recordUse(const Location& file, std::string contentType);
    void forget(const std::vector<Location>& files, Done done);
    void clear(Done done);
    // A batch is waiting or being written.
    bool busy() const;

  private:
    struct State;
    void submit(std::vector<RecentEdit> edits, Done done);
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
