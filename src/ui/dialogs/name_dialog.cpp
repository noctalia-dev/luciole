#include "ui/dialogs/name_dialog.h"

#include "ui/builders.h"
#include "ui/dialogs/dialog_chrome.h"

NameDialog::NameDialog(
    float width, const std::string& title, const std::string& glyph, const std::string& value, std::string hint,
    std::function<std::optional<std::string>(const std::string&)> validate, std::function<void(std::string)> submit,
    std::function<void()> cancel, std::string action, std::string busyAction
)
    : m_hint(std::move(hint)), m_action(std::move(action)), m_busyAction(std::move(busyAction)),
      m_validate(std::move(validate)), m_submit(std::move(submit)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  addChild(luciole::dialogHeader(title, glyph, cancel));
  addChild(
      ui::input(
          {.out = &m_input,
           .value = value,
           .placeholder = "Name",
           .onChange = [this](const std::string&) { showError({}); },
           .onSubmit = [this](const std::string&) { this->submit(); }}
      )
  );
  // The message row holds the hint or an error; it collapses while there is neither.
  addChild(
      ui::column(
          {.out = &m_messageRow, .align = FlexAlign::Stretch, .minHeight = 44.0F, .maxHeight = 44.0F},
          ui::label(
              {.out = &m_message,
               .text = m_hint,
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxWidth = width - 24.0F,
               .maxLines = 3,
               .configure = [](Label& message) { message.setUseMarkup(false); }}
          )
      )
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.out = &m_cancel,
               .text = "Cancel",
               .variant = ButtonVariant::Ghost,
               .minWidth = 80.0F,
               .onClick = std::move(cancel)}
          ),
          ui::button(
              {.out = &m_save,
               .text = m_action,
               .variant = ButtonVariant::Primary,
               .minWidth = 88.0F,
               .onClick = [this] { this->submit(); }}
          )
      )
  );
  showError({});
}

void NameDialog::setSaving(bool saving) {
  m_saving = saving;
  m_input->setEnabled(!saving);
  m_save->setEnabled(!saving);
  m_save->setText(saving ? m_busyAction : m_action);
  m_cancel->setText(saving ? "Close" : "Cancel");
}

void NameDialog::showError(const std::string& error) {
  m_input->setInvalid(!error.empty());
  m_message->setText(error.empty() ? m_hint : error);
  const bool shown = !error.empty() || !m_hint.empty();
  m_messageRow->setVisible(shown);
  m_messageRow->setParticipatesInLayout(shown);
  m_message->setColor(colorSpecFromRole(error.empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error));
}

void NameDialog::submit() {
  if (m_saving) {
    return;
  }
  if (const auto error = m_validate(m_input->value())) {
    showError(*error);
    return;
  }
  if (m_submit) {
    m_submit(m_input->value());
  }
}
