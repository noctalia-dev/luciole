#pragma once

#include "ui/controls/flex.h"

#include <cstdint>
#include <functional>
#include <memory>

// Owns whatever a page shows on its menu popup surface: one context menu or dialog at a time.
// The page's closeMenu() is the single close path; it calls close() and later release().
class ActiveDialog {
public:
  enum class Kind : std::uint8_t {
    None,
    Menu,
    Settings,
    PatternSelection,
    EmptyTrash,
    CreateItem,
    Link,
    BatchRename,
    FileRename,
    PlaceRename,
    Properties,
    About,
    QuickPreview,
    Applications,
    TransferConflict,
    DropAction,
    ArchivePassword,
    RunProgram,
    Operations,
    OperationDetails,
  };

  // Takes the popup scene. `content` is the menu or dialog inside it: the scene itself for menus, the dialog
  // under its shadow frame for dialogs. It counts as a plain menu until bind() names it.
  void open(std::unique_ptr<Flex> scene, Flex* content);
  // Names the current dialog and sets the cleanup the next close() runs once: cancel its job, drop widget pointers.
  void bind(Kind kind, std::function<void()> onClose);
  // Ends the current dialog: bumps the generation, then runs its cleanup. The scene lives until release().
  void close();
  // Destroys the scene once the surface and dispatcher no longer reference it.
  void release();

  Kind kind() const { return m_kind; }
  // The menu or dialog content.
  Flex* root() const { return m_content; }
  // What the popup surface draws and the dispatcher hit-tests: the content, or the dialog's shadow frame.
  Flex* scene() const { return m_scene.get(); }
  // Changes on every close(); deferred callbacks compare it to detect a dialog that was replaced.
  std::uint64_t generation() const { return m_generation; }

private:
  Kind m_kind = Kind::None;
  std::unique_ptr<Flex> m_scene;
  Flex* m_content = nullptr;
  std::function<void()> m_onClose;
  std::uint64_t m_generation = 0;
};
