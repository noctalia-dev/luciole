#pragma once

#include "browser/directory_model.h"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace luciole {
  // A new action needs a row in kActions (action_registry.cpp) and a handler in BrowserPage::runAction, both in
  // enum order; static_asserts reject a missing row.
  enum class BrowserAction : std::uint8_t {
    Back,
    Forward,
    Up,
    Home,
    Root,
    Refresh,
    Stop,
    EditLocation,
    Search,
    Grid,
    List,
    Miller,
    ShowHidden,
    ShowSidebar,
    ShowPreview,
    QuickPreview,
    OpenWith,
    SortName,
    SortType,
    SortSize,
    SortModified,
    SortAccessed,
    SortCreated,
    SortOwner,
    SortGroup,
    SortPermissions,
    SortLocation,

    Ascending,
    Descending,
    FoldersFirst,
    HiddenLast,
    ResetFolderView,
    Open,
    ContainingFolder,
    OriginalLocation,
    ShowLinkTarget,
    Pin,
    Unpin,
    CutFiles,
    CopyFiles,
    PasteFiles,
    Rename,
    CreateLink,
    Trash,
    Restore,
    DeletePermanently,
    Properties,
    Compress,
    ExtractHere,
    ExtractTo,
    BrowseArchive,
    CreateFolder,
    CreateFile,
    NewDocument,
    PinCurrent,
    UnpinCurrent,
    ReloadPins,
    EmptyTrash,
    SelectAll,
    InvertSelection,
    SelectPattern,
    ClearSelection,
    Settings,
    About,
    MenuBack,
    SortMenu,
    SelectionMenu,
    CopyPath,
    CopyUri,
    Terminal,
    CopyTargetPath,
    CopyTargetUri,
    RunProgram,
    RunInTerminal,
    Scripts,
    Undo,
    Redo,
    Operations,
    TargetTerminal,
    KeyboardShortcuts
  };
  // KeyboardShortcuts stays the last enumerator.
  inline constexpr std::size_t kBrowserActionCount =
      std::size_t{std::to_underlying(BrowserAction::KeyboardShortcuts)} + 1;
  enum class ActionScope : std::uint8_t { Global, Location, Targets, Selection };
  struct ActionTarget {
    EntryId id;
    Location location;
  };
  // Menus retain identity and context, never a borrowed row or operation index.
  struct ActionRequest {
    std::uint64_t context = 0;
    std::optional<Location> folder;
    std::string query;
    std::vector<ActionTarget> targets;
  };
  struct ActionContext {
    const DirectoryModel& model;
    std::uint64_t context = 0;
    std::string_view query;
    bool canBack = false, canForward = false, canUp = false;
    bool operationsBusy = false;
    bool pinsEditable = false, pinsBusy = false, canEmptyTrash = false;
    std::function<bool(const Location&)> pinned;
    ViewMode viewMode = ViewMode::List;
    bool showSidebar = true;
    bool showPreview = false;
    bool clipboardAvailable = false;
    bool clipboardHasFiles = false;
    bool clipboardHasImage = false;
    bool terminalAvailable = false;
    bool canUndo = false, canRedo = false;
    bool chooser = false;
    bool singleSelection = false;
    // Per-folder views are on, and the current folder has its own sort or view mode.
    bool folderViews = false, folderViewCustomized = false;
  };
  // Actions a file chooser window offers: browsing, selection and New Folder. Everything else would act on files or
  // leave the chooser.
  bool chooserAction(BrowserAction action);
  // Deletion entries of the item context menu. Trash always offers permanent deletion; outside Trash it is opt-in.
  std::vector<BrowserAction> itemMenuDeletionActions(bool inTrash, bool showPermanentDeletion);
  struct ActionState {
    bool enabled = true;
    std::string_view reason;
    int checked = -1;
  };
  // Runs after the scope checks in ActionRegistry::state(); nullptr means always enabled.
  using ActionRule = ActionState (*)(const ActionContext& context, const ActionRequest& request);
  struct ActionDefinition {
    BrowserAction action;
    std::string_view label;
    std::string_view shortcut;
    ActionScope scope = ActionScope::Global;
    bool destructive = false;
    ActionRule rule = nullptr;
  };
  // True when a per-action table has one row per action in enum order, so it can be indexed by enum value.
  template <class Row, std::size_t N> constexpr bool actionTableInOrder(const std::array<Row, N>& rows) {
    if (N != kBrowserActionCount) {
      return false;
    }
    for (std::size_t index = 0; index < N; ++index) {
      if (std::to_underlying(rows[index].action) != index) {
        return false;
      }
    }
    return true;
  }
  class ActionRegistry {
  public:
    static std::span<const ActionDefinition> definitions();
    static const ActionDefinition& definition(BrowserAction action);
    static int menuId(BrowserAction action) { return std::to_underlying(action); }
    // The action a menu entry id names, or nullopt for ids outside the action range.
    static std::optional<BrowserAction> fromMenuId(int id);
    static ActionRequest capture(const ActionContext& context);
    static bool current(const ActionContext& context, const ActionRequest& request, ActionScope scope);
    static ActionState state(BrowserAction action, const ActionContext& context, const ActionRequest& request);
    // The action Open performs: Browse Archive for one browsable archive, otherwise Open. Running jobs do not change
    // the choice; state() still refuses Browse Archive meanwhile.
    static BrowserAction openAction(const ActionContext& context, const ActionRequest& request);
  };
} // namespace luciole
