#pragma once

#include "browser/chooser_policy.h"
#include "fs/cancel_token.h"
#include "fs/file_filter.h"
#include "platform/file_chooser_portal.h"
#include "ui/controls/select_dropdown_popup.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class BrowserPage;
class Button;
class Input;
class InputDispatcher;
class Label;
class Node;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;

namespace luciole {
  class WindowSurface;
}

// One FileChooser portal request shown in a chooser window: the page's chooser hooks, its filters and the footer
// with the application's choices, the name to save under and the Cancel and accept buttons. The call is answered
// exactly once, by accepting, cancelling or closing the window. Saving only chooses destinations: nothing is
// written, and an existing file is replaced only after a second confirmation.
class FileChooserSession {
public:
  FileChooserSession(
      luciole::FileChooserRequest request, std::shared_ptr<luciole::FileChooserCall> call,
      WaylandConnection& connection, RenderContext& renderer
  );
  ~FileChooserSession();
  FileChooserSession(const FileChooserSession&) = delete;
  FileChooserSession& operator=(const FileChooserSession&) = delete;

  const luciole::FileChooserRequest& request() const { return m_request; }
  std::string windowTitle() const;
  // Installs the page hooks and filters and returns the footer; `dispatcher` is the window's input, `close` closes
  // the window.
  std::unique_ptr<Node>
  attach(BrowserPage& page, luciole::WindowSurface& surface, InputDispatcher& dispatcher, std::function<void()> close);
  // Once per frame: the accept button's state and the reason it is unavailable.
  void sync();
  // True when the event belonged to the open filter or choice dropdown, or to the focused name field.
  bool pointer(const PointerEvent& event);
  bool keyboard(const KeyboardEvent& event);
  // The window closed without an answer: the request is cancelled.
  void windowClosed();

private:
  bool saving() const { return m_request.mode != luciole::FileChooserMode::Open; }
  luciole::ChooserMode mode() const;
  luciole::ChooserDecision decision(const std::vector<luciole::DirectoryEntry>& entries);
  void selectFilter(std::size_t index);
  void apply(const luciole::ChooserDecision& decision);
  void open(const std::vector<luciole::DirectoryEntry>& targets);
  // Save: answers with the destinations, or asks first when one exists.
  void save();
  void syncSave();
  void savedTargets(std::vector<luciole::Location> targets, std::vector<luciole::Location> existing);
  luciole::CancelToken m_typeToken, m_saveToken;
  std::vector<luciole::DirectoryEntry> m_typeSources, m_typeResolved;
  std::optional<luciole::Location> m_typeFolder;
  std::string m_typeError, m_saveError;
  struct PendingOpen {
    std::vector<luciole::DirectoryEntry> targets;
    std::vector<luciole::EntryId> selection;
  };
  std::optional<PendingOpen> m_openAfterTypes;
  std::uint64_t m_typeGeneration = 0;
  bool m_typesPending = false, m_savePending = false;
  void cancel();

  luciole::FileChooserRequest m_request;
  std::shared_ptr<luciole::FileChooserCall> m_call;
  WaylandConnection& m_connection;
  SelectDropdownPopup m_dropdown;
  wl_output* m_dropdownOutput = nullptr;
  BrowserPage* m_page = nullptr;
  luciole::WindowSurface* m_surface = nullptr;
  std::function<void()> m_close;
  InputDispatcher* m_dispatcher = nullptr;
  std::vector<luciole::FileChooserFilter> m_filters;
  std::size_t m_activeFilter = 0;
  std::optional<luciole::FileFilter> m_filter;
  // Choice ID and current value, in request order.
  std::vector<std::pair<std::string, std::string>> m_choices;
  Button* m_accept = nullptr;
  std::string m_acceptText;
  Label* m_reason = nullptr;
  // Save mode's name field; null otherwise.
  Input* m_name = nullptr;
  bool m_nameFocused = false;
  // The selected file whose name was last copied into the name field.
  std::optional<luciole::EntryId> m_namedFrom;
  std::optional<luciole::Location> m_folder;
  // Destinations the user was asked about; saving to exactly these again replaces them.
  std::vector<luciole::Location> m_confirm;
  std::string m_reasonText;
};
