#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <optional>
#include <string>

class Input;
class Label;
class Button;

class NameDialog final : public Flex {
public:
  NameDialog(
      float width, const std::string& title, const std::string& glyph, const std::string& value, std::string hint,
      std::function<std::optional<std::string>(const std::string&)> validate, std::function<void(std::string)> submit,
      std::function<void()> cancel, std::string action = "Rename", std::string busyAction = "Renaming…"
  );
  Input* input() const { return m_input; }
  void setSaving(bool saving);
  void showError(const std::string& error);

private:
  void submit();
  Input* m_input = nullptr;
  Label* m_message = nullptr;
  Flex* m_messageRow = nullptr;
  Button* m_save = nullptr;
  Button* m_cancel = nullptr;
  bool m_saving = false;
  std::string m_hint;
  std::string m_action;
  std::string m_busyAction;
  std::function<std::optional<std::string>(const std::string&)> m_validate;
  std::function<void(std::string)> m_submit;
};
