#include "app/page/rename_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/dialog_dropdown.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "core/ui_phase.h"
#include "fs/filename.h"
#include "fs/gio_ptr.h"
#include "operations/rename_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_error_text.h"
#include "ui/controls/glyph_picker.h"
#include "ui/controls/input.h"
#include "ui/dialogs/name_dialog.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace {
  std::string renameErrorText(const std::optional<luciole::FileError>& error) {
    return error ? luciole::nameErrorText(*error) : std::string{};
  }
} // namespace

RenameController::RenameController(PageContext& page, luciole::PinService& pins) : m_page(page), m_pins(pins) {}

bool RenameController::canFileRename(const luciole::DirectoryEntry& entry) const {
  return m_page.actionState(luciole::BrowserAction::Rename, m_page.requestFor({entry})).enabled;
}

void RenameController::openFileRename(luciole::DirectoryEntry target) {
  if (!canFileRename(target) || !m_page.targetsCurrent({target})) {
    m_page.actionError("This item cannot be renamed right now.");
    return;
  }
  const bool validText = g_utf8_validate(target.name.data(), static_cast<gssize>(target.name.size()), nullptr);
  auto editor = std::make_unique<NameDialog>(
      m_page.dialogWidth(DialogWidth::Small),
      target.type == luciole::FileType::Directory && !target.symlink ? "Rename folder" : "Rename file", "pencil",
      validText ? target.name : "", validText ? "" : "Enter a new UTF-8 name for this item.", luciole::filenameProblem,
      [this](std::string name) { submitFileRename(std::move(name)); }, [this] { m_page.dismissPopup(); }
  );
  auto* control = editor.get();
  m_page.showDialog(std::move(editor), DialogWidth::Small);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::FileRename, [this] {
      if (m_fileRenameTarget) {
        // Renaming goes on for the page that takes the job over once this page left its window.
        if (m_page.jobs().attached() || !m_job->running()) {
          m_job->cancel();
        }
        m_page.focusFileList();
      }
      m_job.reset();
      m_fileRenameEditor = nullptr;
      m_fileRenameTarget.reset();
      m_fileRenamePreparing = false;
    });
    m_fileRenameEditor = control;
    m_fileRenameTarget = std::move(target);
    m_fileRenamePreparing = true;
    control->setSaving(true);
    m_job = m_page.jobs().create<luciole::RenameService>(JobScope::Page);
    if (!m_job->service<luciole::RenameService>().prepare(m_fileRenameTarget->location())) {
      control->showError("Another rename is still finishing.");
    }
  }
}

void RenameController::submitFileRename(std::string name) {
  if (m_fileRenameEditor == nullptr || !m_fileRenameTarget || !m_job) {
    return;
  }
  if (!m_page.targetsCurrent({*m_fileRenameTarget})) {
    m_fileRenameEditor->showError("The item changed. Close this editor and try again.");
    return;
  }
  auto& renamer = m_job->service<luciole::RenameService>();
  if (!renamer.rename(std::move(name))) {
    m_fileRenameEditor->showError(
        renamer.snapshot().error ? luciole::nameErrorText(*renamer.snapshot().error)
                                 : "The item is unavailable. Reopen Rename."
    );
  } else {
    m_page.jobs().follow(*m_job);
    m_fileRenameEditor->setSaving(true);
  }
}

void RenameController::changed(OperationJob& job) {
  if (&job == m_job.get()) {
    m_dirty = true;
  } else if (!job.running() && job.takeResult()) {
    // A job whose editor closed.
    syncFileRename(job);
  }
}

void RenameController::onFrame() {
  if (std::exchange(m_dirty, false) && m_job) {
    syncFileRename(*m_job);
  }
}

void RenameController::syncFileRename(const OperationJob& job) {
  const auto& renamer = job.service<luciole::RenameService>();
  const auto& snapshot = renamer.snapshot();
  if (renamer.busy()) {
    return;
  }
  const bool editor = &job == m_job.get() && m_fileRenameEditor != nullptr;
  if (snapshot.status == luciole::RenameStatus::Succeeded) {
    m_page.setActionMessage(
        job.historyReplay() == HistoryReplay::Undo       ? "Rename undone."
            : job.historyReplay() == HistoryReplay::Redo ? "Rename redone."
                                                         : ""
    );
    auto& browser = m_page.browser();
    const auto& current = browser.model().snapshot().location;
    if (current && snapshot.source && snapshot.source->isInside(*current)) {
      std::vector<luciole::Location> reveal;
      if (snapshot.destination) {
        reveal.push_back(*snapshot.destination);
      }
      m_page.revealOnReload(std::move(reveal));
      browser.refresh();
    }
    if (editor) {
      m_fileRenameEditor = nullptr;
      m_page.dismissPopup();
    }
  } else if (!editor) {
    if (snapshot.status == luciole::RenameStatus::Error) {
      m_page.actionError(
          (job.historyReplay() == HistoryReplay::Undo       ? "Could not undo rename: "
               : job.historyReplay() == HistoryReplay::Redo ? "Could not redo rename: "
                                                            : "Could not rename: ")
          + renameErrorText(snapshot.error)
      );
    }
  } else if (snapshot.status == luciole::RenameStatus::Ready || snapshot.status == luciole::RenameStatus::Error) {
    m_fileRenameEditor->setSaving(false);
    m_fileRenameEditor->showError(renameErrorText(snapshot.error));
    if (std::exchange(m_fileRenamePreparing, false)) {
      m_page.popupDispatcher().setFocus(m_fileRenameEditor->input()->inputArea());
      const auto& value = m_fileRenameEditor->input()->value();
      const bool directory = m_fileRenameTarget->type == luciole::FileType::Directory && !m_fileRenameTarget->symlink;
      m_fileRenameEditor->input()->selectRange(0, luciole::filenameStemLength(value, directory));
    }
    m_page.popupSurface()->requestLayout();
  }
}

void RenameController::openPlaceRename(const luciole::Bookmark& bookmark) {
  if (bookmark.location.isTrash()) {
    return;
  }
  const auto found = std::ranges::find(m_pins.bookmarks(), bookmark.location, &luciole::Bookmark::location);
  if (!m_pins.editable() || found == m_pins.bookmarks().end()) {
    m_page.actionError("This place is unavailable for editing. Reload Places and try again.");
    return;
  }
  const auto current = *found; // NOLINT(performance-unnecessary-copy-initialization): own the editing snapshot.
  auto editor = std::make_unique<NameDialog>(
      m_page.dialogWidth(DialogWidth::Small), "Rename place", "pencil", current.label,
      "Changes the place label without renaming the folder.",
      [](const std::string& label) -> std::optional<std::string> {
        if (const auto error = luciole::bookmarkLabelProblem(label)) {
          return error;
        }
        if (label.find_first_not_of(' ') == std::string::npos) {
          return "Enter a label.";
        }
        return std::nullopt;
      },
      [this](std::string label) { submitPlaceRename(std::move(label)); }, [this] { m_page.dismissPopup(); }
  );
  auto* control = editor.get();
  m_page.showDialog(std::move(editor), DialogWidth::Small);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::PlaceRename, [this] {
      if (m_placeRenameTarget) {
        m_page.restorePlaceFocus(m_placeRenameTarget->location);
      }
      m_placeEditor = nullptr;
      m_placeRenameTarget.reset();
      m_pendingPlaceLabel.reset();
    });
    m_placeEditor = control;
    m_placeRenameTarget = current;
    m_page.popupDispatcher().setFocus(control->input()->inputArea());
    control->input()->selectAll();
  }
}

void RenameController::openPlaceIconPicker(const luciole::Bookmark& bookmark) {
  if (bookmark.location.isTrash() || !m_pins.editable()) {
    return;
  }
  const auto found = std::ranges::find(m_pins.bookmarks(), bookmark.location, &luciole::Bookmark::location);
  if (found == m_pins.bookmarks().end()) {
    return;
  }
  const auto knownPlace = std::ranges::find_if(m_pins.userPlaces(), [&](const auto& place) {
    return place.bookmark.location == found->location;
  });
  const std::string initialGlyph = !found->iconName.empty() ? found->iconName
      : knownPlace != m_pins.userPlaces().end()             ? knownPlace->glyphName
                                                            : "folder";
  auto picker = std::make_unique<GlyphPicker>([this] {
    if (auto* popup = m_page.popupSurface()) {
      popup->requestUpdate();
    }
  });
  picker->setTitle("Choose bookmark icon");
  picker->setInitialGlyph(initialGlyph);
  auto* control = picker.get();
  picker->setOnCancel([this] { m_page.dismissPopup(); });
  picker->setOnApply([this](const GlyphPickerResult& result) { submitPlaceIcon(result.name); });
  picker->setOnReset([this] { submitPlaceIcon({}); });
  picker->setPopupContext(&m_page.dialogDropdown());
  m_page.showDialog(std::move(picker), DialogWidth::Large);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::PlaceIcon, [this, location = found->location] {
      m_placeIconPicker = nullptr;
      m_placeIconTarget.reset();
      m_pendingPlaceIcon.reset();
      if (m_page.visible()) {
        m_page.restorePlaceFocus(location);
      }
    });
    m_placeIconPicker = control;
    m_placeIconTarget = *found;
    m_page.popupDispatcher().setFocus(control->initialFocusArea());
  }
}

void RenameController::submitPlaceIcon(std::string iconName) {
  if (m_placeIconPicker == nullptr || !m_placeIconTarget) {
    return;
  }
  if (m_placeIconTarget->iconName == iconName) {
    m_page.dismissPopup();
    return;
  }
  m_pendingPlaceIcon = std::move(iconName);
  m_placeIconPicker->setEnabled(false);
  if (!m_pins.setIcon(*m_placeIconTarget, *m_pendingPlaceIcon)) {
    m_pendingPlaceIcon.reset();
    m_placeIconPicker->setEnabled(true);
    const bool targetExists = !m_pins.busy() && refreshPlaceIconTarget();
    const std::string error = m_pins.busy() ? "Places are being saved. Try again shortly."
        : targetExists                      ? "This place changed. Choose an icon again."
                                            : "This place is no longer available. Close this dialog and try again.";
    m_placeIconPicker->showError(error);
    m_page.actionError(error);
  }
}

bool RenameController::refreshPlaceIconTarget() {
  if (!m_placeIconTarget) {
    return false;
  }
  const auto found = std::ranges::find(m_pins.bookmarks(), m_placeIconTarget->location, &luciole::Bookmark::location);
  if (found == m_pins.bookmarks().end()) {
    return false;
  }
  m_placeIconTarget = *found;
  return true;
}

void RenameController::placeIconPointer(float x, float y, bool hover) {
  if (m_placeIconPicker != nullptr) {
    m_placeIconPicker->tooltipPointer(x, y, hover);
  }
}

void RenameController::syncPlaceIconTooltip(Renderer& renderer) {
  if (m_placeIconPicker != nullptr) {
    m_placeIconPicker->syncTooltip(renderer);
  }
}

void RenameController::submitPlaceRename(std::string label) {
  if (m_placeEditor == nullptr || !m_placeRenameTarget) {
    return;
  }
  m_pendingPlaceLabel = label;
  m_placeEditor->setSaving(true);
  if (!m_pins.rename(*m_placeRenameTarget, std::move(label))) {
    m_pendingPlaceLabel.reset();
    m_placeEditor->setSaving(false);
    m_placeEditor->showError(
        m_pins.busy() ? "Places are being saved. Try again shortly."
                      : "This place changed. Close this dialog and try again."
    );
    return;
  }
  syncPlaceEditors();
}

void RenameController::syncPlaceEditors() {
  if (!m_pins.busy()) {
    if (m_placeEditor != nullptr && m_pendingPlaceLabel && m_placeRenameTarget) {
      const auto found =
          std::ranges::find(m_pins.bookmarks(), m_placeRenameTarget->location, &luciole::Bookmark::location);
      if (found != m_pins.bookmarks().end() && found->label == *m_pendingPlaceLabel) {
        m_page.dismissPopup();
      } else {
        m_placeEditor->setSaving(false);
        m_placeEditor->showError(
            m_pins.error().empty() ? "This place changed. Close this dialog and try again."
                                   : "Could not save the label. Close this dialog and try again."
        );
      }
      m_pendingPlaceLabel.reset();
    }
    if (m_placeIconPicker != nullptr && m_pendingPlaceIcon && m_placeIconTarget) {
      const auto found =
          std::ranges::find(m_pins.bookmarks(), m_placeIconTarget->location, &luciole::Bookmark::location);
      if (found != m_pins.bookmarks().end() && found->iconName == *m_pendingPlaceIcon) {
        m_page.dismissPopup();
      } else {
        const bool targetExists = refreshPlaceIconTarget();
        m_placeIconPicker->setEnabled(true);
        const std::string error = !targetExists ? "This place is no longer available. Close this dialog and try again."
            : m_pins.error().empty()            ? "This place changed. Try choosing an icon again."
                                                : "Could not save the bookmark icon. Try again.";
        m_placeIconPicker->showError(error);
        m_page.actionError(error);
      }
      m_pendingPlaceIcon.reset();
    }
  }
}
