# Recent

See the files you used last, in any application, and get back to them.

## Open Recent

Choose **Recent** under **Home** in Places, or enter `recent:///`. Files appear newest first, each with the folder it lives in. Sort it like any folder. A sort you pick there lasts while the window is open, or is remembered with [per-folder views](views.md#remember-a-view-per-folder), and never changes the sort of other folders.

The list is the one GTK applications and file dialogs share, so files opened elsewhere appear too. It needs the system's GVfs service; without it, Recent is not shown.

## Use a recent file

Open, Open With, Quick Preview, Copy, Copy Location and Properties work on the file itself. **Show in Containing Folder** opens the folder the file is in, where you can rename, move or delete it. Dragging a file out of Recent always copies it, so the original stays where it is.

The menu also offers what fits the file:

- An archive opens like a folder and offers **Browse Archive**, **Extract Here** and **Extract To…**. Extract Here puts the contents next to the archive, not in Recent.
- **Compress…** creates the archive next to the file.
- A program, script or application launcher you may run offers **Run as a Program**, and **Run in Terminal** for programs and scripts. It runs from the folder the file is in.

Recent only lists files, so actions that would change the folder, such as Rename, Cut, Paste, New Folder, Move to Trash or Scripts, are not offered there.

## Remove or clear

**Remove from Recent**, or `Delete`, takes the selected files off the list. **Clear Recent…** in the folder's context menu empties the whole list after asking. The files themselves are never touched. Clearing also empties the list for every other application that shares it.

## What Luciole records

Files you open from Luciole, and files you pick in its Open dialog, are added to the list, as GTK applications do. Only local files are recorded. To stop this, turn off **Settings → Appearance → Sidebar → Remember Opened Files**; what is already on the list stays.

## Show or hide Recent

**Settings → Appearance → Sidebar → Show Recent**, or **Hide Recent** in the row's context menu, hides or restores the row in both sidebar modes. `recent:///` still opens while it is hidden.
