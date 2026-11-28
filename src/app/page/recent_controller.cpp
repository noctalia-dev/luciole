#include "app/page/recent_controller.h"

#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "fs/recent_files.h"
#include "ui/builders.h"
#include "ui/dialogs/dialog_chrome.h"
#include "wayland/popup_surface.h"

RecentController::RecentController(PageContext& page, luciole::RecentFiles& recent, bool remember)
    : m_page(page), m_recent(recent), m_remember(remember) {}

bool RecentController::busy() const { return m_recent.busy(); }

void RecentController::remove(const std::vector<luciole::DirectoryEntry>& items) {
  std::vector<luciole::Location> files;
  files.reserve(items.size());
  for (const auto& item : items) {
    if (const auto* target = item.recentTarget()) {
      files.push_back(*target);
    }
  }
  m_recent.forget(files, [this, lifetime = std::weak_ptr(m_lifetime)](const std::string& error) {
    if (!lifetime.expired()) {
      finished(error);
    }
  });
}

void RecentController::openClear() {
  m_page.cancelDrag();
  const float width = m_page.dialogWidth(DialogWidth::Small);
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("Clear Recent?", "clear-all", [this] { m_page.dismissPopup(); }),
      ui::label(
          {.text = "Every application's list of recently used files is cleared. The files themselves stay.",
           .maxWidth = width - 24.0F,
           .maxLines = 4}
      ),
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.text = "Cancel", .variant = ButtonVariant::Default, .onClick = [this] { m_page.dismissPopup(); }}
          ),
          ui::button({.text = "Clear", .variant = ButtonVariant::Destructive, .onClick = [this] {
                        m_page.dismissPopup();
                        m_recent.clear([this, lifetime = std::weak_ptr(m_lifetime)](const std::string& error) {
                          if (!lifetime.expired()) {
                            finished(error);
                          }
                        });
                      }})
      )
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Small);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::ClearRecent, [] {});
  }
}

void RecentController::recordUse(const luciole::Location& file, std::string contentType) {
  if (m_remember) {
    m_recent.recordUse(file, std::move(contentType));
  }
}

// Recent's own monitor shows the change too; refreshing covers providers that cannot report it.
void RecentController::finished(const std::string& error) {
  if (!error.empty()) {
    m_page.actionError(error);
  }
  const auto& location = m_page.browser().model().snapshot().location;
  if (location && location->isRecent()) {
    m_page.browser().refresh();
  }
}
