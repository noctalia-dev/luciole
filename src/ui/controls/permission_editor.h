#pragma once

#include "operations/permission_service.h"
#include "ui/controls/flex.h"

#include <array>
#include <string>

class Input;
class Toggle;
class Label;
class Segmented;
class RecursivePermissionEditor;

// The snapshot's failure text, or its status message when it did not fail as a whole.
std::string permissionStatusText(const luciole::PermissionSnapshot& snapshot);
// "rwxr-x---  0750": symbolic like ls, with s/S and t/T for the special bits, then octal.
std::string permissionModeText(std::uint32_t mode);

class PermissionEditor final : public Flex {
public:
  explicit PermissionEditor(std::function<void()> edited, bool multiple = false, bool folders = false);
  void sync(const luciole::PermissionSnapshot& snapshot);
  bool canApply() const;
  luciole::PermissionEdit edit() const;
  bool recursive() const;
  luciole::RecursivePermissionEdit recursiveEdit() const;
  const Node* messageNode() const;

private:
  void changed();
  std::uint32_t mode() const;
  std::array<Toggle*, 12> m_bits{};
  std::array<Segmented*, 12> m_choices{};
  std::array<Label*, 12> m_current{};
  bool m_multiple = false;
  Toggle* m_recursive = nullptr;
  Flex* m_direct = nullptr;
  RecursivePermissionEditor* m_tree = nullptr;
  Input *m_owner = nullptr, *m_group = nullptr;
  // m_mode is the Access card's badge; the IDs sit under the Owner and Group row titles.
  Label *m_mode = nullptr, *m_reason = nullptr, *m_message = nullptr, *m_ownerId = nullptr, *m_groupId = nullptr;
  luciole::PermissionSnapshot m_snapshot;
  std::string m_initialOwner, m_initialGroup;
  bool m_syncing = false;
  std::function<void()> m_edited;
};
