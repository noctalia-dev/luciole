#pragma once

#include "browser/path_completion.h"
#include "fs/location.h"
#include "render/scene/input_area.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct KeyboardEvent;
struct PointerEvent;
class Button;
class ContextMenuControl;
class FileKeyboardController;
class Flex;
class Input;
class MenuController;
class PageContext;
class Renderer;
class ScrollView;
class WaylandConnection;

// The location bar and search field: breadcrumbs with their overflow menu and double-click to edit, editing the
// location as text with folder completion, and the search filter.
class LocationBarController {
public:
  LocationBarController(
      PageContext& page, WaylandConnection& connection, FileKeyboardController& keys, MenuController& menus
  );

  // Scene: the breadcrumb and location field, the search field, and the completion dropdown on `toolbar`.
  std::unique_ptr<Flex> buildLocationArea();
  std::unique_ptr<Flex> buildSearchArea();
  void buildCompletion(Node& toolbar);

  // True while the location is edited as text.
  bool editing() const { return m_editingLocation; }
  // True while the search filter is open.
  bool filterOpen() const { return m_filterOpen; }
  // True while the location or search field has keyboard focus.
  bool textFocused() const;
  Input& filterInput() { return *m_filterInput; }
  bool hasLocationInput() const { return m_location != nullptr; }
  bool hasFilterInput() const { return m_filterInput != nullptr; }
  // Shown breadcrumbs, for drops and middle clicks.
  const std::vector<std::pair<luciole::Location, Button*>>& breadcrumbs() const { return m_dropBreadcrumbs; }
  // True while the breadcrumbs are shown and contain the scene point.
  bool breadcrumbsContain(float x, float y) const;

  void beginLocationEdit();
  void endLocationEdit(bool focusFiles);
  // Opens `target` from a breadcrumb and focuses the file list.
  void openBreadcrumb(const luciole::Location& target) { breadcrumbClick(target); }
  void openFilter(bool selectAll = true, bool preserveTextInput = false);
  // Switches the search field between searching subfolders and filtering the current folder.
  void toggleSearchScope();
  void clearCompletions();
  // Completes the edited location for a Tab press with `modifiers` that the window handles.
  bool completeOnTab(std::uint32_t modifiers);
  // Forgets a first click on the empty location bar.
  void cancelBlankClick() { m_blankLocationClick.reset(); }

  // Pointer hooks in BrowserPage::pointer() for the page surface.
  // Ends a pending double click on movement and closes completions on a press outside them.
  void pointer(const PointerEvent& event, float x, float y);
  // Starts editing on a second press on the current breadcrumb or empty bar, or a single press on either when enabled.
  bool press(const PointerEvent& event, float x, float y);
  // Keyboard hooks in BrowserPage::keyboard().
  // Any key press ends pending double clicks.
  void keyPressed();
  // The menu key on a breadcrumb opens its menu.
  bool menuKey(const KeyboardEvent& event);
  // Arrow, Tab, Enter and Escape keys in the completion list.
  bool completionKey(const KeyboardEvent& event);
  // Escape ends location editing or closes the search filter.
  bool escapeKey(const KeyboardEvent& event);
  // Down in the search field moves to the results.
  bool filterKey(const KeyboardEvent& event);

  // BrowserPage::syncBrowser(): resets the bar for a new location; true when `location` changed.
  bool syncLocation(const std::optional<luciole::Location>& location);
  // BrowserPage::syncBrowser(): field values and borders.
  void sync();
  // Frame slot in BrowserPage::prepareFrame(): starts editing after a double click; true when it did.
  bool onFrame();
  // Layout hooks: sizes the search field and rebuilds breadcrumbs before the page is arranged.
  void prepareLayout();
  // Fits the breadcrumbs into the bar; true when they moved.
  bool layoutBreadcrumbs(Renderer& renderer);
  void layoutCompletions(Renderer& renderer);

private:
  struct BreadcrumbPart {
    luciole::Location location;
    Button* button;
    Node* separator;
  };
  struct LocationBlankClick {
    luciole::Location location;
    std::uint64_t context;
    std::chrono::steady_clock::time_point time;
    float x, y;
  };

  void requestCompletions(const std::string& text);
  void acceptCompletion(std::size_t index, bool navigate);
  bool completionKey(std::uint32_t key, std::uint32_t modifiers);
  void breadcrumbClick(const luciole::Location& target, const InputArea::PointerData* pointer = nullptr);
  void openBreadcrumbOverflow();
  void rebuildBreadcrumbs();
  void changeNameFilter(const std::string& text);
  void closeFilter();
  void focusResults();

  PageContext& m_page;
  WaylandConnection& m_connection;
  FileKeyboardController& m_keys;
  MenuController& m_menus;
  Input* m_location = nullptr;
  Input* m_filterInput = nullptr;
  Button* m_scopeButton = nullptr;
  Flex* m_searchArea = nullptr;
  Flex* m_locationArea = nullptr;
  ScrollView* m_breadcrumbScroll = nullptr;
  Flex* m_breadcrumbs = nullptr;
  std::vector<BreadcrumbPart> m_breadcrumbParts;
  std::vector<std::pair<luciole::Location, Button*>> m_dropBreadcrumbs;
  Button* m_breadcrumbOverflow = nullptr;
  std::size_t m_firstBreadcrumb = 0;
  float m_breadcrumbLayoutWidth = -1;
  float m_breadcrumbRenderScale = 0;
  std::uint64_t m_breadcrumbMetricsGeneration = 0;
  std::optional<LocationBlankClick> m_blankLocationClick;
  std::optional<luciole::Location> m_lastBreadcrumb;
  std::optional<luciole::Location> m_breadcrumbEditTarget;
  std::chrono::steady_clock::time_point m_breadcrumbClickTime;
  float m_breadcrumbClickX = 0;
  float m_breadcrumbClickY = 0;
  bool m_breadcrumbsDirty = true;
  bool m_editingLocation = false;
  bool m_filterOpen = false;
  bool m_locationInvalid = false;
  std::string m_displayedLocation;
  luciole::PathCompletion m_pathCompletion;
  luciole::PathSuggestions m_completionItems;
  std::string m_completionText;
  std::uint64_t m_completionContext = 0;
  std::optional<std::size_t> m_completionIndex;
  bool m_completionDirty = false;
  ContextMenuControl* m_completionControl = nullptr;
  Flex* m_completionFrame = nullptr;
};
