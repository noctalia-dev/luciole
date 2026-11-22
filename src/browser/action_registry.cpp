#include "browser/action_registry.h"

#include "fs/archive_type.h"
#include "fs/entry_access.h"
#include "fs/transfer_types.h"

#include <algorithm>
#include <array>

namespace {
  using A = luciole::BrowserAction;
  using S = luciole::ActionScope;
  using C = luciole::ActionContext;
  using R = luciole::ActionRequest;
  using State = luciole::ActionState;
  using luciole::DirectoryEntry;
  using luciole::FileType;

  State unavailable(std::string_view reason) { return {.enabled = false, .reason = reason}; }
  State require(bool ok, std::string_view reason) { return ok ? State{} : unavailable(reason); }
  State checked(bool on) { return {.enabled = true, .reason = {}, .checked = on ? 1 : 0}; }

  bool single(const R& r) { return r.targets.size() == 1; }
  // Rules run after ActionRegistry::current() verified every target still resolves.
  template <class Predicate> bool all(const C& c, const R& r, Predicate predicate) {
    return std::ranges::all_of(r.targets, [&](const auto& target) { return predicate(*c.model.entry(target.id)); });
  }
  bool ready(const C& c) {
    const auto status = c.model.snapshot().status;
    return status == luciole::DirectoryStatus::Ready || status == luciole::DirectoryStatus::Loading;
  }
  bool native(const R& r) { return r.folder && r.folder->nativePath().has_value(); }
  State writable(const C& c) {
    if (c.operationsBusy) {
      return unavailable("Another file operation is still finishing.");
    }
    if (c.model.snapshot().metadata.filesystem.readOnly == true) {
      return unavailable("The filesystem is read-only.");
    }
    return {};
  }
  State openFolder(const C& c, const R&) {
    return require(c.model.snapshot().location.has_value(), "Open a folder first.");
  }
  State resetFolderView(const C& c, const R& r) {
    if (!c.folderViews) {
      return unavailable("Per-folder views are off in Settings.");
    }
    if (!r.folder) {
      return unavailable("Open a folder first.");
    }
    return require(c.folderViewCustomized, "This folder already uses the default view.");
  }
  State visibleRows(const C& c, const R&) { return require(!c.model.rows().empty(), "There are no visible items."); }
  State clipboard(const C& c, const R&) {
    return require(c.clipboardAvailable, "The application clipboard is unavailable.");
  }
  State folderClipboard(const C& c, const R& r) {
    if (!r.folder) {
      return unavailable("Open a folder first.");
    }
    return clipboard(c, r);
  }
  State trashItems(const C& c, const R& r, bool restore);
  State fileClipboard(const C& c, const R& r, bool cut) {
    if (!c.clipboardAvailable) {
      return unavailable("The application clipboard is unavailable.");
    }
    if (cut && r.folder && r.folder->isTrash()) {
      return trashItems(c, r, false);
    }
    if (!all(c, r, [&](const DirectoryEntry& entry) {
          const auto location = entry.location();
          return transferLocationSupported(location)
              && ((entry.symlink && location.isNative()) || entry.access.read.value_or(true))
              && (entry.type == FileType::Directory || entry.type == FileType::Regular || entry.symlink)
              && (!cut || (entry.access.rename != false && entry.access.remove != false));
        })) {
      return unavailable("Select accessible local or supported remote files or folders outside Trash.");
    }
    return {};
  }
  State createItem(const C& c, const R& r) {
    if (!native(r) || !c.query.empty() || !ready(c)) {
      return unavailable("Open an available native folder first.");
    }
    if (c.model.snapshot().metadata.access.write == false) {
      return unavailable("Writing to this folder is not permitted.");
    }
    return writable(c);
  }
  State paste(const C& c, const R& r) {
    if (!c.clipboardAvailable || (!c.clipboardHasFiles && !c.clipboardHasImage)) {
      return unavailable("The clipboard has no files or image to paste.");
    }
    if (!r.folder || !transferLocationSupported(*r.folder) || !c.query.empty() || !ready(c)) {
      return unavailable("Open an available local or supported remote folder first.");
    }
    if (c.model.snapshot().metadata.access.write == false || c.model.snapshot().metadata.filesystem.readOnly == true) {
      return unavailable("Writing to this folder is not permitted.");
    }
    return writable(c);
  }
  State renameOrTrash(const C& c, const R& r, bool rename) {
    if (const auto state = writable(c); !state.enabled) {
      return state;
    }
    if (rename && !single(r)) {
      if (r.targets.size() > 4096) {
        return unavailable("Select at most 4096 items for batch rename.");
      }
      const auto parent = r.targets.front().location.parent();
      if (!parent || !all(c, r, [&](const DirectoryEntry& entry) {
            const auto other = entry.location().parent();
            return entry.identity().has_value() && other && *parent == *other;
          })) {
        return unavailable("Batch rename requires identified items from one native folder.");
      }
    }
    if (!all(c, r, [](const DirectoryEntry& entry) { return entry.location().nativePath().has_value(); })) {
      return unavailable("This operation currently supports native files only.");
    }
    if (!rename && (!ready(c) || r.targets.size() > 4096)) {
      return unavailable("Select at most 4096 native items from an available listing.");
    }
    if (!all(c, r, [&](const DirectoryEntry& entry) {
          return (rename ? entry.access.rename : entry.access.trash).value_or(true);
        })) {
      return unavailable("The provider does not permit this operation on every selected item.");
    }
    return {};
  }
  State emptyTrash(const C& c, const R&) {
    if (!c.canEmptyTrash || c.operationsBusy) {
      return unavailable("Trash is unavailable or another operation is still finishing.");
    }
    const auto& snapshot = c.model.snapshot();
    // Search results and other folders cannot establish whether the whole Trash is empty.
    if (snapshot.location == luciole::Location::trash() && c.query.empty()) {
      if (snapshot.status == luciole::DirectoryStatus::Loading) {
        return unavailable("Wait until Trash has finished loading.");
      }
      if (snapshot.status != luciole::DirectoryStatus::Ready) {
        return unavailable("Trash is unavailable.");
      }
      return require(!snapshot.entries.empty(), "Trash is already empty.");
    }
    return {};
  }
  State trashItems(const C& c, const R& r, bool restore) {
    if (const auto state = writable(c); !state.enabled) {
      return state;
    }
    if (r.folder != luciole::Location::trash() || !c.query.empty() || !ready(c)) {
      return unavailable("Select items directly in Trash first.");
    }
    if (!all(c, r, [](const DirectoryEntry& entry) {
          const auto location = entry.location();
          const auto parent = location.parent();
          return location.isTrash()
              && parent
              && *parent == luciole::Location::trash()
              && entry.identity().has_value()
              && entry.access.remove.value_or(true);
        })) {
      return unavailable("These items cannot be changed by the Trash provider.");
    }
    if (restore && !all(c, r, [](const DirectoryEntry& entry) {
          return entry.trashInfo().original && entry.trashInfo().original->nativePath().has_value();
        })) {
      return unavailable("The original location is unavailable for one or more selected items.");
    }
    return {};
  }
  // One readable native regular archive: what Browse Archive and Extract accept, regardless of running jobs.
  bool browsableArchive(const C& c, const R& r) {
    return single(r) && all(c, r, [](const DirectoryEntry& entry) {
             return entry.type == FileType::Regular
                 && !entry.symlink
                 && entry.location().nativePath().has_value()
                 && entry.access.read.value_or(true)
                 && luciole::isSupportedArchive(entry.location(), entry.contentType.value_or(""));
           });
  }
  State archive(const C& c, const R& r, bool compress) {
    if (c.operationsBusy) {
      return unavailable("Another file operation is still finishing.");
    }
    if (!all(c, r, [](const DirectoryEntry& entry) {
          return entry.location().nativePath().has_value() && entry.access.read.value_or(true);
        })) {
      return unavailable("Archives require readable native items.");
    }
    if (compress) {
      if (!all(c, r, [](const DirectoryEntry& entry) {
            return entry.type == FileType::Regular || entry.type == FileType::Directory || entry.symlink;
          })) {
        return unavailable("Special files cannot be compressed.");
      }
    } else if (!browsableArchive(c, r)) {
      return unavailable("Select one ZIP or TAR archive.");
    }
    return {};
  }
  State extract(const C& c, const R& r) { return archive(c, r, false); }
  State open(const C& c, const R& r) {
    if (!single(r) && !all(c, r, [](const DirectoryEntry& entry) { return entry.type == FileType::Regular; })) {
      return unavailable("Open folders individually.");
    }
    if (!all(c, r, [](const DirectoryEntry& entry) {
          return !entry.location().isTrash() || (entry.type == FileType::Directory && !entry.symlink);
        })) {
      return unavailable("Restore this item before opening it.");
    }
    if (!all(c, r, [](const DirectoryEntry& entry) { return entry.access.read.value_or(true); })) {
      return unavailable("Reading this item is not permitted.");
    }
    return {};
  }
  State runProgram(const C& c, const R& r) {
    if (!single(r) || c.operationsBusy) {
      return unavailable("Select one program and wait for file operations to finish.");
    }
    const auto* entry = c.model.entry(r.targets.front().id);
    const auto location = entry->location();
    if (!location.isNative() || location.isTrash() || (entry->type != FileType::Regular && !entry->symlink)) {
      return unavailable("Run requires a native program outside Trash.");
    }
    return require(
        location.basename().value_or("").ends_with(".desktop") || entry->access.execute != false,
        "Allow executing this file in Properties first."
    );
  }
  State scripts(const C& c, const R& r) {
    if (!r.folder || !r.folder->isNative() || r.folder->isTrash() || c.operationsBusy) {
      return unavailable("Scripts require a native folder outside Trash.");
    }
    return require(
        std::ranges::all_of(
            r.targets, [](const auto& target) { return target.location.isNative() && !target.location.isTrash(); }
        ),
        "Scripts currently accept native files outside Trash."
    );
  }
  State containing(const C&, const R& r) { return require(single(r), "Select exactly one item."); }
  State linkTarget(const C& c, const R& r) {
    return require(
        single(r) && c.model.entry(r.targets.front().id)->symlink && !r.targets.front().location.isTrash(),
        "Select one link."
    );
  }
  State originalLocation(const C& c, const R& r) {
    if (!single(r)) {
      return unavailable("Select exactly one item.");
    }
    return require(
        c.model.entry(r.targets.front().id)->trashInfo().original.has_value(), "The original location is unavailable."
    );
  }
  State pin(const C& c, const R& r, bool unpin) {
    if (!all(c, r, [](const DirectoryEntry& entry) {
          return entry.type == FileType::Directory && !entry.location().isTrash();
        })) {
      return unavailable("Only folders outside Trash can be pinned.");
    }
    if (!c.pinsEditable) {
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    }
    const bool any = c.pinned
        && std::ranges::any_of(r.targets, [&](const auto& target) { return c.pinned(target.location) == unpin; });
    return require(any, unpin ? "These folders are not pinned." : "These folders are already pinned.");
  }
  State pinCurrent(const C& c, const R& r, bool unpin) {
    if (!r.folder || r.folder->isTrash()) {
      return unavailable("This location cannot be pinned.");
    }
    if (!c.pinsEditable) {
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    }
    return require(c.pinned && c.pinned(*r.folder) == unpin, "The pin state changed.");
  }
  State terminal(const C& c, const R& r) {
    if (!c.terminalAvailable) {
      return unavailable("No terminal is available. Check Settings > General > Terminal.");
    }
    if (!native(r) || r.folder->isTrash()) {
      return unavailable("Terminals require a native folder.");
    }
    return require(c.model.snapshot().metadata.access.execute != false, "Access to this folder is not permitted.");
  }
  State targetTerminal(const C& c, const R& r) {
    if (!c.terminalAvailable) {
      return unavailable("No terminal is available. Check Settings > General > Terminal.");
    }
    return require(
        single(r)
            && all(
                c, r,
                [](const DirectoryEntry& entry) {
                  return entry.type == FileType::Directory
                      && entry.location().nativePath().has_value()
                      && entry.access.execute.value_or(true);
                }
            ),
        "Select one accessible native folder."
    );
  }
  State createLink(const C& c, const R& r) {
    if (c.operationsBusy) {
      return unavailable("Another file operation is still finishing.");
    }
    return require(
        single(r)
            && all(
                c, r,
                [](const DirectoryEntry& entry) {
                  const auto location = entry.location();
                  return location.nativePath().has_value()
                      && !location.isTrash()
                      && entry.identity().has_value()
                      && (entry.type == FileType::Regular
                          || entry.type == FileType::Directory
                          || entry.type == FileType::Symlink);
                }
            ),
        "Select one native file, folder or symbolic link."
    );
  }
  State openWith(const C& c, const R& r) {
    return require(
        !r.targets.empty()
            && all(
                c, r,
                [](const DirectoryEntry& entry) {
                  return entry.type == FileType::Regular && !entry.symlink && entry.access.read != false;
                }
            ),
        "Select readable files."
    );
  }
  bool sortField(const C& c, luciole::SortField field) { return c.model.sort().field == field; }

  constexpr auto kActions = std::to_array<luciole::ActionDefinition>({
      {.action = A::Back,
       .label = "Back",
       .shortcut = "Alt+Left",
       .rule = [](const C& c, const R&) { return require(c.canBack, "There is no previous folder."); }},
      {.action = A::Forward,
       .label = "Forward",
       .shortcut = "Alt+Right",
       .rule = [](const C& c, const R&) { return require(c.canForward, "There is no next folder."); }},
      {.action = A::Up,
       .label = "Parent Folder",
       .shortcut = "Alt+Up",
       .rule = [](const C& c, const R&) { return require(c.canUp, "This location has no parent folder."); }},
      {.action = A::Home, .label = "Home", .shortcut = "Alt+Home"},
      {.action = A::Root, .label = "File System", .shortcut = ""},
      {.action = A::Refresh, .label = "Refresh", .shortcut = "F5", .rule = openFolder},
      {.action = A::Stop,
       .label = "Stop Loading",
       .shortcut = "",
       .rule =
           [](const C& c, const R&) {
             return require(c.model.snapshot().status == luciole::DirectoryStatus::Loading, "Nothing is loading.");
           }},
      {.action = A::EditLocation, .label = "Edit Location", .shortcut = "Ctrl+L"},
      {.action = A::Search, .label = "Search Files", .shortcut = "Ctrl+F", .rule = openFolder},
      {.action = A::Grid,
       .label = "Grid View",
       .shortcut = "Ctrl+1",
       .rule = [](const C& c, const R&) { return checked(c.viewMode == luciole::ViewMode::Grid); }},
      {.action = A::List,
       .label = "List View",
       .shortcut = "Ctrl+2",
       .rule = [](const C& c, const R&) { return checked(c.viewMode == luciole::ViewMode::List); }},
      {.action = A::Miller,
       .label = "Column View",
       .shortcut = "Ctrl+3",
       .rule = [](const C& c, const R&) { return checked(c.viewMode == luciole::ViewMode::Miller); }},
      {.action = A::ShowHidden,
       .label = "Show Hidden Files",
       .shortcut = "Ctrl+H",
       .rule = [](const C& c, const R&) { return checked(c.model.filter().showHidden); }},
      {.action = A::ShowSidebar,
       .label = "Show Sidebar",
       .shortcut = "F9",
       .rule = [](const C& c, const R&) { return checked(c.showSidebar); }},
      {.action = A::ShowPreview,
       .label = "Show Preview",
       .shortcut = "F8",
       .rule = [](const C& c, const R&) { return checked(c.showPreview); }},
      {.action = A::QuickPreview,
       .label = "Preview",
       .shortcut = "Space",
       .scope = S::Targets,
       .rule = [](const C&, const R& r) { return require(single(r), "Select one item to preview."); }},
      {.action = A::OpenWith, .label = "Open With…", .shortcut = "", .scope = S::Targets, .rule = openWith},
      {.action = A::SortName,
       .label = "Name",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Name)); }},
      {.action = A::SortType,
       .label = "Type",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Type)); }},
      {.action = A::SortSize,
       .label = "Size",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Size)); }},
      {.action = A::SortModified,
       .label = "Modified",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Modified)); }},
      {.action = A::SortAccessed,
       .label = "Accessed",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Accessed)); }},
      {.action = A::SortCreated,
       .label = "Created",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Created)); }},
      {.action = A::SortOwner,
       .label = "Owner",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Owner)); }},
      {.action = A::SortGroup,
       .label = "Group",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Group)); }},
      {.action = A::SortPermissions,
       .label = "Permissions",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Permissions)); }},
      {.action = A::SortLocation,
       .label = "Location",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(sortField(c, luciole::SortField::Location)); }},
      {.action = A::Ascending,
       .label = "Ascending",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(!c.model.sort().descending); }},
      {.action = A::Descending,
       .label = "Descending",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(c.model.sort().descending); }},
      {.action = A::FoldersFirst,
       .label = "Folders First",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(c.model.sort().directoriesFirst); }},
      {.action = A::HiddenLast,
       .label = "Hidden Last",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return checked(c.model.sort().hiddenLast); }},
      {.action = A::ResetFolderView,
       .label = "Use Default View for This Folder",
       .shortcut = "",
       .scope = S::Location,
       .rule = resetFolderView},
      {.action = A::Open, .label = "Open", .shortcut = "Enter", .scope = S::Targets, .rule = open},
      {.action = A::ContainingFolder,
       .label = "Show in Containing Folder",
       .shortcut = "",
       .scope = S::Targets,
       .rule = containing},
      {.action = A::OriginalLocation,
       .label = "Show Original Location",
       .shortcut = "",
       .scope = S::Targets,
       .rule = originalLocation},
      {.action = A::ShowLinkTarget,
       .label = "Show Link Target",
       .shortcut = "",
       .scope = S::Targets,
       .rule = linkTarget},
      {.action = A::Pin,
       .label = "Pin to Sidebar",
       .shortcut = "",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return pin(c, r, false); }},
      {.action = A::Unpin,
       .label = "Unpin",
       .shortcut = "",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return pin(c, r, true); }},
      {.action = A::CutFiles,
       .label = "Cut",
       .shortcut = "Ctrl+X",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return fileClipboard(c, r, true); }},
      {.action = A::CopyFiles,
       .label = "Copy",
       .shortcut = "Ctrl+C",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return fileClipboard(c, r, false); }},
      {.action = A::PasteFiles, .label = "Paste", .shortcut = "Ctrl+V", .scope = S::Location, .rule = paste},
      {.action = A::Rename,
       .label = "Rename…",
       .shortcut = "F2",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return renameOrTrash(c, r, true); }},
      {.action = A::CreateLink, .label = "Create Link…", .shortcut = "", .scope = S::Targets, .rule = createLink},
      {.action = A::Trash,
       .label = "Move to Trash",
       .shortcut = "Delete",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return renameOrTrash(c, r, false); }},
      {.action = A::Restore,
       .label = "Restore",
       .shortcut = "",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return trashItems(c, r, true); }},
      {.action = A::DeletePermanently,
       .label = "Delete Permanently…",
       .shortcut = "Shift+Delete",
       .scope = S::Targets,
       .destructive = true,
       .rule = [](const C& c, const R& r) {
         if (r.folder && r.folder->isTrash()) {
           return trashItems(c, r, false);
         }
         if (const auto state = writable(c); !state.enabled) {
           return state;
         }
         return require(ready(c) && r.targets.size() <= 4096 && all(c, r, [](const DirectoryEntry& entry) {
           const auto location = entry.location();
           return location.isNative() && location.parent().has_value() && entry.access.remove != false;
         }), "Select deletable native items outside Trash.");
       }},
      {.action = A::Properties, .label = "Properties", .shortcut = "Alt+Return", .scope = S::Targets},
      {.action = A::Compress,
       .label = "Compress…",
       .shortcut = "",
       .scope = S::Targets,
       .rule = [](const C& c, const R& r) { return archive(c, r, true); }},
      {.action = A::ExtractHere, .label = "Extract Here", .shortcut = "", .scope = S::Targets, .rule = extract},
      {.action = A::ExtractTo, .label = "Extract To…", .shortcut = "", .scope = S::Targets, .rule = extract},
      {.action = A::BrowseArchive, .label = "Browse Archive", .shortcut = "", .scope = S::Targets, .rule = extract},
      {.action = A::CreateFolder,
       .label = "New Folder…",
       .shortcut = "Ctrl+Shift+N",
       .scope = S::Location,
       .rule = createItem},
      {.action = A::CreateFile,
       .label = "Empty File…",
       .shortcut = "Ctrl+Alt+N",
       .scope = S::Location,
       .rule = createItem},
      {.action = A::NewDocument, .label = "New Document", .shortcut = "", .scope = S::Location, .rule = createItem},
      {.action = A::PinCurrent,
       .label = "Pin This Folder to Sidebar",
       .shortcut = "",
       .scope = S::Location,
       .rule = [](const C& c, const R& r) { return pinCurrent(c, r, false); }},
      {.action = A::UnpinCurrent,
       .label = "Unpin This Folder",
       .shortcut = "",
       .scope = S::Location,
       .rule = [](const C& c, const R& r) { return pinCurrent(c, r, true); }},
      {.action = A::ReloadPins,
       .label = "Reload Pins",
       .shortcut = "",
       .rule = [](const C& c, const R&) { return require(!c.pinsBusy, "Pins are being saved."); }},
      {.action = A::EmptyTrash, .label = "Empty Trash…", .shortcut = "", .destructive = true, .rule = emptyTrash},
      {.action = A::SelectAll, .label = "Select All", .shortcut = "Ctrl+A", .scope = S::Selection, .rule = visibleRows},
      {.action = A::InvertSelection,
       .label = "Invert Selection",
       .shortcut = "Ctrl+Shift+I",
       .scope = S::Selection,
       .rule = visibleRows},
      {.action = A::SelectPattern,
       .label = "Select by Pattern…",
       .shortcut = "Ctrl+S",
       .scope = S::Selection,
       .rule = visibleRows},
      {.action = A::ClearSelection, .label = "Clear Selection", .shortcut = "Escape", .scope = S::Selection},
      {.action = A::Settings, .label = "Settings…", .shortcut = "Ctrl+,"},
      {.action = A::About, .label = "About Luciole", .shortcut = ""},
      {.action = A::MenuBack, .label = "Back", .shortcut = ""},
      {.action = A::SortMenu, .label = "Sort…", .shortcut = ""},
      {.action = A::SelectionMenu, .label = "Selection…", .shortcut = ""},
      {.action = A::CopyPath,
       .label = "Copy Folder Path",
       .shortcut = "",
       .scope = S::Location,
       .rule = folderClipboard},
      {.action = A::CopyUri, .label = "Copy Folder URI", .shortcut = "", .scope = S::Location, .rule = folderClipboard},
      {.action = A::Terminal, .label = "Open Terminal Here", .shortcut = "F4", .scope = S::Location, .rule = terminal},
      {.action = A::CopyTargetPath, .label = "Copy Path", .shortcut = "", .scope = S::Targets, .rule = clipboard},
      {.action = A::CopyTargetUri, .label = "Copy URI", .shortcut = "", .scope = S::Targets, .rule = clipboard},
      {.action = A::RunProgram, .label = "Run as a Program", .shortcut = "", .scope = S::Targets, .rule = runProgram},
      {.action = A::RunInTerminal,
       .label = "Run in Terminal",
       .shortcut = "",
       .scope = S::Targets,
       .rule =
           [](const C& c, const R& r) {
             const auto state = runProgram(c, r);
             if (!state.enabled) {
               return state;
             }
             return require(
                 c.terminalAvailable && !r.targets.front().location.basename().value_or("").ends_with(".desktop"),
                 "Run in Terminal requires xdg-terminal-exec and a program or script."
             );
           }},
      {.action = A::Scripts, .label = "Scripts", .shortcut = "", .scope = S::Selection, .rule = scripts},
      {.action = A::Undo,
       .label = "Undo",
       .shortcut = "Ctrl+Z",
       .rule = [](
                   const C& c, const R&
               ) { return require(c.canUndo && !c.operationsBusy, "No file operation can be undone right now."); }},
      {.action = A::Redo,
       .label = "Redo",
       .shortcut = "Ctrl+Shift+Z",
       .rule = [](
                   const C& c, const R&
               ) { return require(c.canRedo && !c.operationsBusy, "No file operation can be redone right now."); }},
      {.action = A::Operations, .label = "File Operations", .shortcut = ""},
      {.action = A::TargetTerminal,
       .label = "Open in Terminal",
       .shortcut = "",
       .scope = S::Targets,
       .rule = targetTerminal},
      {.action = A::KeyboardShortcuts, .label = "Keyboard Shortcuts", .shortcut = "F1"},
  });
  static_assert(luciole::actionTableInOrder(kActions));
} // namespace

std::span<const luciole::ActionDefinition> luciole::ActionRegistry::definitions() { return kActions; }
const luciole::ActionDefinition& luciole::ActionRegistry::definition(BrowserAction action) {
  return kActions[std::to_underlying(action)];
}
std::optional<luciole::BrowserAction> luciole::ActionRegistry::fromMenuId(int id) {
  if (id < 0 || static_cast<std::size_t>(id) >= kActions.size()) {
    return std::nullopt;
  }
  return kActions[static_cast<std::size_t>(id)].action;
}
luciole::ActionRequest luciole::ActionRegistry::capture(const ActionContext& context) {
  ActionRequest request{context.context, context.model.snapshot().location, std::string(context.query), {}};
  for (const auto id : context.model.selectedIds()) {
    if (auto location = context.model.location(id)) {
      request.targets.push_back({id, std::move(*location)});
    }
  }
  return request;
}
bool luciole::ActionRegistry::current(const ActionContext& context, const ActionRequest& request, ActionScope scope) {
  if (scope == ActionScope::Global) {
    return true;
  }
  if (request.context != context.context
      || request.folder != context.model.snapshot().location
      || request.query != context.query) {
    return false;
  }
  return scope != ActionScope::Targets || std::ranges::all_of(request.targets, [&](const auto& target) {
           return context.model.location(target.id) == target.location && context.model.visible(target.id);
         });
}
luciole::BrowserAction luciole::ActionRegistry::openAction(const ActionContext& context, const ActionRequest& request) {
  return !context.chooser && current(context, request, ActionScope::Targets) && browsableArchive(context, request)
      ? A::BrowseArchive
      : A::Open;
}
bool luciole::chooserAction(BrowserAction action) {
  switch (action) {
  case A::Back:
  case A::Forward:
  case A::Up:
  case A::Home:
  case A::Root:
  case A::Refresh:
  case A::Stop:
  case A::EditLocation:
  case A::Search:
  case A::Grid:
  case A::List:
  case A::ShowHidden:
  case A::ShowSidebar:
  case A::ShowPreview:
  case A::QuickPreview:
  case A::SortName:
  case A::SortType:
  case A::SortSize:
  case A::SortModified:
  case A::SortAccessed:
  case A::SortCreated:
  case A::SortOwner:
  case A::SortGroup:
  case A::SortPermissions:
  case A::SortLocation:
  case A::Ascending:
  case A::Descending:
  case A::FoldersFirst:
  case A::HiddenLast:
  case A::Open:
  case A::SelectAll:
  case A::InvertSelection:
  case A::SelectPattern:
  case A::ClearSelection:
  case A::SortMenu:
  case A::SelectionMenu:
  case A::CopyPath:
  case A::CopyUri:
  case A::KeyboardShortcuts:
  case A::CreateFolder:
    return true;
  case A::Miller:
  case A::ResetFolderView:
  case A::OpenWith:
  case A::ContainingFolder:
  case A::OriginalLocation:
  case A::ShowLinkTarget:
  case A::Pin:
  case A::Unpin:
  case A::CutFiles:
  case A::CopyFiles:
  case A::PasteFiles:
  case A::Rename:
  case A::CreateLink:
  case A::Trash:
  case A::Restore:
  case A::DeletePermanently:
  case A::Properties:
  case A::Compress:
  case A::ExtractHere:
  case A::ExtractTo:
  case A::BrowseArchive:
  case A::CreateFile:
  case A::NewDocument:
  case A::PinCurrent:
  case A::UnpinCurrent:
  case A::ReloadPins:
  case A::EmptyTrash:
  case A::Settings:
  case A::About:
  case A::MenuBack:
  case A::Terminal:
  case A::CopyTargetPath:
  case A::CopyTargetUri:
  case A::RunProgram:
  case A::RunInTerminal:
  case A::Scripts:
  case A::Undo:
  case A::Redo:
  case A::Operations:
  case A::TargetTerminal:
    return false;
  }
  return false;
}

std::vector<luciole::BrowserAction> luciole::itemMenuDeletionActions(bool inTrash, bool showPermanentDeletion) {
  if (inTrash) {
    return {A::Restore, A::DeletePermanently};
  }
  if (showPermanentDeletion) {
    return {A::Trash, A::DeletePermanently};
  }
  return {A::Trash};
}

luciole::ActionState
luciole::ActionRegistry::state(BrowserAction action, const ActionContext& c, const ActionRequest& r) {
  if (c.chooser && !chooserAction(action)) {
    return unavailable("Not available while choosing files.");
  }
  if (c.singleSelection && (action == A::SelectAll || action == A::InvertSelection || action == A::SelectPattern)) {
    return unavailable("Only one item can be chosen.");
  }
  const auto& row = definition(action);
  if (!current(c, r, row.scope)) {
    return unavailable("The folder or targets changed. Open the menu again.");
  }
  if (row.scope == ActionScope::Targets && r.targets.empty()) {
    return unavailable("Select an item first.");
  }
  if (row.scope == ActionScope::Targets) {
    if (!all(c, r, [](const DirectoryEntry& entry) { return !entry.metadataPending; })) {
      return unavailable("Reading item metadata…");
    }
    if (!all(c, r, [](const DirectoryEntry& entry) { return !entry.metadataFailed; })) {
      return unavailable("Item metadata is unavailable. Refresh and try again.");
    }
  }
  if ((row.scope == ActionScope::Targets || row.scope == ActionScope::Selection) && !c.model.selectionAvailable()) {
    return unavailable("The listing is not available for selection.");
  }
  return row.rule != nullptr ? row.rule(c, r) : ActionState{};
}
