#pragma once

// Window and tab commands a page asks the workspace to carry out.
enum class WorkspaceCommand {
  ClosePane,
  NewTab,
  // Folder opening follows the preference; explicit NewTab always activates.
  OpenTab,
  NewBackgroundTab,
  Duplicate,
  Close,
  Reopen,
  Split,
  SwitchArea,
  Swap,
  NewWindow,
  Detach,
  MoveWindow,
  OpenOther,
  CopyOther,
  MoveOther
};
