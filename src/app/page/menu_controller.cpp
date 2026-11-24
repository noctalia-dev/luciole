#include "app/page/menu_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/create_controller.h"
#include "app/page/details_header_controller.h"
#include "app/page/location_bar_controller.h"
#include "app/page/page_context.h"
#include "app/page/program_controller.h"
#include "app/page/properties_controller.h"
#include "app/page/rename_controller.h"
#include "app/page/submenu_controller.h"
#include "app/page/transfer_controller.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "browser/path_completion.h"
#include "browser/places.h"
#include "clipboard/text_clipboard.h"
#include "config/keybinds.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/deferred_call.h"
#include "fs/archive_type.h"
#include "fs/entry_access.h"
#include "platform/desktop_integration.h"
#include "platform/terminal_launcher.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Action = luciole::BrowserAction;

namespace {
  // File menu entries that are not browser actions.
  enum FileMenuId : std::uint16_t {
    NewTab = 9000,
    OtherPane,
    NewWindow,
    CopyToOther,
    MoveToOther,
    ClosePane,
    PasteInto,
    OpenIn,
    CopyLocation,
  };

  constexpr float kShortSubmenuWidth = 200.0F;

  // Appends a separator unless `entries` is empty or already ends with one.
  void addSeparator(std::vector<ContextMenuControlEntry>& entries) {
    if (!entries.empty() && !entries.back().separator) {
      entries.push_back({.label = "", .separator = true});
    }
  }
} // namespace

MenuController::MenuController(
    PageContext& page, luciole::PinService& pins, luciole::ViewPreferenceWriter& preferences,
    TransferController& transferController, luciole::TerminalLauncher& terminal,
    luciole::PathCompletion& siblingCompletion, CreateController& creator, SubmenuController& submenus,
    RenameController& renamer, PropertiesController& properties, DetailsHeaderController& columns,
    LocationBarController& locationBar, ProgramController& programs
)
    : m_page(page), m_pins(pins), m_preferences(preferences), m_transferController(transferController),
      m_terminal(terminal), m_siblingCompletion(siblingCompletion), m_creator(creator), m_submenus(submenus),
      m_renamer(renamer), m_properties(properties), m_columns(columns), m_locationBar(locationBar),
      m_programs(programs) {}

ContextMenuControlEntry
MenuController::actionEntry(Action action, const luciole::ActionRequest& request, bool radio) const {
  const auto& definition = luciole::ActionRegistry::definition(action);
  const auto state = m_page.actionState(action, request);
  auto shortcut = luciole::actionShortcutLabel(action);
  return {
      .id = luciole::ActionRegistry::menuId(action),
      .label = std::string(
          action == Action::Undo       ? m_page.jobs().historyLabel(false)
              : action == Action::Redo ? m_page.jobs().historyLabel(true)
                                       : definition.label
      ),
      .enabled = state.enabled,
      .checkmark = !radio && state.checked != -1,
      .radio = radio,
      .toggleState = state.checked,
      .shortcut = shortcut.empty() ? std::nullopt : std::optional(std::move(shortcut)),
      .destructive = definition.destructive
  };
}

std::vector<ContextMenuControlEntry> MenuController::folderMenuEntries(const luciole::ActionRequest& request) const {
  std::vector<ContextMenuControlEntry> entries;
  const auto add = [&](Action action) { entries.push_back(actionEntry(action, request)); };
  if (request.folder == luciole::Location::trash()) {
    add(Action::EmptyTrash);
    addSeparator(entries);
  }
  if (m_page.hasOtherPane()) {
    auto shortcut = luciole::keybindShortcutLabel(luciole::KeybindCommand::ClosePane);
    entries.push_back(
        {.id = ClosePane,
         .label = "Close Pane",
         .shortcut = shortcut.empty() ? std::nullopt : std::optional(std::move(shortcut))}
    );
  }
  add(Action::Terminal);
  addSeparator(entries);
  add(Action::CreateFolder);
  add(Action::NewDocument);
  entries.back().hasSubmenu = true;
  add(Action::PasteFiles);
  add(Action::Undo);
  add(Action::Redo);
  addSeparator(entries);
  add(Action::SelectAll);
  add(Action::InvertSelection);
  add(Action::SelectPattern);
  addSeparator(entries);
  add(Action::SortMenu);
  add(Action::Refresh);
  if (request.folder && !request.folder->isTrash()) {
    add(m_pins.contains(*request.folder) ? Action::UnpinCurrent : Action::PinCurrent);
  }
  add(Action::ReloadPins);
  addSeparator(entries);
  entries.push_back({.id = CopyLocation, .label = "Copy Location", .hasSubmenu = true});
  add(Action::Scripts);
  entries.back().hasSubmenu = true;
  return entries;
}

std::vector<ContextMenuControlEntry> MenuController::itemMenuEntries(const luciole::ActionRequest& request) const {
  std::vector<ContextMenuControlEntry> entries;
  const auto add = [&](Action action) { entries.push_back(actionEntry(action, request)); };
  const auto& model = m_page.browser().model();
  const bool one = request.targets.size() == 1;
  const auto* single = one ? model.entry(request.targets.front().id) : nullptr;
  const auto singleLocation = single ? std::optional(single->location()) : std::nullopt;
  const bool singleFolder = single != nullptr && single->isFolderLink();
  const bool trash = request.targets.front().location.isTrash();

  add(Action::Open);
  add(Action::OpenWith);
  if (single
      && singleLocation->isNative()
      && !trash
      && (single->type == luciole::FileType::Regular || single->symlink)) {
    const auto execute = luciole::entryAccess(*single).execute;
    if (execute == true
        || (!execute && single->details.mode && (*single->details.mode & 0111U) != 0)
        || singleLocation->basename().value_or("").ends_with(".desktop")) {
      add(Action::RunProgram);
      if (!singleLocation->basename().value_or("").ends_with(".desktop")) {
        add(Action::RunInTerminal);
      }
    }
  }
  if (!trash && m_page.actionState(Action::Scripts, request).enabled) {
    add(Action::Scripts);
    entries.back().hasSubmenu = true;
  }
  if (one) {
    add(Action::QuickPreview);
  }
  if (singleFolder) {
    entries.push_back({.id = OpenIn, .label = "Open In", .hasSubmenu = true});
    if (!trash) {
      add(Action::TargetTerminal);
    }
  }
  addSeparator(entries);
  add(Action::CutFiles);
  add(Action::CopyFiles);
  if (singleFolder && !trash && m_page.clipboard().available() && m_page.clipboard().hasFiles()) {
    entries.push_back(
        {.id = PasteInto,
         .label = "Paste into Folder",
         .enabled = !m_page.operationsBusy()
             && luciole::transferLocationSupported(*singleLocation)
             && luciole::entryAccess(*single).write != false}
    );
  }
  if (m_page.hasOtherPane()) {
    entries.push_back({.id = CopyToOther, .label = "Copy to Other Pane", .enabled = !m_page.operationsBusy()});
    entries.push_back({.id = MoveToOther, .label = "Move to Other Pane", .enabled = !m_page.operationsBusy()});
  }
  entries.push_back({.id = CopyLocation, .label = "Copy Location", .hasSubmenu = true});
  addSeparator(entries);
  if (single != nullptr) {
    if (luciole::isSupportedArchive(*singleLocation, single->contentType.value_or(""))) {
      add(Action::BrowseArchive);
      add(Action::ExtractHere);
      add(Action::ExtractTo);
    }
    // Only useful when the item lives outside the shown folder (search results); otherwise it reopens this one.
    if (singleLocation->parent() != model.snapshot().location) {
      add(Action::ContainingFolder);
    }
    if (single->trashInfo().original) {
      add(Action::OriginalLocation);
    }
    if (single->symlink && !trash) {
      add(Action::ShowLinkTarget);
    }
  }
  addSeparator(entries);
  const bool folders = std::ranges::all_of(request.targets, [&model](const auto& target) {
    const auto* entry = model.entry(target.id);
    return entry->type == luciole::FileType::Directory && !entry->location().isTrash();
  });
  if (folders) {
    if (std::ranges::any_of(request.targets, [this](const auto& target) {
          return !m_pins.contains(target.location);
        })) {
      add(Action::Pin);
    }
    if (std::ranges::any_of(request.targets, [this](const auto& target) { return m_pins.contains(target.location); })) {
      add(Action::Unpin);
    }
  }
  addSeparator(entries);
  if (!trash) {
    add(Action::Rename);
    add(Action::CreateLink);
    add(Action::Compress);
    addSeparator(entries);
  }
  for (const auto action : luciole::itemMenuDeletionActions(trash, m_page.settings().showPermanentDeletion)) {
    add(action);
  }
  addSeparator(entries);
  add(Action::Properties);
  return entries;
}

void MenuController::openFileMenu(std::optional<luciole::EntryId> id, float x, float y) {
  if (m_page.choosing()) {
    return;
  }
  m_page.setActionMessage({});
  const auto request = m_page.actionRequest(id.has_value());
  if (id && request.targets.empty()) {
    return;
  }
  if (id) {
    const auto ids = m_page.browser().model().selectedIds();
    const std::weak_ptr<int> lifetime = m_lifetime;
    if (m_page.browser().whenMetadataReady(ids, [this, lifetime, request, ids, id, x, y] {
          if (!lifetime.expired()
              && m_page.requestCurrent(request, luciole::ActionScope::Targets)
              && m_page.browser().model().selectedIds() == ids) {
            openFileMenu(id, x, y);
          }
        })) {
      return;
    }
  }
  auto entries = id ? itemMenuEntries(request) : folderMenuEntries(request);
  std::vector<std::pair<std::size_t, int>> submenus;
  for (std::size_t row = 0; row < entries.size(); ++row) {
    if (entries[row].hasSubmenu) {
      submenus.emplace_back(row, entries[row].id);
    }
  }
  showContextMenu(
      std::move(entries),
      [this, request, x, y](int action) {
        if (action == luciole::ActionRegistry::menuId(Action::SortMenu)) {
          openSortMenu(LayoutRect{x, y, 1, 1}, false);
        } else if (action == ClosePane) {
          m_page.requestWorkspace(WorkspaceCommand::ClosePane, std::nullopt, std::nullopt);
        } else if (action == PasteInto) {
          if (m_page.requestCurrent(request, luciole::ActionScope::Targets) && !m_page.operationsBusy()) {
            m_transferController.pasteInto(request);
          }
        } else if (action == CopyToOther || action == MoveToOther) {
          if (m_page.requestCurrent(request, luciole::ActionScope::Targets)) {
            m_page.requestWorkspace(
                action == CopyToOther ? WorkspaceCommand::CopyOther : WorkspaceCommand::MoveOther, std::nullopt, request
            );
          }
        } else if (const auto browserAction = luciole::ActionRegistry::fromMenuId(action)) {
          m_page.dispatchAction(*browserAction, request);
        }
      },
      {x, y, 1, 1}
  );
  if (!m_menu) {
    return;
  }
  for (const auto& [row, entryId] : submenus) {
    if (entryId == luciole::ActionRegistry::menuId(Action::NewDocument)) {
      m_submenus.attach(
          row, entryId, [this, request] { m_creator.openDocumentMenu(request); },
          [this] { m_creator.documentMenuClosed(); }
      );
    } else if (entryId == luciole::ActionRegistry::menuId(Action::Scripts)) {
      m_submenus.attach(
          row, entryId, [this, request] { m_programs.scripts(request); }, [this] { m_programs.closeScripts(); }
      );
    } else if (entryId == OpenIn) {
      m_submenus.attach(row, entryId, [this, request] { showOpenInMenu(request); });
    } else if (entryId == CopyLocation) {
      m_submenus.attach(row, entryId, [this, request, targets = id.has_value()] {
        showCopyLocationMenu(request, targets);
      });
    }
  }
}

void MenuController::showOpenInMenu(const luciole::ActionRequest& request) {
  const bool singleWindow = m_page.settings().singleWindow;
  std::vector<ContextMenuControlEntry> entries{
      {.id = NewTab, .label = "New Tab"},
      // Without a second pane, the workspace splits the window to open one.
      {.id = OtherPane, .label = m_page.hasOtherPane() ? "Other Pane" : "New Pane"},
  };
  if (!singleWindow) {
    entries.push_back({.id = NewWindow, .label = "New Window"});
  }
  m_submenus.show(
      std::move(entries),
      [this, request](int id) {
        if (const auto action = luciole::ActionRegistry::fromMenuId(id)) {
          m_page.dispatchAction(*action, request);
          return;
        }
        if (!m_page.requestCurrent(request, luciole::ActionScope::Targets)) {
          return;
        }
        const auto* entry = m_page.browser().model().entry(request.targets.front().id);
        if (!entry || !entry->isFolderLink()) {
          return;
        }
        m_page.requestWorkspace(
            id == NewTab          ? WorkspaceCommand::OpenTab
                : id == NewWindow ? WorkspaceCommand::NewWindow
                                  : WorkspaceCommand::OpenOther,
            entry->navigationLocation(), std::nullopt
        );
      },
      kShortSubmenuWidth
  );
}

void MenuController::showCopyLocationMenu(const luciole::ActionRequest& request, bool targets) {
  auto path = actionEntry(targets ? Action::CopyTargetPath : Action::CopyPath, request);
  path.label = "Path";
  auto uri = actionEntry(targets ? Action::CopyTargetUri : Action::CopyUri, request);
  uri.label = "URI";
  m_submenus.show(
      {std::move(path), std::move(uri)},
      [this, request](int id) {
        if (const auto action = luciole::ActionRegistry::fromMenuId(id)) {
          m_page.dispatchAction(*action, request);
        }
      },
      kShortSubmenuWidth
  );
}

void MenuController::openSeparatorMenu(std::size_t before, float x, float y) {
  if (m_page.choosing()) {
    return;
  }
  // The gap above or below the one this separator is drawn in, among the rows Places shows.
  const auto neighbor = [this, before](bool up) -> std::optional<std::size_t> {
    const luciole::SeparatorGaps gaps(m_pins, m_page.settings().showNetwork);
    const auto gap = gaps.gap(before);
    const auto target = !gap ? std::nullopt : up ? gaps.previousShown(*gap) : gaps.nextShown(*gap);
    return target && gaps.free(*target) ? target : std::nullopt;
  };
  std::vector<ContextMenuControlEntry> entries{
      {.id = 2, .label = "Move Up", .enabled = m_pins.editable() && neighbor(true).has_value()},
      {.id = 3, .label = "Move Down", .enabled = m_pins.editable() && neighbor(false).has_value()},
      {.label = "", .separator = true},
      {.id = 1, .label = "Remove Separator", .enabled = m_pins.editable()},
  };
  showContextMenu(
      std::move(entries),
      [this, before, neighbor](int action) {
        if (!std::ranges::contains(m_pins.separators(), before)) {
          m_page.actionError("This separator is no longer available.");
          return;
        }
        if (action == 1) {
          m_pins.removeSeparator(before);
        } else if (action == 2 || action == 3) {
          if (const auto target = neighbor(action == 2)) {
            m_pins.moveSeparatorTo(before, *target);
          }
        }
      },
      {x, y, 1, 1}
  );
}

void MenuController::openPlaceMenu(const luciole::Bookmark& bookmark, float x, float y) {
  if (m_page.choosing()) {
    return;
  }
  std::vector<ContextMenuControlEntry> entries{{.id = 1, .label = "Open"}};
  if (!m_pins.contains(bookmark.location)) {
    showContextMenu(
        std::move(entries), [this, location = bookmark.location](int) { m_page.browser().open(location); }, {x, y, 1, 1}
    );
    return;
  }
  const auto adjacent = [this, location = bookmark.location](bool previous) -> std::optional<std::size_t> {
    const auto& pins = m_pins.bookmarks();
    const auto found = std::ranges::find(pins, location, &luciole::Bookmark::location);
    if (found == pins.end()) {
      return std::nullopt;
    }
    auto index = static_cast<std::size_t>(found - pins.begin());
    while (previous ? index > 0 : index + 1 < pins.size()) {
      index = previous ? index - 1 : index + 1;
      if (luciole::placeShown(pins[index], m_page.settings().showNetwork)) {
        return index;
      }
    }
    return std::nullopt;
  };
  // The gap under this place, above the next row Places shows, if a separator can go there.
  const auto below = [this, location = bookmark.location] -> std::optional<std::size_t> {
    const auto& pins = m_pins.bookmarks();
    const auto found = std::ranges::find(pins, location, &luciole::Bookmark::location);
    if (found == pins.end()) {
      return std::nullopt;
    }
    const luciole::SeparatorGaps gaps(m_pins, m_page.settings().showNetwork);
    const auto next = gaps.nextShown(static_cast<std::size_t>(found - pins.begin()));
    return next && gaps.free(*next) ? next : std::nullopt;
  };
  {
    entries.push_back({.label = "", .separator = true});
    if (!bookmark.location.isTrash() && bookmark.location != luciole::Location::network()) {
      entries.push_back({.id = 2, .label = "Unpin", .enabled = m_pins.editable()});
      entries.push_back({.id = 6, .label = "Rename Place…", .enabled = m_pins.editable()});
      entries.push_back({.id = 8, .label = "Change Icon…", .enabled = m_pins.editable()});
    }
    entries.push_back({.id = 3, .label = "Move Up", .enabled = m_pins.editable() && adjacent(true).has_value()});
    entries.push_back({.id = 4, .label = "Move Down", .enabled = m_pins.editable() && adjacent(false).has_value()});
    // A line under this row; the last row has nothing below it to separate.
    entries.push_back({.id = 10, .label = "Add Separator Below", .enabled = m_pins.editable() && below().has_value()});
  }
  entries.push_back({.label = "", .separator = true});
  if (bookmark.location == luciole::Location::trash()) {
    entries.push_back({.id = 7, .label = "Empty Trash…", .enabled = false, .destructive = true});
    entries.push_back({.label = "", .separator = true});
  }
  if (bookmark.location == luciole::Location::network()) {
    entries.push_back({.id = 9, .label = "Hide Network"});
  } else {
    entries.push_back({.id = 5, .label = "Properties"});
  }
  showContextMenu(
      entries,
      [this, bookmark, adjacent, below](int action) {
        if (!m_pins.contains(bookmark.location)) {
          m_page.actionError("This pin is no longer available.");
          return;
        }
        if (action == 1) {
          m_page.browser().open(bookmark.location);
        } else if (action == 2) {
          m_pins.remove(bookmark.location);
        } else if (action == 9 && bookmark.location == luciole::Location::network()) {
          m_preferences.submit({{luciole::BrowserPreference::ShowNetwork, false}});
        } else if (action == 7 && bookmark.location == luciole::Location::trash()) {
          m_page.dispatchAction(Action::EmptyTrash);
        } else if (action == 6) {
          m_renamer.openPlaceRename(bookmark);
        } else if (action == 8) {
          m_renamer.openPlaceIconPicker(bookmark);
        } else if (action == 3 || action == 4) {
          if (const auto index = adjacent(action == 3)) {
            m_pins.move(bookmark.location, *index);
          }
        } else if (action == 10) {
          if (const auto target = below()) {
            m_pins.addSeparator(*target);
          }
        } else if (action == 5) {
          luciole::DirectoryEntry entry{
              .place = bookmark.location,
              .name = bookmark.label,
              .type = luciole::FileType::Directory,
              .size = std::nullopt,
              .modifiedSeconds = std::nullopt
          };
          entry.setDisplayName(bookmark.label);
          m_properties.show({std::move(entry)}, true);
        }
      },
      {x, y, 1, 1}
  );
  if (bookmark.location == luciole::Location::trash() && m_menu) {
    const auto generation = m_page.activePopup().generation();
    const std::weak_ptr<int> lifetime = m_lifetime;
    m_trashAvailability.query([this, lifetime, generation, entries = std::move(entries)](auto count) mutable {
      if (lifetime.expired() || generation != m_page.activePopup().generation() || !m_menu || !m_page.popupSurface()) {
        return;
      }
      const auto empty = std::ranges::find(entries, 7, &ContextMenuControlEntry::id);
      if (empty != entries.end()) {
        empty->enabled =
            count && *count > 0 && m_page.actionState(Action::EmptyTrash, m_page.actionRequest(false)).enabled;
      }
      m_menu->setEntries(std::move(entries));
      m_page.popupSurface()->requestLayout();
    });
  }
}

void MenuController::openBreadcrumbMenu(const luciole::Location& target, LayoutRect anchor) {
  if (m_page.choosing()) {
    return;
  }
  const auto context = m_page.browser().contextSerial();
  auto targets = std::make_shared<std::vector<luciole::Location>>();
  std::vector<ContextMenuControlEntry> entries;
  for (const auto& crumb : luciole::locationBreadcrumbs(target)) {
    const auto index = targets->size();
    targets->push_back(crumb.location);
    entries.push_back({.id = static_cast<int>(index), .label = crumb.label});
  }
  entries.push_back({.label = "", .separator = true});
  const auto request = m_page.actionRequest(false);
  for (const auto action : {Action::CopyPath, Action::CopyUri, Action::Terminal}) {
    auto entry = actionEntry(action, request);
    entry.id = -1 - luciole::ActionRegistry::menuId(action);
    if (action == Action::Terminal) {
      entry.enabled = target.nativePath().has_value() && m_terminal.available();
    }
    entries.push_back(std::move(entry));
  }
  entries.push_back({.label = "", .separator = true});
  entries.push_back({.label = "Sibling Folders", .header = true});
  entries.push_back({.label = "Loading folders…", .enabled = false});
  const auto activate = [this, context, target, targets](int id) {
    if (context != m_page.browser().contextSerial()) {
      return;
    }
    if (id >= 0) {
      if (static_cast<std::size_t>(id) < targets->size()) {
        m_locationBar.openBreadcrumb((*targets)[static_cast<std::size_t>(id)]);
      }
      return;
    }
    const auto action = luciole::ActionRegistry::fromMenuId(-1 - id);
    if (!action) {
      return;
    }
    if (*action == Action::Terminal) {
      if (!target.nativePath() || !m_terminal.available()) {
        return;
      }
      const std::weak_ptr<int> lifetime = m_lifetime;
      m_terminal.open(target, [this, lifetime, context](std::string error) {
        if (!lifetime.expired() && context == m_page.browser().contextSerial() && !error.empty()) {
          m_page.actionError(std::move(error));
        }
      });
    } else if (m_page.clipboard().available()) {
      m_page.clipboard().setClipboardText(
          *action == Action::CopyUri ? target.uri() : luciole::locationEditText(target)
      );
    }
  };
  showContextMenu(entries, activate, anchor);
  const auto generation = m_page.activePopup().generation();
  const auto siblingParent = target.parent().value_or(target);
  m_siblingCompletion.children(
      siblingParent, m_page.browser().model().filter().showHidden,
      [this, context, generation, anchor, targets, entries = std::move(entries),
       activate](const luciole::PathSuggestions& suggestions) mutable {
        if (context != m_page.browser().contextSerial()
            || generation != m_page.activePopup().generation()
            || !m_page.popupSurface()) {
          return;
        }
        entries.pop_back();
        for (const auto& item : suggestions.items) {
          entries.push_back({.id = static_cast<int>(targets->size()), .label = item.label});
          targets->push_back(item.location);
        }
        if (suggestions.items.empty()) {
          entries.push_back(
              {.label = suggestions.error.empty() ? "No sibling folders" : "Folders unavailable", .enabled = false}
          );
        }
        if (suggestions.truncated) {
          entries.push_back({.label = "First 256 folders; open parent for all", .enabled = false});
        }
        showContextMenu(std::move(entries), activate, anchor);
      }
  );
}

void MenuController::revealHighlight() {
  if (!m_scroll || !m_menu) {
    return;
  }
  const auto index = m_menu->highlightedIndex();
  const float top = m_menu->rowTop(index), bottom = m_menu->rowBottom(index);
  const float offset = m_scroll->scrollOffset();
  if (top < offset) {
    m_scroll->requestScrollToOffset(top);
  } else if (bottom > offset + m_scroll->height()) {
    m_scroll->requestScrollToOffset(bottom - m_scroll->height());
  }
  m_page.popupSurface()->requestLayout();
}

void MenuController::key(const KeyboardEvent& event) {
  using K = luciole::KeybindCommand;
  const bool cancel = luciole::keybindMatches(K::Cancel, event.sym, event.modifiers);
  const bool focusNext = luciole::keybindMatches(K::FocusNext, event.sym, event.modifiers);
  const bool focusPrevious = luciole::keybindMatches(K::FocusPrevious, event.sym, event.modifiers);
  if (m_menu == nullptr) {
    if (event.pressed && cancel) {
      m_page.dismissPopup();
    } else if (event.pressed && (focusNext || focusPrevious)) {
      (void)m_page.popupDispatcher().cycleTabFocus(focusPrevious);
    } else {
      m_page.popupDispatcher().keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    }
    return;
  }
  if (m_submenus.key(event)) {
    return;
  }
  if (!event.pressed || event.preedit) {
    return;
  }
  if (cancel) {
    m_page.dismissPopup();
  } else if (luciole::keybindMatches(K::MoveUp, event.sym, event.modifiers)) {
    if (m_menu->moveHighlight(-1)) {
      revealHighlight();
      m_page.popupSurface()->requestRedraw();
    }
  } else if (luciole::keybindMatches(K::MoveDown, event.sym, event.modifiers)) {
    if (m_menu->moveHighlight(1)) {
      revealHighlight();
      m_page.popupSurface()->requestRedraw();
    }
  } else if (luciole::keybindMatches(K::MoveRight, event.sym, event.modifiers)) {
    m_submenus.openHighlighted();
  } else if (luciole::keybindMatches(K::Validate, event.sym, event.modifiers) && !m_submenus.openHighlighted()) {
    (void)m_menu->activateHighlighted();
  }
}

void MenuController::menuClosed() {
  m_trashAvailability.cancel();
  m_menu = nullptr;
  m_scroll = nullptr;
}

void MenuController::showContextMenu(
    std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor, bool alignLeft,
    float preferredWidth
) {
  m_columns.cancelGesture();
  m_page.cancelDrag();
  auto menu = std::make_unique<ContextMenuControl>();
  auto* control = menu.get();
  const bool compact = m_page.settings().compactMenus;
  const float contentHeight = ContextMenuControl::preferredHeight(entries, entries.size(), 1.0F, compact);
  const float height = std::min(contentHeight, m_page.popupMaxHeight());
  control->setCompact(compact);
  control->setMaxVisible(entries.size());
  const float width = std::min(preferredWidth, static_cast<float>(m_page.surface().width()) - 32.0F);
  control->setEntries(std::move(entries));
  const bool overflowing = contentHeight > height + 0.5F;
  const float gutter = overflowing ? Style::scrollbarWidth + Style::scrollbarGap : 0.0F;
  // The menu has an intrinsic width; reserve the scroller's lane explicitly.
  control->setMenuWidth(width - gutter);
  const auto activate = [this, action = std::move(action)](const ContextMenuControlEntry& entry) {
    const int id = entry.id;
    const std::weak_ptr<int> lifetime = m_lifetime;
    // The action runs before the close: clipboard actions need the menu's keyboard focus for their serial.
    DeferredCall::callLater([lifetime, action, id] {
      if (!lifetime.expired()) {
        action(id);
      }
    });
    m_page.dismissPopup();
  };
  control->setOnActivate(activate);
  ScrollView* scroll = nullptr;
  auto scrolling = ui::scrollView(
      {.out = &scroll,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .height = height,
       .flexGrow = 1.0F}
  );
  scrolling->setReserveScrollbarGutter(overflowing);
  scrolling->content()->addChild(std::move(menu));
  auto root = ui::column(
      {.align = FlexAlign::Stretch,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
       .styleRadius = Style::radiusLg},
      std::move(scrolling)
  );
  m_page.showPopup(std::move(root), control, anchor, width, height, alignLeft);
  m_scroll = scroll;
}

void MenuController::openColumnsMenu(const LayoutRect& anchor, std::optional<luciole::DetailsColumn> column) {
  std::vector<ContextMenuControlEntry> entries{
      {.label = "Name", .enabled = false, .checkmark = true, .toggleState = 1}
  };
  for (const auto candidate : m_columns.columnOrder()) {
    entries.push_back(
        {.id = static_cast<int>(candidate) + 1,
         .label = std::string(luciole::detailsColumnLabel(candidate)),
         .checkmark = true,
         .toggleState = luciole::detailsColumnVisible(m_columns.columnVisibility(), candidate) ? 1 : 0}
    );
  }
  entries.push_back({.label = "", .separator = true});
  entries.push_back(
      {.id = 100, .label = "Show All Columns", .enabled = m_columns.columnVisibility() != luciole::allDetailsColumns()}
  );
  entries.push_back(
      {.id = 106,
       .label = "Reset Column Visibility",
       .enabled = m_columns.columnVisibility() != luciole::DetailsColumnVisibility{}}
  );
  const int left = Style::rtl() ? 1 : -1;
  if (column) {
    entries.push_back({.label = "", .separator = true});
    entries.push_back(
        {.id = 101, .label = "Move Left", .enabled = m_columns.movedColumnOrder(*column, left).has_value()}
    );
    entries.push_back(
        {.id = 102, .label = "Move Right", .enabled = m_columns.movedColumnOrder(*column, -left).has_value()}
    );
  }
  entries.push_back({.label = "", .separator = true});
  entries.push_back(
      {.id = 103,
       .label = "Reset Column Order",
       .enabled = m_columns.columnOrder() != luciole::kDefaultDetailsColumnOrder}
  );
  const std::size_t widthIndex = column ? static_cast<std::size_t>(*column) + 1 : 0;
  entries.push_back(
      {.id = 104,
       .label = "Reset Column Width",
       .enabled = m_columns.columnWidths()[widthIndex] != luciole::kDefaultDetailsColumnWidths[widthIndex]}
  );
  entries.push_back(
      {.id = 105,
       .label = "Reset All Column Widths",
       .enabled = m_columns.columnWidths() != luciole::kDefaultDetailsColumnWidths}
  );
  showContextMenu(
      std::move(entries),
      [this, column, left, widthIndex](int id) {
        if (id == 104 || id == 105) {
          auto widths = id == 105 ? luciole::kDefaultDetailsColumnWidths : m_columns.columnWidths();
          widths[widthIndex] = luciole::kDefaultDetailsColumnWidths[widthIndex];
          m_columns.setDetailsColumnWidths(widths);
        } else if ((id == 101 || id == 102) && column) {
          if (const auto order = m_columns.movedColumnOrder(*column, id == 101 ? left : -left)) {
            m_columns.setDetailsColumnOrder(*order);
          }
        } else if (id == 103) {
          m_columns.setDetailsColumnOrder(luciole::kDefaultDetailsColumnOrder);
        } else {
          auto visibility = m_columns.columnVisibility();
          if (id >= 1 && id <= static_cast<int>(luciole::kDetailsColumnCount)) {
            auto& flag = luciole::detailsColumnFlag(visibility, static_cast<luciole::DetailsColumn>(id - 1));
            flag = !flag;
          } else if (id == 100) {
            visibility = luciole::allDetailsColumns();
          } else if (id == 106) {
            visibility = {};
          }
          m_columns.setDetailsColumnVisibility(visibility);
        }
        m_page.focusFileList();
      },
      anchor
  );
}

void MenuController::showActionMenu(
    std::initializer_list<std::optional<Action>> actions, bool radio, bool back, std::optional<LayoutRect> anchor,
    bool alignLeft
) {
  const auto request = m_page.actionRequest(false);
  std::vector<ContextMenuControlEntry> entries;
  if (back) {
    entries.push_back(actionEntry(Action::MenuBack, request));
    entries.push_back({.label = "", .separator = true});
  }
  for (const auto action : actions) {
    if (action) {
      entries.push_back(actionEntry(
          *action, request,
          radio
              && *action != Action::FoldersFirst
              && *action != Action::HiddenLast
              && *action != Action::ResetFolderView
      ));
    } else {
      entries.push_back({.label = "", .separator = true});
    }
  }
  showContextMenu(
      std::move(entries),
      [this, request](int id) {
        const auto action = luciole::ActionRegistry::fromMenuId(id);
        if (!action) {
          return;
        }
        // Closed first: Back reopens the main menu, and the check below must only see popups the action opened.
        m_page.closePopup();
        m_page.dispatchAction(*action, request);
        if (!m_page.popupSurface() && *action != Action::Search && *action != Action::EditLocation) {
          m_page.focusFileList();
        }
      },
      anchor.value_or(m_page.menuAnchor()), alignLeft
  );
}

void MenuController::openSortMenu(std::optional<LayoutRect> anchor, bool alignLeft) {
  if (m_page.browser().folderView()) {
    showActionMenu(
        {Action::SortName, Action::SortType, Action::SortSize, Action::SortModified, Action::SortAccessed,
         Action::SortCreated, Action::SortOwner, Action::SortGroup, Action::SortPermissions, Action::SortLocation,
         std::nullopt, Action::Ascending, Action::Descending, std::nullopt, Action::FoldersFirst, Action::HiddenLast,
         std::nullopt, Action::ResetFolderView},
        true, !anchor.has_value(), anchor, alignLeft
    );
    return;
  }
  showActionMenu(
      {Action::SortName, Action::SortType, Action::SortSize, Action::SortModified, Action::SortAccessed,
       Action::SortCreated, Action::SortOwner, Action::SortGroup, Action::SortPermissions, Action::SortLocation,
       std::nullopt, Action::Ascending, Action::Descending, std::nullopt, Action::FoldersFirst, Action::HiddenLast},
      true, !anchor.has_value(), anchor, alignLeft
  );
}

void MenuController::openSelectionMenu() {
  showActionMenu(
      {Action::SelectAll, Action::InvertSelection, Action::SelectPattern, Action::ClearSelection}, false, true
  );
}

void MenuController::openMenu() {
  if (m_page.choosing()) {
    return;
  }
  if (m_page.popupSurface() != nullptr) {
    m_page.dismissPopup();
    return;
  }
  std::vector<ContextMenuControlEntry> entries{
      {.id = 9000,
       .label = "New Tab",
       .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::NewTab)},
  };
  if (!m_page.settings().singleWindow) {
    entries.push_back(
        {.id = 9001,
         .label = "New Window",
         .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::NewWindow)}
    );
  }
  entries.push_back(
      {.id = 9002,
       .label = "Split View",
       .shortcut = luciole::keybindShortcutOptionalLabel(luciole::KeybindCommand::SplitView)}
  );
  entries.push_back({.label = "", .separator = true});
  const auto request = m_page.actionRequest(true);
  entries.push_back(actionEntry(Action::Undo, request));
  entries.push_back(actionEntry(Action::Redo, request));
  addSeparator(entries);
  entries.push_back(actionEntry(Action::Search, request));
  for (const auto action : {Action::Grid, Action::List, Action::Miller}) {
    entries.push_back(actionEntry(action, request, true));
  }
  if (m_page.browser().folderView()) {
    entries.push_back(actionEntry(Action::ResetFolderView, request));
  }
  entries.push_back({.label = "", .separator = true});
  entries.push_back(actionEntry(
      m_page.browser().model().snapshot().status == luciole::DirectoryStatus::Loading ? Action::Stop : Action::Refresh,
      request
  ));
  for (const auto action :
       {Action::SortMenu, Action::SelectionMenu, Action::ShowHidden, Action::ShowSidebar, Action::ShowPreview}) {
    entries.push_back(actionEntry(action, request));
  }
  entries.push_back({.label = "", .separator = true});
  entries.push_back(actionEntry(Action::Operations, request));
  entries.push_back(actionEntry(Action::Settings, request));
  entries.push_back(actionEntry(Action::KeyboardShortcuts, request));
  entries.push_back(actionEntry(Action::About, request));
  showContextMenu(
      std::move(entries),
      [this, request](int id) {
        if (id >= 9000) {
          m_page.requestWorkspace(
              id == 9000       ? WorkspaceCommand::NewTab
                  : id == 9001 ? WorkspaceCommand::NewWindow
                               : WorkspaceCommand::Split,
              std::nullopt, std::nullopt
          );
        } else if (const auto action = luciole::ActionRegistry::fromMenuId(id)) {
          m_page.dispatchAction(*action, request);
        }
      },
      m_page.menuAnchor(), true
  );
}
