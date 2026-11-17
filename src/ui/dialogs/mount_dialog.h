#pragma once

#include "fs/mount_interaction.h"
#include "ui/controls/flex.h"

class Input;
class InputArea;
class Label;
class MountDialog final : public Flex {
public:
  MountDialog(
      const luciole::MountPrompt& prompt, std::function<bool(luciole::MountCredentials)> submit,
      const std::function<void(std::size_t)>& choose, std::function<void()> cancel
  );
  InputArea* focusArea() const;

private:
  void submit(bool guest);
  Input* m_user = nullptr;
  Input* m_domain = nullptr;
  Input* m_password = nullptr;
  Label* m_error = nullptr;
  std::function<bool(luciole::MountCredentials)> m_submit;
};
