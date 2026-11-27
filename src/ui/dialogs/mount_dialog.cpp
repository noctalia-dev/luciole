#include "ui/dialogs/mount_dialog.h"

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"

MountDialog::MountDialog(
    const luciole::MountPrompt& prompt, std::function<bool(luciole::MountCredentials)> submit,
    const std::function<void(std::size_t)>& choose, std::function<void()> cancel
)
    : m_submit(std::move(submit)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  const bool device = prompt.purpose == luciole::MountPurpose::Device;
  const auto* title = device ? (prompt.password ? "Unlock Drive" : "Drive Confirmation")
                             : (prompt.password ? "Connect to Server" : "Server Confirmation");
  addChild(luciole::dialogHeader(title, device ? "database" : "network", cancel));
  auto scroll = ui::scrollView({.viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .flexGrow = 1.0F});
  scroll->content()->setDirection(FlexDirection::Vertical);
  scroll->content()->setAlign(FlexAlign::Stretch);
  scroll->content()->setGap(Style::spaceSm);
  auto& body = *scroll->content();
  body.addChild(ui::label({.text = prompt.message, .maxLines = 4, .configure = [](Label& label) {
                             label.setUseMarkup(false);
                           }}));
  const auto field = [&](const std::string& label, Input** out, const std::string& value, bool password) {
    body.addChild(
        ui::label(
            {.text = label, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    body.addChild(
        ui::input({.out = out, .value = value, .passwordMode = password, .onSubmit = [this](const std::string&) {
                     this->submit(false);
                   }})
    );
  };
  if (prompt.password) {
    if (prompt.flags & G_ASK_PASSWORD_NEED_USERNAME) {
      field("Username", &m_user, prompt.user, false);
    }
    if (prompt.flags & G_ASK_PASSWORD_NEED_DOMAIN) {
      field("Domain", &m_domain, prompt.domain, false);
    }
    if (prompt.flags & G_ASK_PASSWORD_NEED_PASSWORD) {
      field("Password", &m_password, {}, true);
    }
  } else {
    for (std::size_t i = 0; i < prompt.choices.size(); ++i) {
      body.addChild(ui::button({.text = prompt.choices[i], .variant = ButtonVariant::Ghost, .onClick = [choose, i] {
                                  choose(i);
                                }}));
    }
  }
  addChild(std::move(scroll));
  addChild(ui::label({.out = &m_error, .color = colorSpecFromRole(ColorRole::Error), .maxLines = 2}));
  auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm});
  footer->addChild(ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = std::move(cancel)}));
  if (prompt.password) {
    if (prompt.flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) {
      footer->addChild(ui::button({.text = "Guest", .variant = ButtonVariant::Ghost, .onClick = [this] {
                                     this->submit(true);
                                   }}));
    }
    footer->addChild(
        ui::button({.text = device ? "Unlock" : "Connect", .variant = ButtonVariant::Primary, .onClick = [this] {
                      this->submit(false);
                    }})
    );
  }
  addChild(std::move(footer));
}
void MountDialog::submit(bool guest) {
  if (!guest && m_user && m_user->value().empty()) {
    m_error->setText(i18n::text("Enter a username or connect as a guest."));
    markLayoutDirty();
    return;
  }
  luciole::MountCredentials credentials{
      m_user ? m_user->value() : "", m_domain ? m_domain->value() : "", m_password ? m_password->value() : "", guest
  };
  if (!m_submit(std::move(credentials))) {
    m_error->setText(i18n::text("This request is no longer active."));
    markLayoutDirty();
  }
}
InputArea* MountDialog::focusArea() const {
  const auto* field = m_user && m_user->value().empty() ? m_user : m_password ? m_password : m_user;
  return field ? field->inputArea() : nullptr;
}
