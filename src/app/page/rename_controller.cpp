#include "app/page/rename_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/dialog_dropdown.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "browser/selection_tools.h"
#include "config/keybinds.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "fs/filename.h"
#include "fs/gio_ptr.h"
#include "operations/rename_service.h"
#include "platform/desktop_integration.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph_picker.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/name_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace {
  // Matches Input's internal kTextInnerInset so frameless inline text aligns with the underlying row/tile label.
  constexpr float kInputTextInnerInset = 3.0F;
  constexpr float kInlineMinHeight = 26.0F;
  constexpr float kInlineMinWidth = 48.0F;
  constexpr float kInlineErrorGap = 2.0F;
  constexpr float kInlineErrorBarWidth = 2.0F;
  constexpr float kInlineErrorPaddingV = 3.0F;
  constexpr float kInlineErrorPaddingTrailing = 8.0F;
  constexpr float kInlineErrorEdgeMargin = 8.0F;
  constexpr float kInlineErrorMinWidth = 120.0F;
  constexpr float kInlineErrorMaxWidth = 380.0F;

  std::string renameErrorText(const std::optional<luciole::FileError>& error) {
    return error ? luciole::nameErrorText(*error) : std::string{};
  }
} // namespace

RenameController::RenameController(PageContext& page, luciole::PinService& pins) : m_page(page), m_pins(pins) {}

void RenameController::buildInlineEditor(Node& filePane) {
  auto clip = std::make_unique<Node>();
  clip->setClipChildren(true);
  clip->setParticipatesInLayout(false);
  clip->setVisible(false);
  clip->setZIndex(20);
  clip->addChild(
      ui::input(
          {.out = &m_inlineInput,
           .fontSize = Style::fontSizeBody,
           .controlHeight = kInlineMinHeight,
           .horizontalPadding = 0.0F,
           .frameVisible = false,
           .onChange =
               [this](const std::string&) {
                 if (!editingFile() || m_fileRenameSubmitting) {
                   return;
                 }
                 clearInlineError();
                 if (m_inlineClipped) {
                   ensureTargetVisible();
                 }
               },
           .onSubmit =
               [this](const std::string& name) {
                 if (editingFile() && m_inlineInput != nullptr && !m_inlineInput->composing()) {
                   submitInlineRename(name, std::nullopt);
                 }
               },
           .onFocusLoss = [this] { handleInlineFocusLoss(); },
           .configure =
               [this](Input& input) {
                 input.inputArea()->setTabStop(false);
                 input.setOnCompositionChange([this](bool composing) {
                   if (!composing) {
                     m_xkbPreedit = false;
                   }
                 });
               }}
      )
  );
  m_inlineClip = filePane.addChild(std::move(clip));

  auto banner = ui::row(
      {.out = &m_inlineErrorBanner,
       .align = FlexAlign::Stretch,
       .gap = Style::spaceXs,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .visible = false,
       .participatesInLayout = false},
      ui::box({.out = &m_inlineErrorBar, .fill = colorSpecFromRole(ColorRole::Error), .width = kInlineErrorBarWidth}),
      ui::label(
          {.out = &m_inlineErrorLabel,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::Error),
           .maxLines = 2,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      )
  );
  banner->setHitTestVisible(false);
  banner->setZIndex(21);
  filePane.addChild(std::move(banner));
}

bool RenameController::inlineContains(float x, float y) const {
  return editingFile()
      && m_inlineClip != nullptr
      && m_inlineClip->visible()
      && m_inlineClip->width() > 0.0F
      && m_inlineClip->height() > 0.0F
      && m_inlineClip->containsScenePoint(x, y);
}

TextInputClient* RenameController::focusedTextInputClient() const {
  if (!editingFile() || m_inlineInput == nullptr || m_page.dispatcher().focusedArea() != m_inlineInput->inputArea()) {
    return nullptr;
  }
  return m_inlineInput;
}

bool RenameController::targetStillCurrent(const luciole::DirectoryEntry& target) const {
  const auto& model = m_page.browser().model();
  if (!model.rowOf(target.id)) {
    return false;
  }
  const auto location = model.location(target.id);
  return location && *location == target.location();
}

bool RenameController::resolveActiveTarget() {
  if (!editingFile()) {
    return false;
  }
  const auto& model = m_page.browser().model();
  if (targetStillCurrent(*m_fileRenameTarget)) {
    return true;
  }
  const auto generation = model.snapshot().generation;
  if (m_targetScannedGeneration == generation) {
    return false;
  }
  m_targetScannedGeneration = generation;
  // A refresh may reassign EntryIds while keeping the same file visible at its location.
  const auto found = std::ranges::find_if(model.rows(), [&](luciole::EntryId id) {
    const auto location = model.location(id);
    return location && *location == m_fileRenameTarget->location();
  });
  if (found != model.rows().end()) {
    m_fileRenameTarget->id = *found;
    m_targetScannedGeneration.reset();
    return true;
  }
  return false;
}

void RenameController::ensureTargetVisible() {
  if (!m_fileRenameTarget) {
    return;
  }
  const auto index = m_page.browser().model().rowOf(m_fileRenameTarget->id);
  if (index && m_page.fileList() != nullptr) {
    m_page.fileList()->scrollToIndex(*index);
  }
  if (m_page.viewMode() == luciole::ViewMode::Miller) {
    m_page.millerView().revealFocused();
  }
  m_page.ensureNameColumnVisible();
}

void RenameController::applyPendingReveal() {
  if (!std::exchange(m_revealPending, false) || !editingFile()) {
    return;
  }
  ensureTargetVisible();
}

void RenameController::cancelPendingResume() { m_resumeNextTarget.reset(); }

void RenameController::openFileRename(luciole::DirectoryEntry target) {
  if (m_inlineInput == nullptr || m_inlineClip == nullptr) {
    return;
  }
  m_resumeNextTarget.reset();
  if (!m_page.active() || m_page.overlay() || (m_page.popupSurface() != nullptr && m_page.contextMenu() == nullptr)) {
    return;
  }
  if (!m_page.canInlineRename(target, nullptr) || !m_page.targetsCurrent({target})) {
    m_page.actionError("This item cannot be renamed right now.");
    return;
  }
  beginInlineRename(std::move(target));
}

void RenameController::beginInlineRename(luciole::DirectoryEntry target) {
  if (m_inlineInput == nullptr || m_inlineClip == nullptr) {
    return;
  }
  releaseInlineJob();
  m_resumeNextTarget.reset();

  auto& model = m_page.browser().model();
  if (model.selection().selected.size() != 1 || !model.selection().selected.contains(target.id)) {
    (void)model.select(target.id);
  }
  const bool validText = g_utf8_validate(target.name.data(), static_cast<gssize>(target.name.size()), nullptr);
  m_fileRenameTarget = std::move(target);
  m_fileRenameValidUtf8 = validText;
  m_fileRenameSubmitting = false;
  m_pendingSubmitName.reset();
  m_pendingNextTarget.reset();
  m_inlineHint = validText ? "" : "Enter a new UTF-8 name for this item.";
  m_inlineError.clear();
  m_inlineRowIndex = model.rowOf(m_fileRenameTarget->id);
  m_targetScannedGeneration.reset();
  m_xkbPreedit = false;
  m_revealPending = true;
  m_inlineClipped = false;

  const bool grid = m_page.viewMode() == luciole::ViewMode::Grid;
  m_inlineInput->setTextAlign(grid ? TextAlign::Center : TextAlign::Start);
  m_inlineInput->setPlaceholder(validText ? "" : "Name");
  m_inlineInput->setValue(validText ? m_fileRenameTarget->name : "");
  m_inlineInput->setEnabled(true);
  m_inlineInput->setInvalid(false);
  m_inlineClip->setVisible(true);
  syncInlineErrorVisual();

  m_page.setRenamingEntry(m_fileRenameTarget->id, true);
  m_page.dispatcher().setFocus(m_inlineInput->inputArea());
  const bool directory = m_fileRenameTarget->type == luciole::FileType::Directory && !m_fileRenameTarget->symlink;
  const auto& value = m_inlineInput->value();
  m_inlineInput->selectRange(0, luciole::filenameStemLength(value, directory));

  m_job = m_page.jobs().create<luciole::RenameService>(JobScope::Page);
  if (!m_job->service<luciole::RenameService>().prepare(m_fileRenameTarget->location())) {
    showInlineError("Another rename is still finishing.");
  }
  m_page.requestSync();
}

void RenameController::notePreeditKey() {
  if (editingFile() && m_inlineInput != nullptr && m_page.dispatcher().focusedArea() == m_inlineInput->inputArea()) {
    m_xkbPreedit = true;
  }
}

bool RenameController::key(const KeyboardEvent& event) {
  if (!event.pressed) {
    return false;
  }
  const bool cancel = luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers);
  if (!editingFile()) {
    if (m_resumeNextTarget && cancel) {
      m_resumeNextTarget.reset();
      return true;
    }
    return false;
  }
  if (m_inlineInput == nullptr || m_page.dispatcher().focusedArea() != m_inlineInput->inputArea()) {
    return false;
  }
  if (m_inlineInput->composing()) {
    if (cancel) {
      m_inlineInput->textInputResetPreedit();
      m_page.notifyTextInputChanged(*m_inlineInput);
      return true;
    }
    if (!m_xkbPreedit) {
      // Keep raw Validate/Enter from stripping a Wayland IME preedit and submitting an incomplete name;
      // explicit IME commits and submissions arrive through TextInputClient::textInputApplyEdit().
      return luciole::keybindMatches(luciole::KeybindCommand::Validate, event.sym, event.modifiers)
          || KeySymbol::isEnter(event.sym);
    }
    if (KeySymbol::isModifier(event.sym)) {
      return false;
    }
    // WaylandSeat already completed or cancelled XKB compose before sending this non-preedit key.
    m_inlineInput->textInputResetPreedit();
    m_page.notifyTextInputChanged(*m_inlineInput);
  }
  if (cancel) {
    cancelFileRename(true);
    return true;
  }
  const bool nonPrintableChord =
      event.utf32 == 0 || (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0;
  if (nonPrintableChord && luciole::keybindMatches(luciole::KeybindCommand::Rename, event.sym, event.modifiers)) {
    if (!event.repeat && !m_fileRenameSubmitting) {
      const auto& value = m_inlineInput->value();
      const auto state = m_inlineInput->textInputState();
      const auto start = static_cast<std::size_t>(std::max(0, std::min(state.cursor, state.anchor)));
      const auto end = static_cast<std::size_t>(std::max({0, state.cursor, state.anchor}));
      const bool directory = m_fileRenameTarget->type == luciole::FileType::Directory && !m_fileRenameTarget->symlink;
      m_inlineInput->selectRange(0, luciole::nextRenameSelectionEnd(value, directory, start, end));
      if (m_inlineClipped) {
        ensureTargetVisible();
      }
      m_page.surface().requestRedraw();
    }
    return true;
  }
  if (event.modifiers == 0 && (KeySymbol::isUp(event.sym) || KeySymbol::isDown(event.sym))) {
    navigateInlineRename(KeySymbol::isDown(event.sym) ? 1 : -1, true);
    return true;
  }
  return m_fileRenameSubmitting;
}

bool RenameController::tabKey(bool reverse) {
  if (!editingFile() || m_inlineInput == nullptr || m_page.dispatcher().focusedArea() != m_inlineInput->inputArea()) {
    return false;
  }
  if (m_fileRenameSubmitting) {
    return true;
  }
  if (m_inlineInput->composing()) {
    if (!m_xkbPreedit) {
      return true;
    }
    // XKB compose already cancelled on Tab; clear the stale preedit and step cleanly.
    m_inlineInput->textInputResetPreedit();
    m_page.notifyTextInputChanged(*m_inlineInput);
  }
  navigateInlineRename(reverse ? -1 : 1, false);
  return true;
}

std::optional<luciole::EntryId> RenameController::findAdjacentTarget(
    std::size_t rowIndex, int direction, std::size_t stride, const OperationJob* ignoreJob
) const {
  const auto& model = m_page.browser().model();
  // Inspect only already-loaded facts so stepping does not trigger metadata requests across a large catalogue.
  return luciole::adjacentRenameTarget(
      model.rows(), rowIndex, direction, stride, [this, &model, ignoreJob](luciole::EntryId id) {
        if (const auto* known = model.knownEntry(id); known != nullptr && !known->metadataPending) {
          return m_page.canInlineRename(*known, ignoreJob);
        }
        const auto location = model.location(id);
        return location && (location->nativePath().has_value() || location->isAdmin());
      }
  );
}

void RenameController::navigateInlineRename(int direction, bool byRow) {
  if (!editingFile() || m_fileRenameSubmitting || m_inlineInput == nullptr || m_inlineInput->composing()) {
    return;
  }
  if (!m_page.canInlineRenameInCurrentFolder(m_job.get())) {
    return;
  }
  auto& model = m_page.browser().model();
  const auto currentRow = model.rowOf(m_fileRenameTarget->id);
  if (!currentRow) {
    return;
  }
  const auto stride = byRow && m_page.fileList() != nullptr
      ? std::max<std::size_t>(1, m_page.fileList()->layoutColumnCount())
      : std::size_t{1};
  const auto nextId = findAdjacentTarget(*currentRow, direction, stride, m_job.get());
  if (!nextId) {
    return;
  }
  const auto nextLocation = model.location(*nextId);
  if (!nextLocation) {
    return;
  }
  const std::string& name = m_inlineInput->value();
  if (m_fileRenameValidUtf8 && name == m_fileRenameTarget->name) {
    if (const auto* known = model.knownEntry(*nextId); known != nullptr && !known->metadataPending) {
      auto nextCopy = *known;
      beginInlineRename(std::move(nextCopy));
    } else {
      // Cold catalogue entry: request metadata for only this candidate and open inline rename once it resolves.
      (void)model.select(*nextId);
      model.requestDetails(*nextId, 0);
      const auto retiring = m_job;
      m_resumeNextTarget = PendingResume{
          .id = *nextId,
          .location = *nextLocation,
          .contextSerial = m_page.browser().contextSerial(),
          .direction = direction,
          .stride = stride,
          .retiringJob = retiring
      };
      releaseInlineJob();
      closeInlineUi(true, true);
    }
    return;
  }
  submitInlineRename(
      name, PendingNextTarget{.id = *nextId, .location = *nextLocation, .direction = direction, .stride = stride}
  );
}

void RenameController::resumeSteppingFrom(std::size_t rowIndex, int direction, std::size_t stride) {
  if (direction == 0 || !m_page.canInlineRenameInCurrentFolder(nullptr)) {
    return;
  }
  auto& model = m_page.browser().model();
  const auto nextId = findAdjacentTarget(rowIndex, direction, stride, nullptr);
  if (!nextId) {
    return;
  }
  if (const auto* known = model.knownEntry(*nextId); known != nullptr && !known->metadataPending) {
    auto nextCopy = *known;
    beginInlineRename(std::move(nextCopy));
    return;
  }
  if (const auto nextLocation = model.location(*nextId)) {
    (void)model.select(*nextId);
    model.requestDetails(*nextId, 0);
    m_resumeNextTarget = PendingResume{
        .id = *nextId,
        .location = *nextLocation,
        .contextSerial = m_page.browser().contextSerial(),
        .direction = direction,
        .stride = stride,
        .retiringJob = {}
    };
  }
}

void RenameController::submitInlineRename(std::string name, std::optional<PendingNextTarget> nextTarget) {
  if (!editingFile() || m_inlineInput == nullptr || !m_job || m_fileRenameSubmitting) {
    return;
  }
  if (m_fileRenameValidUtf8 && name == m_fileRenameTarget->name && !nextTarget) {
    cancelFileRename(true);
    return;
  }
  if (const auto error = luciole::filenameProblem(name)) {
    showInlineError(*error);
    return;
  }
  const auto previousId = m_fileRenameTarget->id;
  if (!resolveActiveTarget()) {
    m_page.actionError("The item changed.");
    cancelFileRename(true);
    return;
  }
  if (m_fileRenameTarget->id != previousId) {
    m_page.setRenamingEntry(m_fileRenameTarget->id);
  }
  auto& renamer = m_job->service<luciole::RenameService>();
  if (renamer.busy()) {
    m_pendingSubmitName = std::move(name);
    m_pendingNextTarget = std::move(nextTarget);
    m_fileRenameSubmitting = true;
    m_inlineInput->setEnabled(false);
    clearInlineError();
    return;
  }
  if (!renamer.rename(std::move(name))) {
    showInlineError(
        renamer.snapshot().error ? luciole::nameErrorText(*renamer.snapshot().error)
                                 : "The item is unavailable. Reopen Rename."
    );
    return;
  }
  m_pendingNextTarget = std::move(nextTarget);
  m_fileRenameSubmitting = true;
  m_inlineInput->setEnabled(false);
  clearInlineError();
  m_page.jobs().follow(*m_job);
}

bool RenameController::detachPendingRename(std::string name) {
  if (!m_job) {
    return false;
  }
  auto job = std::move(m_job);
  auto& renamer = job->service<luciole::RenameService>();
  if (renamer.canRename()) {
    if (!renamer.rename(std::move(name))) {
      m_job = std::move(job);
      return false;
    }
    m_page.jobs().follow(*job);
    m_dirty = false;
    closeInlineUi(false);
    return true;
  }
  if (renamer.busy() && renamer.snapshot().status == luciole::RenameStatus::Preparing) {
    m_page.jobs().follow(*job);
    job->whenIdle([pendingName = std::move(name)](OperationJob& done) {
      auto& service = done.service<luciole::RenameService>();
      if (service.canRename()) {
        (void)service.rename(pendingName);
      }
    });
    m_dirty = false;
    closeInlineUi(false);
    return true;
  }
  if (!job->running() && job->takeResult()) {
    m_dirty = false;
    closeInlineUi(false);
    syncFileRename(*job);
    return true;
  }
  m_job = std::move(job);
  return false;
}

void RenameController::handleInlineFocusLoss() {
  if (!editingFile() || m_closingInline) {
    return;
  }
  commitOrCancelInline();
}

void RenameController::commitOrCancelInline() {
  m_resumeNextTarget.reset();
  if (!editingFile() || m_closingInline) {
    return;
  }
  if (m_fileRenameSubmitting) {
    m_pendingNextTarget.reset();
    if (auto pending = std::exchange(m_pendingSubmitName, std::nullopt)) {
      if (!resolveActiveTarget() || !detachPendingRename(std::move(*pending))) {
        cancelFileRename(false);
      }
      return;
    }
    // Mutation is already running (or just finished before onFrame()); close UI and flush any completed result.
    auto job = std::move(m_job);
    m_dirty = false;
    closeInlineUi(false);
    if (job && !job->running() && job->takeResult()) {
      syncFileRename(*job);
    }
    return;
  }
  if (m_inlineInput != nullptr && m_inlineInput->composing()) {
    m_inlineInput->textInputResetPreedit();
    m_page.notifyTextInputChanged(*m_inlineInput);
  }
  const std::string name = m_inlineInput != nullptr ? m_inlineInput->value() : std::string{};
  const bool unchanged = m_fileRenameValidUtf8 && name == m_fileRenameTarget->name;
  if (!unchanged && !luciole::filenameProblem(name) && resolveActiveTarget()) {
    if (detachPendingRename(name)) {
      return;
    }
  }
  cancelFileRename(false);
}

void RenameController::cancelFileRename(bool restoreFocus) {
  m_resumeNextTarget.reset();
  if (!editingFile()) {
    return;
  }
  // If a submitted mutation already finished before onFrame(), flush its refresh/error instead of dropping it.
  if (m_fileRenameSubmitting && !m_pendingSubmitName && m_job && !m_job->running() && m_job->takeResult()) {
    auto job = std::move(m_job);
    m_dirty = false;
    closeInlineUi(restoreFocus);
    syncFileRename(*job);
    return;
  }
  releaseInlineJob();
  closeInlineUi(restoreFocus);
}

void RenameController::releaseInlineJob() {
  if (!m_job) {
    return;
  }
  // A followed rename keeps running for the handoff page once this page left its window.
  if (m_page.jobs().attached() || !m_job->running()) {
    m_job->cancel();
  }
  m_job.reset();
  m_dirty = false;
}

void RenameController::closeInlineUi(bool restoreFocus, bool preserveResume) {
  const bool wasClosing = std::exchange(m_closingInline, true);
  m_fileRenameTarget.reset();
  m_inlineRowIndex.reset();
  m_targetScannedGeneration.reset();
  m_pendingSubmitName.reset();
  m_pendingNextTarget.reset();
  if (!preserveResume) {
    m_resumeNextTarget.reset();
  }
  m_inlineHint.clear();
  m_inlineError.clear();
  m_fileRenameValidUtf8 = true;
  m_fileRenameSubmitting = false;
  m_xkbPreedit = false;
  m_revealPending = false;
  m_inlineClipped = false;
  m_page.setRenamingEntry(std::nullopt);
  if (m_inlineInput != nullptr) {
    m_inlineInput->setInvalid(false);
    m_inlineInput->setEnabled(true);
  }
  if (m_inlineClip != nullptr) {
    m_inlineClip->setVisible(false);
  }
  if (m_inlineErrorBanner != nullptr) {
    m_inlineErrorBanner->setVisible(false);
  }
  if (restoreFocus) {
    m_page.focusFileList();
  } else if (m_inlineInput != nullptr && m_page.dispatcher().focusedArea() == m_inlineInput->inputArea()) {
    m_page.dispatcher().setFocus(nullptr);
  }
  m_closingInline = wasClosing;
  m_page.requestSync();
}

void RenameController::showInlineError(std::string error) {
  m_inlineError = std::move(error);
  syncInlineErrorVisual();
  m_page.surface().requestLayout();
}

void RenameController::clearInlineError() {
  if (m_inlineError.empty()) {
    return;
  }
  m_inlineError.clear();
  syncInlineErrorVisual();
  m_page.surface().requestLayout();
}

void RenameController::syncInlineErrorVisual() {
  if (m_inlineInput == nullptr
      || m_inlineErrorBanner == nullptr
      || m_inlineErrorBar == nullptr
      || m_inlineErrorLabel == nullptr) {
    return;
  }
  const bool hasError = !m_inlineError.empty();
  const std::string& text = hasError ? m_inlineError : m_inlineHint;
  m_inlineInput->setInvalid(hasError);
  const auto role = hasError ? ColorRole::Error : ColorRole::OnSurfaceVariant;
  m_inlineErrorBar->setFill(colorSpecFromRole(role));
  m_inlineErrorLabel->setColor(colorSpecFromRole(role));
  m_inlineErrorLabel->setText(text);
  m_inlineErrorBanner->setVisible(editingFile() && !text.empty());
}

void RenameController::advancePendingResume() {
  if (!m_resumeNextTarget) {
    return;
  }
  auto& browser = m_page.browser();
  auto& model = browser.model();
  const auto& snapshot = model.snapshot();
  if (m_resumeNextTarget->contextSerial != browser.contextSerial()
      || !m_page.active()
      || m_page.popupSurface() != nullptr
      || m_page.overlay()) {
    m_resumeNextTarget.reset();
    return;
  }
  std::optional<luciole::EntryId> matched;
  if (model.rowOf(m_resumeNextTarget->id)) {
    if (const auto location = model.location(m_resumeNextTarget->id);
        location && *location == m_resumeNextTarget->location) {
      matched = m_resumeNextTarget->id;
    }
  }
  if (!matched && m_resumeNextTarget->scannedGeneration != snapshot.generation) {
    m_resumeNextTarget->scannedGeneration = snapshot.generation;
    for (const auto id : model.rows()) {
      if (const auto location = model.location(id); location && *location == m_resumeNextTarget->location) {
        matched = id;
        m_resumeNextTarget->id = id;
        break;
      }
    }
  }
  if (matched) {
    const auto* known = model.knownEntry(*matched);
    if (known == nullptr || known->metadataPending) {
      if (!model.selection().selected.contains(*matched)) {
        (void)model.select(*matched);
      }
      model.requestDetails(*matched, 0);
      return;
    }
    if (const auto retiring = m_resumeNextTarget->retiringJob.lock(); retiring && retiring->pending()) {
      return;
    }
    const auto resume = *std::exchange(m_resumeNextTarget, std::nullopt);
    if (m_page.canInlineRename(*known, nullptr)) {
      auto copy = *known;
      beginInlineRename(std::move(copy));
    } else if (const auto row = model.rowOf(*matched)) {
      resumeSteppingFrom(*row, resume.direction, resume.stride);
    }
    return;
  }
  if (snapshot.status != luciole::DirectoryStatus::Loading) {
    m_resumeNextTarget.reset();
  }
}

void RenameController::syncInlineTarget() {
  if (!editingFile()) {
    advancePendingResume();
    return;
  }
  auto& model = m_page.browser().model();
  const auto& snapshot = model.snapshot();
  const auto previousId = m_fileRenameTarget->id;
  if (resolveActiveTarget()) {
    const auto id = m_fileRenameTarget->id;
    if (id != previousId && !model.selection().selected.contains(id)) {
      (void)model.select(id);
    }
    m_page.setRenamingEntry(id);
    const auto row = model.rowOf(id);
    if (id != previousId || row != m_inlineRowIndex) {
      m_inlineRowIndex = row;
      m_revealPending = true;
    }
    return;
  }
  if (m_fileRenameSubmitting || (snapshot.status == luciole::DirectoryStatus::Loading && snapshot.incrementalResults)) {
    // Wait without hiding an unrelated entry that reused the numeric EntryId.
    m_page.setRenamingEntry(std::nullopt);
    return;
  }
  cancelFileRename(true);
}

void RenameController::layoutInline(Renderer& renderer) {
  if (m_inlineClip == nullptr || m_inlineInput == nullptr || m_inlineErrorBanner == nullptr) {
    return;
  }
  if (!editingFile()) {
    m_inlineClipped = false;
    m_inlineClip->setVisible(false);
    m_inlineErrorBanner->setVisible(false);
    return;
  }
  auto* fileList = m_page.fileList();
  if (fileList == nullptr || !fileList->visible() || !targetStillCurrent(*m_fileRenameTarget)) {
    m_inlineClipped = true;
    m_inlineClip->setVisible(false);
    m_inlineErrorBanner->setVisible(false);
    return;
  }
  const float singleLine = renderer.fontRowExtent(Style::fontSizeBody);
  const auto labelRect = m_page.entryNameRect(m_fileRenameTarget->id, singleLine);
  auto viewport = fileList->scrollView().contentViewportRect();
  if (m_page.viewMode() == luciole::ViewMode::Miller) {
    const auto millerViewport = m_page.millerView().viewportRect();
    const float left = std::max(viewport.x, millerViewport.x);
    const float top = std::max(viewport.y, millerViewport.y);
    const float right = std::min(viewport.x + viewport.width, millerViewport.x + millerViewport.width);
    const float bottom = std::min(viewport.y + viewport.height, millerViewport.y + millerViewport.height);
    viewport = {left, top, std::max(0.0F, right - left), std::max(0.0F, bottom - top)};
  }
  if (!labelRect || viewport.width <= 0.0F || viewport.height <= 0.0F) {
    m_inlineClipped = true;
    m_inlineClip->setFrameSize(0.0F, 0.0F);
    m_inlineErrorBanner->setVisible(false);
    return;
  }
  const float inputH = std::max(kInlineMinHeight, labelRect->height + kInputTextInnerInset * 2.0F);
  const float labelCenterY = labelRect->y + labelRect->height * 0.5F;
  const float inputX = labelRect->x - kInputTextInnerInset;
  const float inputY = std::round(labelCenterY - inputH * 0.5F);
  const float inputW = std::max(kInlineMinWidth, labelRect->width + kInputTextInnerInset * 2.0F);

  const float clipLeft = std::max(inputX, viewport.x);
  const float clipTop = std::max(inputY, viewport.y);
  const float clipRight = std::min(inputX + inputW, viewport.x + viewport.width);
  const float clipBottom = std::min(inputY + inputH, viewport.y + viewport.height);
  if (clipRight <= clipLeft || clipBottom <= clipTop) {
    m_inlineClipped = true;
    m_inlineClip->setFrameSize(0.0F, 0.0F);
    m_inlineErrorBanner->setVisible(false);
    return;
  }
  m_inlineClipped = clipLeft > inputX + 0.5F
      || clipTop > inputY + 0.5F
      || clipRight < inputX + inputW - 0.5F
      || clipBottom < inputY + inputH - 0.5F;

  float hostX = 0.0F;
  float hostY = 0.0F;
  (void)Node::mapFromScene(m_inlineClip->parent(), clipLeft, clipTop, hostX, hostY);
  m_inlineClip->setVisible(true);
  m_inlineClip->setPosition(hostX, hostY);
  m_inlineClip->setFrameSize(clipRight - clipLeft, clipBottom - clipTop);
  m_inlineInput->setControlHeight(inputH);
  m_inlineInput->arrange(renderer, {inputX - clipLeft, inputY - clipTop, inputW, inputH});

  const std::string& noteText = !m_inlineError.empty() ? m_inlineError : m_inlineHint;
  if (noteText.empty()) {
    m_inlineErrorBanner->setVisible(false);
    return;
  }
  const auto* pane = m_inlineErrorBanner->parent();
  const auto paneScene = sceneRect(*pane);
  const float maxBannerW =
      std::clamp(paneScene.width - kInlineErrorEdgeMargin * 2.0F, kInlineErrorMinWidth, kInlineErrorMaxWidth);
  m_inlineErrorBanner->setPadding(
      kInlineErrorPaddingV, Style::rtl() ? 0.0F : kInlineErrorPaddingTrailing, kInlineErrorPaddingV,
      Style::rtl() ? kInlineErrorPaddingTrailing : 0.0F
  );
  m_inlineErrorLabel->setMaxWidth(
      std::max(1.0F, maxBannerW - kInlineErrorBarWidth - Style::spaceXs - kInlineErrorPaddingTrailing)
  );
  LayoutConstraints constraints;
  constraints.setMaxWidth(maxBannerW);
  const auto bannerSize = m_inlineErrorBanner->measure(renderer, constraints);
  const float bannerW = std::min(bannerSize.width, maxBannerW);
  const float bannerH = bannerSize.height;

  const float preferredX = m_page.viewMode() == luciole::ViewMode::Grid
      ? labelRect->x + (labelRect->width - bannerW) * 0.5F
      : (Style::rtl() ? labelRect->x + labelRect->width - bannerW : labelRect->x);
  const float minX = paneScene.x + kInlineErrorEdgeMargin;
  const float maxX = std::max(minX, paneScene.x + paneScene.width - bannerW - kInlineErrorEdgeMargin);
  const float bannerSceneX = std::clamp(preferredX, minX, maxX);

  float bannerSceneY = clipBottom + kInlineErrorGap;
  if (bannerSceneY + bannerH > paneScene.y + paneScene.height - kInlineErrorEdgeMargin
      && clipTop - bannerH - kInlineErrorGap >= paneScene.y + kInlineErrorGap) {
    bannerSceneY = clipTop - bannerH - kInlineErrorGap;
  }

  float bannerX = 0.0F;
  float bannerY = 0.0F;
  (void)Node::mapFromScene(pane, bannerSceneX, bannerSceneY, bannerX, bannerY);
  m_inlineErrorBanner->setVisible(true);
  m_inlineErrorBanner->arrange(renderer, {bannerX, bannerY, bannerW, bannerH});
}

void RenameController::changed(OperationJob& job) {
  if (&job == m_job.get()) {
    m_dirty = true;
  } else if (!job.running() && job.takeResult()) {
    // A job whose editor closed (undo/redo replay or background focus-loss commit).
    syncFileRename(job);
  }
}

void RenameController::onFrame() {
  if (std::exchange(m_dirty, false) && m_job) {
    syncFileRename(*m_job);
  }
}

void RenameController::syncFileRename(OperationJob& job) {
  const auto& renamer = job.service<luciole::RenameService>();
  const auto& snapshot = renamer.snapshot();
  if (renamer.busy()) {
    return;
  }
  const bool editor = &job == m_job.get() && editingFile();
  if (snapshot.status == luciole::RenameStatus::Succeeded) {
    if (editor) {
      (void)job.takeResult();
    }
    m_page.setActionMessage(
        job.historyReplay() == HistoryReplay::Undo       ? "Rename undone."
            : job.historyReplay() == HistoryReplay::Redo ? "Rename redone."
                                                         : ""
    );
    auto& browser = m_page.browser();
    if (editor) {
      const auto next = std::exchange(m_pendingNextTarget, std::nullopt);
      m_pendingSubmitName.reset();
      m_fileRenameSubmitting = false;
      m_job.reset();
      if (next) {
        if (snapshot.source) {
          m_page.refreshAfterRename(*snapshot.source);
        }
        m_resumeNextTarget = PendingResume{
            .id = next->id,
            .location = next->location,
            .contextSerial = browser.contextSerial(),
            .direction = next->direction,
            .stride = next->stride,
            .retiringJob = {}
        };
        closeInlineUi(true, true);
        advancePendingResume();
      } else {
        if (snapshot.source) {
          m_page.refreshAfterRename(*snapshot.source, snapshot.destination);
        }
        closeInlineUi(true);
      }
    } else if (snapshot.source) {
      m_page.refreshAfterRename(
          *snapshot.source, job.historyReplay() != HistoryReplay::None ? snapshot.destination : std::nullopt
      );
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
  } else if (snapshot.status == luciole::RenameStatus::Ready) {
    if (auto pending = std::exchange(m_pendingSubmitName, std::nullopt)) {
      auto next = std::exchange(m_pendingNextTarget, std::nullopt);
      m_fileRenameSubmitting = false;
      m_inlineInput->setEnabled(true);
      submitInlineRename(std::move(*pending), std::move(next));
      if (editingFile() && !m_fileRenameSubmitting) {
        m_page.dispatcher().setFocus(m_inlineInput->inputArea());
      }
    }
  } else if (snapshot.status == luciole::RenameStatus::Error) {
    (void)job.takeResult();
    m_fileRenameSubmitting = false;
    m_pendingSubmitName.reset();
    m_pendingNextTarget.reset();
    if (!resolveActiveTarget()) {
      m_page.actionError("Could not rename: " + renameErrorText(snapshot.error));
      cancelFileRename(true);
      return;
    }
    m_inlineInput->setEnabled(true);
    showInlineError(renameErrorText(snapshot.error));
    m_page.dispatcher().setFocus(m_inlineInput->inputArea());
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
      m_page.dialogWidth(DialogWidth::Small), "Rename Place", "pencil", current.label,
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

void RenameController::openSeparatorHeading(std::size_t before) {
  if (!m_pins.editable()
      || before >= m_pins.bookmarks().size()
      || !std::ranges::contains(m_pins.separators(), before)) {
    m_page.actionError("This separator is no longer available.");
    return;
  }
  const auto current = m_pins.separatorHeading(before);
  // The separator is known by the place it sits above, as Rename Place knows its place: rows renumber when
  // Places changes while the dialog is open, in another window or another application.
  const auto anchor = m_pins.bookmarks()[before].location;
  auto editor = std::make_unique<NameDialog>(
      m_page.dialogWidth(DialogWidth::Small), current.empty() ? "Add Heading" : "Rename Heading", "heading", current,
      "Shown under the separator line. Only Luciole shows it.",
      [](const std::string& label) { return luciole::separatorHeadingProblem(label); },
      [this, anchor](std::string label) {
        const auto& pins = m_pins.bookmarks();
        const auto found = std::ranges::find(pins, anchor, &luciole::Bookmark::location);
        const auto row = static_cast<std::size_t>(found - pins.begin());
        if (found == pins.end()
            || !std::ranges::contains(m_pins.separators(), row)
            || !m_pins.setSeparatorHeading(row, std::move(label))) {
          if (m_headingEditor != nullptr) {
            m_headingEditor->showError(
                m_pins.busy() ? "Places are being saved. Try again shortly."
                              : "This separator changed. Close this dialog and try again."
            );
          }
          return;
        }
        m_page.dismissPopup();
      },
      [this] { m_page.dismissPopup(); }, current.empty() ? "Add" : "Rename", "Saving…"
  );
  auto* control = editor.get();
  m_page.showDialog(std::move(editor), DialogWidth::Small);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::SeparatorHeading, [this] { m_headingEditor = nullptr; });
    m_headingEditor = control;
    control->input()->setPlaceholder("Heading");
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
  picker->setTitle("Choose Bookmark Icon");
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
