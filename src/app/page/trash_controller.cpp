#include "app/page/trash_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "config/settings.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "fs/gio_ptr.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>

TrashController::TrashController(PageContext& page) : m_page(page) {}

bool TrashController::canTrashItems(const std::vector<luciole::DirectoryEntry>& targets) const {
  return m_page.actionState(luciole::BrowserAction::Trash, m_page.requestFor(targets)).enabled;
}

void TrashController::trashItems(const std::vector<luciole::DirectoryEntry>& targets) {
  if (!canTrashItems(targets) || !m_page.targetsCurrent(targets)) {
    m_page.actionError("This selection cannot be moved to Trash right now.");
    return;
  }
  m_page.cancelDrag();
  std::vector<luciole::Location> sources;
  sources.reserve(targets.size());
  for (const auto& target : targets) {
    sources.push_back(target.location());
  }
  const auto job = m_page.jobs().create<luciole::TrashService>();
  if (!job->service<luciole::TrashService>().prepare(std::move(sources))) {
    m_page.actionError("Another Trash operation is still finishing.");
  }
}

void TrashController::restore(const std::vector<luciole::DirectoryEntry>& targets) {
  m_page.cancelDrag();
  m_page.setActionMessage({});
  if (!m_page.jobs().create<luciole::RestoreService>()->service<luciole::RestoreService>().start(targets)) {
    m_page.actionError("Could not start restoring the selection.");
  }
}

void TrashController::restoreChanged(OperationJob& job) {
  const auto& restore = job.service<luciole::RestoreService>();
  if (!restore.busy() && job.takeResult()) {
    const auto& results = restore.results();
    const auto count = [&](luciole::RestoreOutcome outcome) {
      return std::ranges::count(results, outcome, &luciole::RestoreResult::outcome);
    };
    const auto restored = count(luciole::RestoreOutcome::Restored);
    std::string message = std::format(
        "{} item{} {}", restored, restored == 1 ? "" : "s", restore.moving() ? "moved out of Trash" : "restored"
    );
    for (const auto& [outcome, label] :
         {std::pair{luciole::RestoreOutcome::Skipped, "skipped"}, std::pair{luciole::RestoreOutcome::Failed, "failed"},
          std::pair{luciole::RestoreOutcome::Cancelled, "cancelled"},
          std::pair{luciole::RestoreOutcome::Unconfirmed, "could not be confirmed"}}) {
      if (const auto amount = count(outcome); amount != 0) {
        message += std::format(" · {} {}", amount, label);
      }
    }
    const auto error = std::ranges::find_if(results, [](const auto& item) { return item.error.has_value(); });
    if (error != results.end()) {
      message += std::format(" · {}: {}", error->displayName, luciole::fileErrorText(error->error));
    }
    m_page.setActionMessage(std::move(message));
    m_page.browser().refresh();
  }
}

void TrashController::deleteItems(const std::vector<luciole::DirectoryEntry>& targets) {
  if (targets.empty()) {
    return;
  }
  if (m_page.settings().confirmPermanentDeletion) {
    openEmptyTrash(targets);
    return;
  }
  m_page.cancelDrag();
  if (!m_page.jobs().create<luciole::EmptyTrashService>()->service<luciole::EmptyTrashService>().deleteSelection(
          targets
      )) {
    m_page.actionError("Could not start deleting the selection.");
  }
}

void TrashController::openEmptyTrash(std::optional<std::vector<luciole::DirectoryEntry>> selection) {
  if (m_page.jobs().busy()) {
    m_page.actionError("Another file operation is still finishing.");
    return;
  }
  const bool outsideTrash = selection && !selection->empty() && !selection->front().location().isTrash();
  m_page.cancelDrag();
  const float width = m_page.dialogWidth(DialogWidth::Small);
  std::string title = "Empty Trash?";
  if (selection) {
    title = selection->size() == 1 ? std::format("Delete “{}”?", selection->front().displayName())
                                   : std::format("Delete {} items?", selection->size());
  }
  Label* titleLabel = nullptr;
  Label* message = nullptr;
  Label* size = nullptr;
  Button* confirm = nullptr;
  Flex* buttonRow = nullptr;
  auto buttons = ui::row(
      {.out = &buttonRow, .align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
      ui::button({.text = "Cancel", .variant = ButtonVariant::Default, .onClick = [this] { m_page.dismissPopup(); }}),
      ui::button(
          {.out = &confirm,
           .text = selection ? "Delete" : "Empty Trash",
           .enabled = false,
           .variant = ButtonVariant::Destructive,
           .onClick = [this] { confirmEmptyTrash(); }}
      )
  );
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader(
          title, "trash", [this] { m_page.dismissPopup(); }, &titleLabel
      ),
      ui::label(
          {.out = &message,
           .text = outsideTrash ? "Preparing deletion…" : "Reading Trash…",
           .maxWidth = width - 24.0F,
           .maxLines = 4,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      ),
      ui::label(
          {.out = &size,
           .text = "Size: calculating…",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 24.0F,
           .maxLines = 2}
      ),
      std::move(buttons)
  );
  if (selection) {
    titleLabel->setMaxLines(2);
    titleLabel->setEllipsize(TextEllipsize::Middle);
  }
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Small);
  if (m_page.popupSurface() != nullptr) {
    m_emptyTrashButtonRow = buttonRow;
    m_page.activePopup().bind(ActivePopup::Kind::EmptyTrash, [this] {
      if (const auto job = std::exchange(m_emptyTrash, nullptr)) {
        job->cancel();
      }
      m_emptyTrashMessage = nullptr;
      m_emptyTrashSize = nullptr;
      m_emptyTrashConfirm = nullptr;
      m_emptyTrashButtonRow = nullptr;
    });
    m_emptyTrash = m_page.jobs().create<luciole::EmptyTrashService>(JobScope::Page);
    m_emptyTrashMessage = message;
    m_emptyTrashSize = size;
    m_emptyTrashConfirm = confirm;
    auto& emptyTrash = m_emptyTrash->service<luciole::EmptyTrashService>();
    if (!(outsideTrash    ? emptyTrash.prepareDeletion(std::move(*selection))
              : selection ? emptyTrash.prepareSelection(std::move(*selection))
                          : emptyTrash.prepare())) {
      message->setText("Another deletion is still finishing.");
    }
  }
}

bool TrashController::key(const KeyboardEvent& event) {
  if (m_page.activePopup().kind() == ActivePopup::Kind::EmptyTrash
      && !m_page.popupDispatcher().focusedArea()
      && event.pressed
      && !event.preedit
      && event.modifiers == 0
      && KeySymbol::isEnter(event.sym)) {
    m_page.dismissPopup();
    return true;
  }
  if (m_page.activePopup().kind() == ActivePopup::Kind::EmptyTrash
      && event.pressed
      && !event.preedit
      && event.modifiers == 0
      && (KeySymbol::isLeft(event.sym) || KeySymbol::isRight(event.sym))) {
    m_page.popupDispatcher().cycleTabFocusInSubtree(m_emptyTrashButtonRow, KeySymbol::isLeft(event.sym));
    return true;
  }
  return false;
}

void TrashController::confirmEmptyTrash() {
  if (!m_emptyTrash || !m_emptyTrash->service<luciole::EmptyTrashService>().empty()) {
    return;
  }
  m_page.jobs().follow(*m_emptyTrash);
  m_emptyTrashConfirm->setEnabled(false);
  m_emptyTrash.reset();
  m_page.dismissPopup();
}

void TrashController::onFrame() {
  if (std::exchange(m_emptyTrashDirty, false)) {
    syncEmptyTrash();
  }
}

void TrashController::emptyTrashChanged(OperationJob& job) {
  if (&job == m_emptyTrash.get()) {
    m_emptyTrashDirty = true;
    return;
  }
  const auto& snapshot = job.service<luciole::EmptyTrashService>().snapshot();
  if (snapshot.status == luciole::EmptyTrashStatus::Finished && job.takeResult()) {
    const auto count = [&](luciole::EmptyTrashOutcome outcome) {
      return std::ranges::count(snapshot.results, outcome, &luciole::EmptyTrashResult::outcome);
    };
    const auto done = count(luciole::EmptyTrashOutcome::Deleted);
    std::string message = std::format("{} item{} permanently deleted", done, done == 1 ? "" : "s");
    for (const auto& [outcome, label] :
         {std::pair{luciole::EmptyTrashOutcome::Skipped, "skipped"},
          std::pair{luciole::EmptyTrashOutcome::Failed, "failed"},
          std::pair{luciole::EmptyTrashOutcome::Cancelled, "cancelled"},
          std::pair{luciole::EmptyTrashOutcome::Unconfirmed, "could not be confirmed"}}) {
      if (const auto amount = count(outcome); amount != 0) {
        message += std::format(" · {} {}", amount, label);
      }
    }
    const auto error = std::ranges::find_if(snapshot.results, [](const auto& item) { return item.error.has_value(); });
    if (error != snapshot.results.end()) {
      message += std::format(" · {}: {}", error->displayName, luciole::fileErrorText(error->error));
    }
    m_page.actionError(std::move(message));
    auto& browser = m_page.browser();
    if (snapshot.outsideTrash) {
      browser.refresh();
    } else if (const auto& location = browser.model().snapshot().location; location && location->isTrash()) {
      browser.open(luciole::Location::trash());
    }
  } else if (snapshot.status == luciole::EmptyTrashStatus::Error && job.takeResult()) {
    m_page.actionError(
        std::string(snapshot.selection ? "Could not delete selected items: " : "Could not empty Trash: ")
        + luciole::fileErrorText(snapshot.error)
    );
  }
}

void TrashController::syncEmptyTrash() {
  if (!m_emptyTrash || m_emptyTrashMessage == nullptr) {
    return;
  }
  const auto& emptyTrash = m_emptyTrash->service<luciole::EmptyTrashService>();
  const auto& snapshot = emptyTrash.snapshot();
  if (snapshot.status == luciole::EmptyTrashStatus::Ready) {
    const auto count = snapshot.results.size();
    if (count == 0) {
      m_emptyTrashMessage->setText("Trash is already empty.");
    } else if (snapshot.selection) {
      m_emptyTrashMessage->setText(
          count == 1 ? "This item will be permanently deleted. This cannot be undone."
                     : "These items will be permanently deleted. This cannot be undone."
      );
    } else {
      m_emptyTrashMessage->setText(
          std::format(
              "Permanently delete {} item{} and their contents? This cannot be undone.", count, count == 1 ? "" : "s"
          )
      );
    }
    m_emptyTrashConfirm->setEnabled(emptyTrash.canEmpty());
  } else if (snapshot.status == luciole::EmptyTrashStatus::Error) {
    m_emptyTrashMessage->setText(
        std::string(snapshot.outsideTrash ? "Could not prepare deletion: " : "Could not read Trash: ")
        + luciole::fileErrorText(snapshot.error)
    );
    m_emptyTrashConfirm->setEnabled(false);
  }
  std::string sizeText = "Size: calculating…";
  if (snapshot.size) {
    const luciole::GString value(g_format_size_full(*snapshot.size, G_FORMAT_SIZE_IEC_UNITS));
    sizeText = std::format("Size: {}{}", snapshot.sizeIncomplete ? "at least " : "", value.get());
  } else if (!emptyTrash.busy()) {
    sizeText = "Size unavailable";
  }
  m_emptyTrashSize->setText(sizeText);
  m_page.popupSurface()->requestLayout();
}

void TrashController::trashChanged(OperationJob& job) {
  const auto& trash = job.service<luciole::TrashService>();
  const auto& snapshot = trash.snapshot();
  // OperationHub::update() starts a prepared move.
  if (trash.busy() || trash.canTrash() || !job.takeResult()) {
    return;
  }
  if (snapshot.status == luciole::TrashStatus::Error) {
    m_page.actionError("Could not move to Trash: " + luciole::fileErrorText(snapshot.error));
  } else if (snapshot.status == luciole::TrashStatus::Cancelled) {
    m_page.actionError("Move to Trash cancelled.");
  } else if (snapshot.status == luciole::TrashStatus::Finished) {
    const auto done =
        std::ranges::count(snapshot.results, luciole::TrashOutcome::Trashed, &luciole::TrashResult::outcome);
    const auto failed =
        std::ranges::count(snapshot.results, luciole::TrashOutcome::Failed, &luciole::TrashResult::outcome);
    const auto cancelled =
        std::ranges::count(snapshot.results, luciole::TrashOutcome::Cancelled, &luciole::TrashResult::outcome);
    std::string message = std::format("{} item{} moved to Trash", done, done == 1 ? "" : "s");
    if (failed != 0) {
      message += std::format(" · {} failed", failed);
    }
    if (cancelled != 0) {
      message += std::format(" · {} cancelled", cancelled);
    }
    const auto error =
        std::ranges::find_if(snapshot.results, [](const auto& result) { return result.error.has_value(); });
    if (error != snapshot.results.end()) {
      message +=
          std::format(" · {}: {}", error->source.displayName().value_or("Item"), luciole::fileErrorText(*error->error));
    }
    m_page.actionError(std::move(message));
    m_page.browser().refresh();
  }
}
