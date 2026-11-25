#include "app/page/program_controller.h"

#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "app/page/submenu_controller.h"
#include "browser/browser_controller.h"
#include "config/settings.h"
#include "fs/background_job.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <utility>

ProgramController::ProgramController(PageContext& page, SubmenuController& submenus, WaylandConnection& connection)
    : m_page(page), m_submenus(submenus), m_connection(connection) {}
ProgramController::~ProgramController() { cancel(); }
void ProgramController::cancel() {
  m_listing.cancel();
  m_launch.cancel();
}
void ProgramController::closeScripts() { m_listing.cancel(); }
bool ProgramController::current(const luciole::ActionRequest& request) const {
  return m_page.visible() && m_page.requestCurrent(request, luciole::ActionScope::Targets);
}

void ProgramController::open(const luciole::ActionRequest& request, std::function<void()> openDocument) {
  if (!current(request) || request.targets.size() != 1 || busy()) {
    return;
  }
  m_launch = luciole::CancelToken{};
  const auto token = m_launch;
  ++m_pending;
  const std::weak_ptr<int> lifetime = m_lifetime;
  luciole::runJob<std::optional<luciole::PreparedProgram>>(
      token,
      [location = request.targets.front().location, token] { return luciole::prepareApplication(location, token); },
      [this, lifetime, request, token, openDocument = std::move(openDocument)](auto result) {
        if (lifetime.expired()) {
          return;
        }
        --m_pending;
        if (token.cancelled() || !current(request)) {
          return;
        }
        if (!result) {
          m_page.actionError(result.error().message);
        } else if (*result) {
          launch(request, std::move(**result), false, false);
        } else {
          openDocument();
        }
      }
  );
}

void ProgramController::run(const luciole::ActionRequest& request, bool terminal) {
  if (!current(request) || request.targets.size() != 1 || busy()) {
    return;
  }
  m_launch = luciole::CancelToken{};
  const auto token = m_launch;
  ++m_pending;
  const std::weak_ptr<int> lifetime = m_lifetime;
  luciole::runJob<luciole::PreparedProgram>(
      token, [location = request.targets.front().location, token] { return luciole::prepareProgram(location, token); },
      [this, lifetime, request, terminal, token](auto result) {
        if (lifetime.expired()) {
          return;
        }
        --m_pending;
        if (token.cancelled() || !current(request)) {
          return;
        }
        if (!result) {
          m_page.actionError(result.error().message);
        } else if (result->desktop) {
          confirm(request, std::move(*result));
        } else {
          launch(request, std::move(*result), terminal, false);
        }
      }
  );
}

void ProgramController::confirm(const luciole::ActionRequest& request, luciole::PreparedProgram program) {
  auto root = ui::column({.align = FlexAlign::Stretch});
  luciole::applyDialogStyle(*root);
  root->addChild(luciole::dialogHeader("Run application launcher?", "player-play", [this] { m_page.dismissPopup(); }));
  const auto text = [&](const std::string& value, int lines) {
    root->addChild(ui::label({.text = value, .maxLines = lines, .configure = [](Label& label) {
                                label.setUseMarkup(false);
                              }}));
  };
  text(program.name, 2);
  auto command = ui::scrollView(
      {.fillWidth = true, .height = std::clamp(static_cast<float>(m_page.surface().height()) - 320.0F, 60.0F, 120.0F)}
  );
  command->content()->addChild(ui::label({.text = program.command, .maxLines = 0, .configure = [](Label& label) {
                                            label.setUseMarkup(false);
                                          }}));
  root->addChild(std::move(command));
  text("Run this command only if you trust this launcher. Permission applies to this launch only.", 3);
  auto buttons = ui::row({.justify = FlexJustify::End, .gap = Style::spaceSm});
  auto cancel =
      ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = [this] { m_page.dismissPopup(); }});
  auto* focus = cancel.get();
  buttons->addChild(std::move(cancel));
  buttons->addChild(
      ui::button(
          {.text = "Launch Once", .onClick = [this, capturedRequest = request, program = std::move(program)] mutable {
             auto captured = std::move(capturedRequest);
             auto selected = std::move(program);
             m_page.dismissPopup();
             if (current(captured)) {
               launch(captured, std::move(selected), false, false);
             }
           }}
      )
  );
  root->addChild(std::move(buttons));
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface()) {
    // Dismissal retires this confirmation, not a launch accepted just before
    // the deferred popup close runs.
    m_page.activePopup().bind(ActivePopup::Kind::RunProgram, [token = m_launch] { token.cancel(); });
    m_page.popupDispatcher().setFocus(focus->inputArea());
  }
}

void ProgramController::launch(
    const luciole::ActionRequest& request, luciole::PreparedProgram program, bool terminal, bool script
) {
  if (!current(request) || busy()) {
    return;
  }
  m_launch = luciole::CancelToken{};
  const auto token = m_launch;
  const auto folder = request.folder ? *request.folder : *request.targets.front().location.parent();
  std::vector<luciole::Location> arguments;
  arguments.reserve(script ? request.targets.size() : 0);
  if (script) {
    for (const auto& target : request.targets) {
      arguments.push_back(target.location);
    }
  }
  ++m_pending;
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_connection.extension<luciole::DesktopIntegration>().requestActivation(
      m_page.surface().wlSurface(), m_connection.lastInputSerial(), {},
      [this, lifetime, request, program = std::move(program), folder, arguments = std::move(arguments), terminal,
       token](std::string activation) mutable {
        if (lifetime.expired()) {
          return;
        }
        if (token.cancelled() || !current(request)) {
          --m_pending;
          return;
        }
        luciole::runJob<bool>(
            token,
            [program = std::move(program), folder, arguments = std::move(arguments), terminal,
             activation = std::move(activation), token, terminalCommand = m_page.settings().terminalCommand] {
              luciole::launchProgram(program, folder, arguments, terminal, activation, token, terminalCommand);
              return true;
            },
            [this, lifetime, request, token](auto result) {
              if (!lifetime.expired()) {
                --m_pending;
                if (!token.cancelled() && current(request)) {
                  m_page.actionError(result ? "Program started." : result.error().message);
                }
              }
            }
        );
      }
  );
}

void ProgramController::showScripts(
    const luciole::ActionRequest& request, luciole::ScriptListing listing, bool loading
) {
  if (!current(request)) {
    return;
  }
  auto programs = std::make_shared<std::vector<luciole::PreparedProgram>>(std::move(listing.items));
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(programs->size() + 4);
  for (std::size_t index = 0; index < programs->size(); ++index) {
    entries.push_back({.id = static_cast<int>(index), .label = (*programs)[index].name, .glyph = "script"});
  }
  if (loading || listing.error || programs->empty()) {
    entries.push_back(
        {.label = loading    ? "Loading scripts…"
             : listing.error ? listing.error->message
                             : "No scripts installed",
         .enabled = false}
    );
  }
  if (listing.truncated) {
    entries.push_back({.label = "Script list limit reached", .enabled = false});
  }
  entries.push_back({.label = "", .separator = true});
  entries.push_back({.id = -1, .label = "Open Scripts Folder", .glyph = "folder-open"});
  m_submenus.show(
      std::move(entries),
      [this, request, programs](int id) {
        if (!current(request)) {
          return;
        }
        if (id == -1) {
          openScriptsFolder(request);
        } else if (id >= 0 && static_cast<std::size_t>(id) < programs->size()) {
          launch(request, (*programs)[static_cast<std::size_t>(id)], false, true);
        }
      },
      300.0F
  );
}

void ProgramController::scripts(const luciole::ActionRequest& request) {
  if (!current(request)) {
    return;
  }
  closeScripts();
  m_listing = luciole::CancelToken{};
  const auto token = m_listing;
  showScripts(request, {}, true);
  const auto generation = m_submenus.generation();
  const std::weak_ptr<int> lifetime = m_lifetime;
  ++m_pending;
  luciole::runJob<luciole::ScriptListing>(
      token, [token] { return luciole::listScripts(token); },
      [this, lifetime, request, generation, token](auto result) {
        if (!lifetime.expired()) {
          --m_pending;
          if (!token.cancelled() && generation == m_submenus.generation()) {
            if (result) {
              showScripts(request, std::move(*result), false);
            } else {
              showScripts(request, {{}, result.error(), false}, false);
            }
          }
        }
      }
  );
}

void ProgramController::openScriptsFolder(const luciole::ActionRequest& request) {
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_launch = luciole::CancelToken{};
  const auto token = m_launch;
  ++m_pending;
  luciole::runJob<luciole::Location>(
      token, [token] { return luciole::ensureScriptsDirectory(token); },
      [this, lifetime, request](auto result) {
        if (!lifetime.expired()) {
          --m_pending;
          if (current(request)) {
            if (result) {
              m_page.browser().open(*result);
            } else {
              m_page.actionError(result.error().message);
            }
          }
        }
      }
  );
}
