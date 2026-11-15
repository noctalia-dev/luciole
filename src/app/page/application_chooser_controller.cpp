#include "app/page/application_chooser_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "platform/file_launcher.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>

struct ApplicationChooserController::Session {
  std::vector<std::string> types;
  std::vector<luciole::Location> locations;
  luciole::ActionRequest request;
  std::uint64_t generation = 0;
  std::string query;
  bool other = false, useDefault = false, submitting = false, canSetDefault = false;
};

ApplicationChooserController::ApplicationChooserController(
    PageContext& page, luciole::FileLauncher& launcher, luciole::FileIconCache& icons
)
    : m_page(page), m_launcher(launcher), m_icons(icons) {}

bool ApplicationChooserController::current(const std::shared_ptr<Session>& session) const {
  return m_session == session
      && m_container
      && m_page.menuSurface()
      && m_page.active()
      && session->generation == m_page.activeDialog().generation();
}

void ApplicationChooserController::show(const luciole::DirectoryEntry& entry) { show(std::vector{entry}); }

void ApplicationChooserController::show(const std::vector<luciole::DirectoryEntry>& entries) {
  if (entries.empty()) {
    return;
  }
  auto session = std::make_shared<Session>();
  session->request = m_page.requestFor(entries);
  for (const auto& entry : entries) {
    session->types.emplace_back(entry.contentType.value_or("application/octet-stream"));
    session->locations.push_back(entry.location());
  }
  std::ranges::sort(session->types);
  const auto duplicates = std::ranges::unique(session->types);
  session->types.erase(duplicates.begin(), duplicates.end());
  session->canSetDefault = session->types.size() == 1
      && !session->types.front().empty()
      && session->types.front() != "application/octet-stream";
  Flex* choices = nullptr;
  ScrollView* choiceScroll = nullptr;
  Input* search = nullptr;
  Button* other = nullptr;
  Toggle* useDefault = nullptr;
  Label* error = nullptr;
  auto scroll = ui::scrollView(
      {.out = &choiceScroll, .viewportPaddingH = 0, .viewportPaddingV = 0, .fillWidth = true, .flexGrow = 1}
  );
  scroll->content()->setAlign(FlexAlign::Stretch);
  scroll->content()->addChild(
      ui::column(
          {.out = &choices, .align = FlexAlign::Stretch, .gap = Style::spaceXs},
          ui::label({.text = "Loading applications…", .maxLines = 2})
      )
  );
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader(
          entries.size() == 1 ? "Open With" : std::format("Open {} Files With", entries.size()), "apps",
          [this] { m_page.dismissMenu(); }
      ),
      ui::input(
          {.out = &search,
           .placeholder = "Search applications…",
           .clearButtonEnabled = true,
           .onChange =
               [this, session](const std::string& value) {
                 if (!current(session) || session->submitting) {
                   return;
                 }
                 session->query = value;
                 m_launcher.cancelApplications();
                 enableChoices(false);
                 m_searchDelay.start(std::chrono::milliseconds(150), [this] { load(); });
               }}
      ),
      std::move(scroll),
      ui::button(
          {.out = &other,
           .text = "Show other applications",
           .variant = ButtonVariant::Ghost,
           .onClick =
               [this, session] {
                 if (current(session) && !session->submitting) {
                   session->other = !session->other;
                   m_other->setText(session->other ? "Show suggested applications" : "Show other applications");
                   load();
                 }
               }}
      ),
      ui::separator({.visible = session->types.size() == 1, .participatesInLayout = session->types.size() == 1}),
      ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .visible = session->types.size() == 1,
           .participatesInLayout = session->types.size() == 1},
          ui::label({.text = "Use as default for this file type", .maxLines = 2, .flexGrow = 1}),
          ui::toggle(
              {.out = &useDefault,
               .enabled = session->canSetDefault,
               .onChange = [session](bool value) { session->useDefault = value; }}
          )
      ),
      ui::label(
          {.out = &error,
           .color = colorSpecFromRole(ColorRole::Error),
           .maxLines = 4,
           .visible = false,
           .participatesInLayout = false,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      )
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (!m_page.menuSurface()) {
    return;
  }
  m_page.activeDialog().bind(ActiveDialog::Kind::Applications, [this] {
    m_searchDelay.stop();
    m_launcher.cancelApplications();
    m_session.reset();
    m_choices.clear();
    m_container = nullptr;
    m_scroll = nullptr;
    m_search = nullptr;
    m_other = nullptr;
    m_default = nullptr;
    m_error = nullptr;
  });
  session->generation = m_page.activeDialog().generation();
  m_session = std::move(session);
  m_container = choices;
  m_scroll = choiceScroll;
  m_search = search;
  m_other = other;
  m_default = useDefault;
  m_error = error;
  m_page.menuDispatcher().setFocus(m_search->inputArea());
  load();
}

void ApplicationChooserController::enableChoices(bool enabled) {
  for (auto* choice : m_choices) {
    choice->setEnabled(enabled);
  }
}

void ApplicationChooserController::load() {
  m_searchDelay.stop();
  if (!m_session || !current(m_session) || m_session->submitting) {
    return;
  }
  enableChoices(false);
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_launcher.applications(
      m_session->types,
      [this, lifetime, session = m_session](luciole::ApplicationList apps) {
        if (!lifetime.expired() && current(session)) {
          populate(session, std::move(apps));
        }
      },
      m_session->other, m_session->query
  );
}

void ApplicationChooserController::populate(const std::shared_ptr<Session>& session, luciole::ApplicationList apps) {
  m_choices.clear();
  while (!m_container->children().empty()) {
    (void)m_container->removeChild(m_container->children().back().get());
  }
  if (!m_page.actionState(luciole::BrowserAction::OpenWith, session->request).enabled) {
    showError("The item changed. Select it again.");
    apps.choices.clear();
  }
  if (apps.choices.empty()) {
    m_container->addChild(ui::label({.text = "No matching applications.", .maxLines = 2}));
  }
  bool suggested = true;
  for (const auto& app : apps.choices) {
    if (session->other && suggested != app.suggested) {
      suggested = app.suggested;
      if (!m_choices.empty()) {
        m_container->addChild(ui::separator({}));
      }
    }
    auto button = ui::button(
        {.text = app.name,
         .controlHeight = Style::controlHeight,
         .contentAlign = ButtonContentAlign::Start,
         .variant = ButtonVariant::Ghost,
         .tooltip = app.name + (app.isDefault ? " (Default)" : ""),
         .gap = Style::spaceSm,
         .onClick = [this, session, app] { choose(session, app); },
         .configure = [](Button& button) { button.label()->setUseMarkup(false); }}
    );
    button->setFillWidth(true);
    auto source = app.icon;
    source.names.emplace_back("application-x-executable");
    auto icon = std::make_unique<luciole::FileIcon>(m_icons, 24.0F);
    icon->bind(source, colorSpecFromRole(ColorRole::OnSurface));
    button->insertChildAt(0, std::move(icon));
    if (app.isDefault) {
      button->addChild(ui::glyph({.glyph = "check", .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
    }
    m_choices.push_back(button.get());
    m_container->addChild(std::move(button));
  }
  if (apps.truncated) {
    m_container->addChild(ui::label({.text = "More applications match. Refine your search.", .maxLines = 2}));
  }
  m_scroll->requestScrollToOffset(0);
  m_page.menuSurface()->requestLayout();
}

void ApplicationChooserController::showError(const std::string& message) {
  m_error->setText(message);
  m_error->setVisible(!message.empty());
  m_error->setParticipatesInLayout(!message.empty());
  m_page.menuSurface()->requestLayout();
}

void ApplicationChooserController::choose(
    const std::shared_ptr<Session>& session, const luciole::ApplicationChoice& app
) {
  if (!current(session) || session->submitting) {
    return;
  }
  if (!m_page.actionState(luciole::BrowserAction::OpenWith, session->request).enabled) {
    showError("The item changed. Select it again.");
    return;
  }
  if (!session->useDefault) {
    m_launcher.openWith(session->locations, app);
    m_page.dismissMenu();
    return;
  }
  session->submitting = true;
  enableChoices(false);
  m_search->setEnabled(false);
  m_other->setEnabled(false);
  m_default->setEnabled(false);
  showError({});
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_launcher.setDefault(session->types.front(), app, [this, lifetime, session, app](const std::string& error) {
    if (lifetime.expired() || !current(session)) {
      return;
    }
    if (error.empty() && m_page.actionState(luciole::BrowserAction::OpenWith, session->request).enabled) {
      m_launcher.openWith(session->locations, app);
      m_page.dismissMenu();
      return;
    }
    session->submitting = false;
    m_search->setEnabled(true);
    m_other->setEnabled(true);
    m_default->setEnabled(session->canSetDefault);
    enableChoices(true);
    showError(error.empty() ? "Default saved, but the item changed. Select it again." : error);
  });
}

void ApplicationChooserController::revealFocus(InputArea* focused) {
  if (m_scroll) {
    m_scroll->requestScrollIntoView(focused);
  }
}
