#pragma once

#include "browser/directory_model.h"
#include "fs/cancel_token.h"
#include "fs/location_mount.h"
#include "fs/recursive_search.h"

#include <glib.h>

namespace luciole {
  class DirectoryTree;
  struct TreeRowInfo;
  struct BrowseViewport {
    ViewMode mode = ViewMode::Grid;
    int iconSize = kDefaultGridIconSize;
    float vertical = 0;
    float horizontal = 0;
    float withinRow = 0;
    std::optional<EntryId> first = std::nullopt;
  };

  class BrowserController {
  public:
    BrowserController(SortOptions sort, bool showHidden, bool showBackups = true);
    ~BrowserController();
    BrowserController(const BrowserController&) = delete;
    BrowserController& operator=(const BrowserController&) = delete;

    void setChangedCallback(std::function<void()> callback);
    void setFileActivatedCallback(std::function<void(const Location&)> callback);
    void open(const Location& location);
    bool openText(const std::string& text);
    void setCaptureViewport(std::function<BrowseViewport()> callback);
    void setViewDefaults(ViewMode mode, int iconSize);
    void setDefaultViewMode(ViewMode mode) {
      m_defaults.mode = mode;
      m_viewport.mode = mode;
      if (!m_history.empty()) {
        m_history[m_historyIndex].viewport.mode = mode;
      }
      if (m_restoredViewport) {
        m_restoredViewport->mode = mode;
      }
    }
    void setDefaultIconSize(int size) {
      m_defaults.iconSize = size;
      m_restoring = false;
      m_restoredViewport.reset();
    }
    const BrowseViewport& viewport() const { return m_viewport; }
    std::uint64_t visitSerial() const { return m_visitSerial; }
    [[nodiscard]] std::optional<BrowseViewport> takeRestoredViewport();
    void duplicateFrom(BrowserController& source);
    void home();
    bool back();
    bool forward();
    bool up();
    void refresh();
    MountInteraction* mountInteraction() const;
    void stop();
    void toggleHidden();
    void setShowBackups(bool showBackups);
    // Current-folder filtering projects the live listing; recursive search traverses descendants.
    void setSearchRecursive(bool recursive);
    bool searchRecursive() const { return m_searchRecursive; }
    // Programmatic/history changes apply immediately; recursive editor changes use the trailing debounce.
    void setSearchQuery(std::string text);
    void editSearchQuery(std::string text);
    void submitSearchQuery();
    void setSearchComposing(bool composing);
    bool searchPending() const { return m_searchPending; }
    void activate(EntryId id);
    // Returns true when the callback was deferred. Retired browsing contexts discard it.
    bool whenMetadataReady(std::vector<EntryId> ids, std::function<void()> callback, std::uint8_t details = 0);
    void setExpandableFolders(bool enabled);
    void releaseLocation(const Location& root);
    bool expandableFolders() const { return m_tree != nullptr; }
    bool toggleFolder(EntryId id);
    bool treeKey(bool expand);
    TreeRowInfo treeRow(EntryId id) const;

    std::uint64_t contextSerial() const { return m_contextSerial; }
    bool canBack() const;
    bool canForward() const;
    bool canUp() const;
    const std::string& message() const { return m_message; }
    const std::string& searchQuery() const { return m_query; }
    std::size_t skippedSearchFolders() const { return m_searchActive ? m_search.skippedFolders() : 0; }
    DirectoryModel& model() { return m_model; }
    const DirectoryModel& model() const { return m_model; }

  private:
    struct ActivationState;
    std::shared_ptr<ActivationState> m_activation;
    void advanceContext();
    void sessionChanged();
    void mountLocation(const Location& location, bool mountable = false);
    void activateMountable(const Location& location);
    std::unique_ptr<LocationMount> m_mount;
    bool m_mountAttempted = false;
    void notify() const;
    void publish(DirectorySnapshot snapshot);
    void showMessage(std::string message);
    void cancelSearchTimeout();
    void scheduleSearch();
    void startSearch();
    void publishPendingSearch();
    void applyQueryFilter();
    void updateMetadataPolicy();
    bool waitForVisitMetadata(std::function<void()> callback);
    bool m_updatingMetadata = false;
    bool m_restoreBulkMetadata = false;
    CancelToken m_duplicateToken;
    bool m_duplicateFactsPending = false;
    struct PendingMetadata {
      std::uint64_t context;
      std::vector<EntryId> ids;
      std::uint8_t details;
      std::function<void()> callback;
    };
    std::vector<PendingMetadata> m_pendingMetadata;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    DirectoryModel m_model;
    std::unique_ptr<DirectoryTree> m_tree;
    DirectorySession m_session;
    RecursiveSearch m_search;
    std::string m_query;
    GSource* m_searchTimeout = nullptr;
    bool m_searchPending = false;
    bool m_searchComposing = false;
    bool m_searchActive = false;
    bool m_searchRecursive = true;
    std::uint64_t m_revision = 0;
    std::uint64_t m_contextSerial = 0;
    struct Reference {
      std::string uri;
      FileIdentity identity;
    };
    struct Visit {
      Location location;
      std::string query;
      BrowseViewport viewport;
      std::vector<Reference> selected;
      std::optional<Reference> focus;
      std::optional<Reference> anchor;
      std::optional<Reference> first;
    };
    void saveVisit();
    void loadVisit();
    std::optional<Reference> reference(std::optional<EntryId> id) const;
    std::vector<Visit> m_history;
    BrowseViewport m_defaults;
    BrowseViewport m_viewport;
    std::optional<BrowseViewport> m_restoredViewport;
    std::function<BrowseViewport()> m_captureViewport;
    std::uint64_t m_visitSerial = 0;
    bool m_restoring = false;
    bool m_publishing = false;
    bool m_switchingSources = false;
    std::size_t m_historyIndex = 0;
    std::string m_message;
    std::function<void()> m_changed;
    std::function<void(const Location&)> m_fileActivated;
  };
} // namespace luciole
