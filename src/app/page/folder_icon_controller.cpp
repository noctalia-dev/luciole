#include "app/page/folder_icon_controller.h"

#include "app/page/active_popup.h"
#include "app/page/dialog_dropdown.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "config/keybinds.h"
#include "fs/background_job.h"
#include "fs/folder_icon.h"
#include "i18n/i18n.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/controls/icon_picker.h"
#include "ui/dialogs/dialog_chrome.h"
#include "wayland/wayland_seat.h"

namespace {
  bool eligibleFolder(const luciole::Location& folder) { return folder.nativePath().has_value() && !folder.isTrash(); }
} // namespace

FolderIconController::FolderIconController(PageContext& page, luciole::FileIconCache& icons)
    : m_page(page), m_icons(icons) {}

void FolderIconController::open(const luciole::Location& folder, std::function<void()> onReturn) {
  if (!eligibleFolder(folder)) {
    return;
  }
  if (m_picker != nullptr) {
    m_page.dismissPopup();
  }
  const std::uint64_t generation = ++m_generation;
  m_target = folder;
  m_returnCallback = std::move(onReturn);
  m_expectingDialog = true;
  luciole::runJob<LoadResult>(
      [folder] {
        return LoadResult{
            .names = IconResolver::listThemeIcons(),
            .current = luciole::readFolderCustomIcon(folder),
        };
      },
      [this, generation](luciole::JobResult<LoadResult> reply) {
        if (!reply || generation != m_generation || !m_expectingDialog) {
          return;
        }
        showPicker(std::move(*reply), generation);
      }
  );
}

void FolderIconController::showPicker(LoadResult result, std::uint64_t generation) {
  if (!m_target || generation != m_generation) {
    return;
  }
  auto picker = std::make_unique<IconPicker>(m_icons, std::move(result.names));
  picker->setTitle(i18n::text("Choose Folder Icon"));
  picker->setInitialIcon(std::move(result.current));
  auto* control = picker.get();
  picker->setOnCancel([this] { cancelPicker(); });
  picker->setOnApply([this](const IconPickerResult& picked) { submitIcon(picked.name); });
  picker->setOnReset([this] { resetIcon(); });
  picker->setPopupContext(&m_page.dialogDropdown());
  m_page.showDialog(std::move(picker), DialogWidth::Large);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::FolderIcon, [this] {
      m_picker = nullptr;
      m_target.reset();
      m_expectingDialog = false;
    });
    m_picker = control;
    m_page.popupDispatcher().setFocus(control->initialFocusArea());
  } else {
    m_expectingDialog = false;
  }
}

void FolderIconController::submitIcon(std::string iconName) {
  if (m_picker == nullptr || !m_target || iconName.empty()) {
    return;
  }
  m_picker->setEnabled(false);
  const auto folder = *m_target;
  luciole::runJob<WriteResult>(
      [folder, iconName = std::move(iconName)] {
        WriteResult result;
        result.ok = luciole::setFolderCustomIcon(folder, iconName, result.error);
        return result;
      },
      [this](luciole::JobResult<WriteResult> reply) { finishWrite(reply ? *reply : WriteResult{}); }
  );
}

void FolderIconController::resetIcon() {
  if (m_picker == nullptr || !m_target) {
    return;
  }
  m_picker->setEnabled(false);
  const auto folder = *m_target;
  luciole::runJob<WriteResult>(
      [folder] {
        WriteResult result;
        result.ok = luciole::clearFolderCustomIcon(folder, result.error);
        return result;
      },
      [this](luciole::JobResult<WriteResult> reply) { finishWrite(reply ? *reply : WriteResult{}); }
  );
}

void FolderIconController::finishWrite(const WriteResult& result) {
  if (m_picker == nullptr || !m_target) {
    return;
  }
  if (!result.ok) {
    m_picker->setEnabled(true);
    const std::string error = result.error.empty() ? "Could not change the folder icon. Try again." : result.error;
    m_picker->showError(error);
    m_page.actionError(error);
    return;
  }
  const auto folder = *m_target;
  auto returnTo = std::move(m_returnCallback);
  m_returnCallback = nullptr;
  m_icons.invalidate();
  if (returnTo) {
    // Back to the caller (Properties): the dialog re-show replaces the picker,
    // and the change hook below rebinds its header to the fresh icon.
    returnTo();
    if (m_picker != nullptr) {
      // The re-show refused (a Properties request is still finishing): leave
      // the picker usable instead of stuck disabled.
      m_picker->setEnabled(true);
    }
  } else {
    m_page.dismissPopup();
  }
  m_page.browser().refresh();
  if (m_onChanged) {
    m_onChanged(folder);
  }
}

void FolderIconController::cancelPicker() {
  if (m_picker == nullptr) {
    return;
  }
  if (m_returnCallback) {
    // The caller re-shows its own dialog (Properties), replacing the picker.
    auto returnTo = std::move(m_returnCallback);
    m_returnCallback = nullptr;
    returnTo();
  } else {
    m_page.dismissPopup();
  }
}

bool FolderIconController::escapeKey(const KeyboardEvent& event) {
  if (m_picker == nullptr
      || !event.pressed
      || !luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers)) {
    return false;
  }
  cancelPicker();
  return true;
}
