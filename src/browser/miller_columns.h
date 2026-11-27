#pragma once

#include "browser/browser_controller.h"

namespace luciole {
  // Column controllers remain alive until the view has detached their adapters.
  class MillerColumns {
  public:
    struct Column {
      std::uint64_t id;
      std::unique_ptr<BrowserController> browser;
      Location location;
      std::optional<FileIdentity> identity;
      float width = 260;
      bool seedPath = false;
      bool explicitPath = false;
      std::uint64_t seedRevision = 0;
      std::optional<EntryId> pathEntry = std::nullopt;
      bool fixedWidth = false;
    };
    static constexpr std::size_t maxColumns = 16;
    float defaultWidth = 260;
    void start(BrowserController& source, bool showParents = true);
    void clear();
    void collectRetired();
    void synchronize();
    bool focus(std::size_t index);
    void load(std::size_t index);
    void interact(std::size_t index);
    bool enter();
    [[nodiscard]] bool enabled() const { return !m_columns.empty(); }
    [[nodiscard]] std::size_t focused() const { return m_focused; }
    [[nodiscard]] BrowserController& active() const { return *m_columns[m_focused]->browser; }
    [[nodiscard]] const std::vector<std::unique_ptr<Column>>& columns() const { return m_columns; }
    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }
    void setFileActivatedCallback(std::function<void(const Location&)> callback) { m_activate = std::move(callback); }
    void setMediaFactsCheck(DirectorySession::MediaFactsCheck check) { m_mediaFacts = std::move(check); }

  private:
    std::unique_ptr<Column> makeColumn(BrowserController& source, const Location& location, bool inheritHistory = true);
    void buildChain(std::unique_ptr<Column> current);
    void trim(std::size_t count);
    void notify() const;
    std::vector<std::unique_ptr<Column>> m_columns, m_retired;
    std::size_t m_focused = 0;
    std::uint64_t m_nextId = 0;
    bool m_showParents = true;
    std::function<void()> m_changed;
    std::function<void(const Location&)> m_activate;
    DirectorySession::MediaFactsCheck m_mediaFacts;
  };
} // namespace luciole
