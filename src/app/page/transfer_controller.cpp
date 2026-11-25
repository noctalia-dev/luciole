#include "app/page/transfer_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/create_controller.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/ui_phase.h"
#include "fs/archive_type.h"
#include "fs/filename.h"
#include "fs/gio_ptr.h"
#include "operations/archive_service.h"
#include "operations/create_service.h"
#include "operations/trash_item.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/dialogs/name_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace {
  constexpr int kDropCopy = 1;
  constexpr int kDropMove = 2;
  constexpr int kDropLink = 3;
  constexpr int kDropExtract = 4;
  constexpr int kDropNewFolder = 5;
  constexpr int kDropCancel = -1;

  bool arrived(const luciole::TransferResult& result) {
    return result.outcome == luciole::TransferOutcome::Moved
        || result.outcome == luciole::TransferOutcome::Copied
        || result.outcome == luciole::TransferOutcome::Linked;
  }
  template <typename Callback> auto withTransferResults(const OperationJob& job, Callback callback) {
    if (job.kind() != JobKind::Restore) {
      return callback(job.service<luciole::TransferService>().results());
    }
    const auto results = job.service<luciole::RestoreService>().transferResults();
    return callback(results);
  }
} // namespace

TransferController::TransferController(PageContext& page, std::function<void()> progressTick, CreateController& create)
    : m_page(page), m_progressTick(std::move(progressTick)), m_create(create) {}

void TransferController::paste(const luciole::ActionRequest& request) {
  if (request.folder) {
    paste(request, luciole::ActionScope::Location, *request.folder);
  }
}

void TransferController::pasteInto(const luciole::ActionRequest& request) {
  if (request.targets.size() == 1) {
    paste(request, luciole::ActionScope::Targets, request.targets.front().location);
  }
}

void TransferController::paste(
    const luciole::ActionRequest& request, luciole::ActionScope scope, luciole::Location destination
) {
  m_pendingFilePaste = true;
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_page.clipboard().requestFiles([this, lifetime, request, scope, destination = std::move(destination)](
                                      std::optional<luciole::ClipboardFiles> selection
                                  ) mutable {
    if (lifetime.expired()) {
      return;
    }
    if (!selection) {
      if (m_page.clipboard().hasImage()) {
        // No file URIs: the clipboard may still hold an image to save as a new file.
        pasteImage(request, scope, std::move(destination));
        return;
      }
      const bool pending = std::exchange(m_pendingFilePaste, false);
      if (!pending || !m_page.visible() || !m_page.requestCurrent(request, scope)) {
        return;
      }
      m_page.actionError("The clipboard changed or its file data could not be read.");
      return;
    }
    const bool pending = std::exchange(m_pendingFilePaste, false);
    if (!pending || !m_page.visible() || !m_page.requestCurrent(request, scope)) {
      return;
    }
    if (m_page.operationsBusy()) {
      return;
    }
    const bool cut = selection->payload.cut;
    if (cut && !m_page.clipboard().claimMove(selection->revision)) {
      return;
    }
    const auto job = startPayloadTransfer(
        selection->payload, destination, cut ? luciole::TransferAction::Move : luciole::TransferAction::Copy
    );
    if (cut) {
      if (job) {
        finishMove(*job, std::move(*selection));
      } else {
        m_page.clipboard().finishMove(*selection, {});
      }
    }
  });
}

void TransferController::pasteImage(
    const luciole::ActionRequest& request, luciole::ActionScope scope, luciole::Location destination
) {
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_page.clipboard().requestImage([this, lifetime, request, scope,
                                   destination =
                                       std::move(destination)](std::optional<luciole::ClipboardImage> image) mutable {
    if (lifetime.expired()) {
      return;
    }
    const bool pending = std::exchange(m_pendingFilePaste, false);
    if (!pending || !m_page.visible() || !m_page.requestCurrent(request, scope)) {
      return;
    }
    if (!image) {
      m_page.actionError("The clipboard changed or its image data could not be read.");
      return;
    }
    if (m_page.operationsBusy()) {
      return;
    }
    m_create.openImagePaste(std::move(*image), std::move(destination));
  });
}

void TransferController::transferTo(
    const luciole::ActionRequest& request, const luciole::Location& destination, bool copy
) {
  if (!m_page.requestCurrent(request, luciole::ActionScope::Targets)) {
    return;
  }
  std::vector<luciole::Location> sources;
  sources.reserve(request.targets.size());
  for (const auto& target : request.targets) {
    sources.push_back(target.location);
  }
  if (!sources.empty()) {
    transfer(std::move(sources), destination, copy ? luciole::TransferAction::Copy : luciole::TransferAction::Move);
  }
}

void TransferController::transfer(
    std::vector<luciole::Location> sources, const luciole::Location& destination, luciole::TransferAction action
) {
  (void)startPayloadTransfer({std::move(sources), false}, destination, action);
}

std::shared_ptr<OperationJob> TransferController::start(luciole::TransferRequest request) {
  auto job = m_page.jobs().create<luciole::TransferService>();
  if (!job->service<luciole::TransferService>().start(std::move(request))) {
    return nullptr;
  }
  return job;
}

void TransferController::finishMove(OperationJob& job, luciole::ClipboardFiles selection) {
  job.whenIdle([&clipboard = m_page.clipboard(), selection = std::move(selection)](OperationJob& done) {
    withTransferResults(done, [&](const auto& results) { clipboard.finishMove(selection, results); });
  });
}

void TransferController::extractDrop(
    luciole::ExtractionOffer offer, const luciole::Location& destination, luciole::DropCompletion finish
) {
  if (m_page.operationsBusy() || !m_page.visible() || m_page.popupSurface()) {
    finish(false);
    return;
  }
  const std::weak_ptr<int> lifetime = m_lifetime;
  const auto context = m_page.browser().contextSerial();
  const auto completed = [this, lifetime, context, finish = std::move(finish)](const std::string& error) {
    finish(error.empty());
    if (!lifetime.expired() && context == m_page.browser().contextSerial()) {
      m_page.actionError(
          error.empty() ? "Extraction requested in the source application." : "Could not request extraction: " + error
      );
    }
  };
  if (!m_extraction.request(std::move(offer), destination, completed)) {
    completed("Another extraction request is still finishing.");
  }
  m_page.requestSync();
}

void TransferController::receiveDrop(
    luciole::FilePayload payload, const luciole::Location& destination, luciole::DropOptions options,
    luciole::DropCompletion finish
) {
  if (destination == luciole::Location::trash()) {
    trashDrop(std::move(payload), options, std::move(finish));
    return;
  }
  if (options.action == luciole::DragAction::Ask) {
    chooseDrop(std::move(payload), destination, std::move(options), std::move(finish));
    return;
  }
  const auto action =
      options.action == luciole::DragAction::Move ? luciole::TransferAction::Move : luciole::TransferAction::Copy;
  if (!options.select || !options.select(action)) {
    finish(false);
    return;
  }
  resolvedDrop(std::move(payload), destination, action, std::move(finish));
}

void TransferController::trashDrop(
    luciole::FilePayload payload, const luciole::DropOptions& options, luciole::DropCompletion finish
) {
  if (m_page.operationsBusy()
      || options.action != luciole::DragAction::Move
      || !options.canMove
      || !options.select
      || !options.select(luciole::TransferAction::Move)) {
    finish(false);
    return;
  }
  const auto job = m_page.jobs().create<luciole::TrashService>();
  if (!job->service<luciole::TrashService>().prepare(std::move(payload.locations))) {
    finish(false);
    return;
  }
  job->whenFinished([finish = std::move(finish)](OperationJob& done) {
    const auto& snapshot = done.service<luciole::TrashService>().snapshot();
    finish(
        snapshot.status == luciole::TrashStatus::Finished
        && !snapshot.results.empty()
        && std::ranges::all_of(snapshot.results, [](const auto& result) {
             return result.outcome == luciole::TrashOutcome::Trashed;
           })
    );
  });
}

void TransferController::resolvedDrop(
    luciole::FilePayload payload, const luciole::Location& destination, luciole::TransferAction action,
    luciole::DropCompletion finish
) {
  const auto job = startPayloadTransfer(std::move(payload), destination, action);
  if (!job) {
    finish(false);
    return;
  }
  job->whenIdle([finish = std::move(finish)](OperationJob& done) {
    finish(withTransferResults(done, [](const auto& results) {
      return !results.empty() && std::ranges::all_of(results, arrived);
    }));
  });
}

void TransferController::extractedDrop(
    const luciole::FilePayload& payload, const luciole::Location& destination, const luciole::DropOptions& options,
    luciole::DropCompletion finish
) {
  if (payload.locations.size() != 1
      || !destination.isNative()
      || !options.canCopy
      || !options.select
      || !luciole::isSupportedArchive(payload.locations.front(), "")) {
    finish(false);
    return;
  }
  if (!options.select(luciole::TransferAction::Copy)) {
    finish(false);
    return;
  }
  const auto& source = payload.locations.front();
  const auto target = destination.child(luciole::archiveFolderName(source));
  const auto job = m_page.jobs().create<luciole::ArchiveService>();
  if (!job->service<luciole::ArchiveService>().extract(source, target, {}, nullptr)) {
    finish(false);
    return;
  }
  job->whenIdle([finish = std::move(finish)](OperationJob& done) {
    const auto& archives = done.service<luciole::ArchiveService>();
    finish(!archives.error().has_value() && !archives.results().empty());
  });
}

void TransferController::moveIntoNewFolder(
    luciole::FilePayload payload, const luciole::Location& destination, luciole::DropOptions options,
    luciole::DropCompletion finish
) {
  if (payload.locations.empty()
      || !options.canMove
      || !destination.isNative()
      || !m_page.visible()
      || m_page.operationsBusy()
      || !m_page.popupAvailable()) {
    finish(false);
    return;
  }
  const auto pending = std::make_shared<PendingNewFolder>(
      std::move(payload), std::move(options), std::move(finish), m_page.browser().contextSerial()
  );
  auto editor = std::make_unique<NameDialog>(
      m_page.dialogWidth(DialogWidth::Small), "New Folder", "folder-plus", "New Folder", "", luciole::filenameProblem,
      [this](std::string name) { submitNewFolder(std::move(name)); }, [this] { m_page.dismissPopup(); }, "Create",
      "Creating…"
  );
  m_newFolder = pending;
  m_newFolderEditor = editor.get();
  m_page.showDialog(std::move(editor), DialogWidth::Small);
  if (!m_page.popupSurface()) {
    closeNewFolder();
    return;
  }
  m_page.activePopup().bind(ActivePopup::Kind::CreateItem, [this] { closeNewFolder(); });
  m_newFolder->createJob = m_page.jobs().create<luciole::CreateService>(JobScope::Page);
  watchNewFolder();
  auto& creator = m_newFolder->createJob->service<luciole::CreateService>();
  m_newFolder->preparing = true;
  m_newFolderEditor->setSaving(true);
  if (!creator.prepare(destination, luciole::CreateKind::Folder)) {
    m_newFolder->preparing = false;
    m_newFolderEditor->setSaving(false);
    m_newFolderEditor->showError("Another creation is still finishing.");
  }
}

void TransferController::submitNewFolder(std::string name) {
  const auto pending = m_newFolder;
  if (!pending || !pending->createJob || !m_newFolderEditor || pending->chosen || pending->preparing) {
    return;
  }
  auto& creator = pending->createJob->service<luciole::CreateService>();
  if (!creator.canCreate()) {
    m_newFolderEditor->showError("The destination is unavailable. Close this dialog and try again.");
    return;
  }
  watchNewFolder();
  if (!creator.create(std::move(name))) {
    const auto& error = creator.snapshot().error;
    m_newFolderEditor->showError(
        error ? luciole::nameErrorText(*error, "The destination folder is no longer available.")
              : "The destination is unavailable. Close this dialog and try again."
    );
    return;
  }
  m_page.jobs().follow(*pending->createJob);
  m_newFolderEditor->setSaving(true);
}

void TransferController::watchNewFolder() {
  const std::weak_ptr<int> lifetime = m_lifetime;
  const std::weak_ptr<PendingNewFolder> weak = m_newFolder;
  m_newFolder->createJob->whenIdle([this, lifetime, weak](OperationJob&) {
    if (!lifetime.expired() && !weak.expired()) {
      m_newFolderDirty = true;
    }
  });
}

void TransferController::closeNewFolder() {
  if (m_newFolder) {
    m_newFolder->cancel();
    if (m_newFolder->createJob && (m_page.jobs().attached() || !m_newFolder->createJob->running())) {
      m_newFolder->createJob->cancel();
    }
  }
  m_newFolder.reset();
  m_newFolderEditor = nullptr;
  m_newFolderDirty = false;
}

void TransferController::onFrame() {
  if (std::exchange(m_newFolderDirty, false)) {
    syncNewFolder();
  }
}

void TransferController::syncNewFolder() {
  const auto pending = m_newFolder;
  if (!pending || !pending->createJob || pending->createJob->running()) {
    return;
  }
  const auto& creator = pending->createJob->service<luciole::CreateService>();
  const auto& snapshot = creator.snapshot();
  if (pending->preparing) {
    if (snapshot.status == luciole::CreateStatus::Cancelled) {
      pending->cancel();
      closeNewFolder();
      m_page.dismissPopup();
      return;
    }
    if (snapshot.status == luciole::CreateStatus::Ready || snapshot.status == luciole::CreateStatus::Error) {
      pending->preparing = false;
      if (m_newFolderEditor) {
        m_newFolderEditor->setSaving(false);
        m_newFolderEditor->showError(
            snapshot.error ? luciole::nameErrorText(*snapshot.error, "Could not prepare the destination.")
                           : std::string{}
        );
        if (m_page.popupSurface()) {
          m_page.popupDispatcher().setFocus(m_newFolderEditor->input()->inputArea());
          m_newFolderEditor->input()->selectAll();
          m_page.popupSurface()->requestLayout();
        }
      }
    }
    return;
  }
  const bool created =
      snapshot.status == luciole::CreateStatus::Succeeded || snapshot.status == luciole::CreateStatus::Partial;
  if (!created) {
    if (m_newFolderEditor) {
      const auto& error = snapshot.error;
      m_newFolderEditor->setSaving(false);
      m_newFolderEditor->showError(
          error ? luciole::nameErrorText(*error, "Could not create the folder.")
                : "The destination folder is no longer available."
      );
      if (m_page.popupSurface()) {
        m_page.popupSurface()->requestLayout();
      }
    }
    return;
  }
  if (pending->context != m_page.browser().contextSerial()
      || !m_page.visible()
      || m_page.operationsBusy()
      || !snapshot.destination
      || !pending->options.select
      || !pending->options.select(luciole::TransferAction::Move)) {
    pending->cancel();
    closeNewFolder();
    m_page.dismissPopup();
    return;
  }
  auto payload = std::move(pending->payload);
  auto finish = std::move(pending->finish);
  pending->chosen = true;
  m_newFolder.reset();
  m_newFolderEditor = nullptr;
  m_page.dismissPopup();
  const auto job = startPayloadTransfer(std::move(payload), *snapshot.destination, luciole::TransferAction::Move);
  if (!job) {
    finish(false);
    return;
  }
  job->whenIdle([finish = std::move(finish)](OperationJob& done) {
    finish(withTransferResults(done, [](const auto& results) {
      return !results.empty() && std::ranges::all_of(results, arrived);
    }));
  });
}

void TransferController::chooseDrop(
    luciole::FilePayload payload, const luciole::Location& destination, luciole::DropOptions options,
    luciole::DropCompletion finish
) {
  if (payload.locations.empty()
      || !m_page.visible()
      || m_page.popupSurface()
      || m_page.operationsBusy()
      || !m_page.popupAvailable()) {
    finish(false);
    return;
  }
  if (!luciole::transferLocationSupported(destination) || std::ranges::any_of(payload.locations, [](const auto& item) {
        return !luciole::transferLocationSupported(item);
      })) {
    m_page.actionError("File transfers support local and mounted SFTP, SMB or WebDAV folders outside Trash.");
    finish(false);
    return;
  }
  struct Pending {
    luciole::FilePayload payload;
    luciole::Location destination;
    luciole::DropOptions options;
    luciole::DropCompletion finish;
    bool chosen = false;
    ~Pending() {
      if (!chosen) {
        finish(false);
      }
    }
    void cancel() {
      if (!std::exchange(chosen, true)) {
        finish(false);
      }
    }
  };
  const auto pending =
      std::make_shared<Pending>(std::move(payload), destination, std::move(options), std::move(finish));
  const auto context = m_page.browser().contextSerial();
  const std::weak_ptr<int> lifetime = m_lifetime;
  const auto choose = [this, lifetime, pending, context](int id) {
    if (pending->chosen) {
      return;
    }
    if (id == kDropCancel || lifetime.expired()) {
      pending->cancel();
      return;
    }
    if (id == kDropExtract) {
      if (!m_page.visible() || m_page.operationsBusy() || context != m_page.browser().contextSerial()) {
        pending->cancel();
        return;
      }
      pending->chosen = true;
      extractedDrop(pending->payload, pending->destination, pending->options, std::move(pending->finish));
      return;
    }
    if (id == kDropNewFolder) {
      if (!m_page.visible() || m_page.operationsBusy() || context != m_page.browser().contextSerial()) {
        pending->cancel();
        return;
      }
      pending->chosen = true;
      moveIntoNewFolder(
          std::move(pending->payload), pending->destination, std::move(pending->options), std::move(pending->finish)
      );
      return;
    }
    const auto action = id == kDropCopy ? luciole::TransferAction::Copy
        : id == kDropMove               ? luciole::TransferAction::Move
                                        : luciole::TransferAction::Link;
    if (!m_page.visible()
        || m_page.operationsBusy()
        || context != m_page.browser().contextSerial()
        || !pending->options.select
        || !pending->options.select(action)) {
      pending->cancel();
      return;
    }
    pending->chosen = true;
    resolvedDrop(std::move(pending->payload), pending->destination, action, std::move(pending->finish));
  };
  const bool canExtract = pending->payload.locations.size() == 1
      && destination.isNative()
      && pending->options.canCopy
      && luciole::isSupportedArchive(pending->payload.locations.front(), "");
  const bool canNewFolder = pending->options.canMove && destination.isNative();
  std::vector<ContextMenuControlEntry> entries;
  entries.push_back(
      {.id = kDropCopy, .label = "Copy Here", .enabled = pending->options.canCopy, .glyph = "copy", .shortcut = "Ctrl"}
  );
  entries.push_back(
      {.id = kDropMove,
       .label = "Move Here",
       .enabled = pending->options.canMove,
       .glyph = "file-import",
       .shortcut = "Shift"}
  );
  entries.push_back({.label = "", .separator = true});
  entries.push_back(
      {.id = kDropLink,
       .label = pending->payload.locations.size() == 1 ? "Create Link Here" : "Create Links Here",
       .enabled = pending->options.canCopy
           && std::ranges::all_of(
                      pending->payload.locations,
                      [&](const auto& source) { return luciole::transferLinkTarget(source, destination).has_value(); }
           ),
       .glyph = "link"}
  );
  entries.push_back({.id = kDropExtract, .label = "Extract Here", .enabled = canExtract, .glyph = "file-export"});
  entries.push_back(
      {.id = kDropNewFolder, .label = "Move into New Folder…", .enabled = canNewFolder, .glyph = "folder-plus"}
  );
  entries.push_back({.label = "", .separator = true});
  entries.push_back({.id = kDropCancel, .label = "Cancel", .glyph = "x", .shortcut = "Esc"});
  m_page.showContextMenu(std::move(entries), choose, {pending->options.x, pending->options.y, 1.0F, 1.0F});
}

void TransferController::transferChanged(OperationJob& job) {
  const auto& transfers = job.service<luciole::TransferService>();
  if (transfers.busy() || !job.takeResult()) {
    return;
  }
  const auto& results = transfers.results();
  const auto done = std::ranges::count_if(results, arrived);
  std::string message = std::format(
      "{} item{} {}", done, done == 1 ? "" : "s",
      transfers.action() == luciole::TransferAction::Copy       ? "copied"
          : transfers.action() == luciole::TransferAction::Link ? "linked"
                                                                : "moved"
  );
  if (job.historyReplay() != HistoryReplay::None) {
    message = std::format(
        "{} item{} {}.", done, done == 1 ? "" : "s", job.historyReplay() == HistoryReplay::Undo ? "undone" : "redone"
    );
  }
  if (const auto error = std::ranges::find_if(results, [](const auto& item) { return item.error.has_value(); });
      error != results.end()) {
    message +=
        std::format(" · {}: {}", error->source.displayName().value_or("Item"), luciole::fileErrorText(*error->error));
  }
  m_page.setActionMessage(std::move(message));
  m_page.browser().refresh();
}

void TransferController::syncConflict() {
  if (!m_page.popupAvailable()) {
    return;
  }
  if (m_activeConflict) {
    const auto* shown = m_page.jobs().find(m_activeConflict->job);
    const auto pending = shown ? shown->conflict() : std::nullopt;
    if (!pending || pending->id != m_activeConflict->conflict) {
      m_activeConflict.reset();
      m_page.dismissPopup();
    }
    return;
  }
  if (m_page.popupSurface() != nullptr) {
    return;
  }
  auto* job = m_page.jobs().oldest([](const OperationJob& candidate) { return candidate.conflict().has_value(); });
  const auto conflict = job ? job->conflict() : std::nullopt;
  if (!conflict) {
    return;
  }
  const bool archive = job->kind() == JobKind::Archive;
  const bool restore = job->kind() == JobKind::Restore;
  const bool nativeReplace = !archive && !restore && conflict->destination.isNative();
  const ActiveConflict active{.job = job->id(), .conflict = conflict->id};
  const float width = m_page.dialogWidth(DialogWidth::Medium);
  auto facts = [&](const std::string& title, bool folder, std::optional<std::uint64_t> bytes,
                   std::optional<std::uint64_t> modified) {
    std::string description = folder ? "Folder" : "File";
    if (!folder && bytes) {
      const luciole::GString size(g_format_size_full(*bytes, G_FORMAT_SIZE_IEC_UNITS));
      description += " · " + std::string(size.get());
    }
    std::string date = "Modification time unknown";
    if (modified && *modified <= static_cast<std::uint64_t>(G_MAXINT64)) {
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> value(
          g_date_time_new_from_unix_local(static_cast<gint64>(*modified)), &g_date_time_unref
      );
      if (value) {
        const luciole::GString text(g_date_time_format(value.get(), "%Y-%m-%d %H:%M"));
        date = text.get();
      }
    }
    return ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .flexGrow = 1.0F},
        ui::label(
            {.text = title, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        ),
        ui::label({.text = description, .maxWidth = (width - 48.0F) * 0.5F, .maxLines = 1}),
        ui::label(
            {.text = date,
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxWidth = (width - 48.0F) * 0.5F,
             .maxLines = 1}
        )
    );
  };
  const auto applyToAll = std::make_shared<bool>(false);
  const auto choose = [this, applyToAll, active](luciole::TransferConflictChoice choice) {
    auto* owner = m_page.jobs().find(active.job);
    if (owner
        && owner->resolve(active.conflict, choice, *applyToAll && choice != luciole::TransferConflictChoice::Cancel)) {
      m_activeConflict.reset();
      m_page.dismissPopup();
    }
  };
  Button* keepBoth = nullptr;
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader(
          "Item Already Exists", "alert-triangle", [choose] { choose(luciole::TransferConflictChoice::Cancel); }
      ),
      ui::label(
          {.text = conflict->name,
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .configure = [](Label& name) { name.setUseMarkup(false); }}
      ),
      ui::label(
          {.text = restore ? luciole::locationEditText(conflict->destination) : "",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .visible = restore,
           .participatesInLayout = restore,
           .configure = [](Label& path) { path.setUseMarkup(false); }}
      ),
      ui::separator({}),
      ui::row(
          {.align = FlexAlign::Start, .gap = Style::spaceMd},
          facts("Incoming", conflict->sourceDirectory, conflict->sourceSize, conflict->sourceModified),
          facts("Existing", conflict->destinationDirectory, conflict->destinationSize, conflict->destinationModified)
      ),
      ui::label(
          {.text = restore                      ? "Folder replacement is unavailable. Keep both or skip this item."
               : conflict->canMerge && !archive ? "Merge keeps existing contents and asks about conflicting "
                                                  "children. Replace removes the whole existing folder."
                                                : "Replace removes the existing folder and its contents.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .visible = conflict->destinationDirectory,
           .participatesInLayout = conflict->destinationDirectory}
      ),
      ui::label(
          {.text = "Replace is permanent. Undo cannot restore the existing item.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .visible = nativeReplace,
           .participatesInLayout = nativeReplace}
      ),
      ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .visible = !archive && !restore && conflict->canApplyToAll,
           .participatesInLayout = !archive && !restore && conflict->canApplyToAll},
          ui::toggle({.onChange = [applyToAll](bool checked) { *applyToAll = checked; }}),
          ui::label(
              {.text = conflict->scopeLabel,
               .fontSize = Style::fontSizeCaption,
               .maxWidth = width - 96.0F,
               .maxLines = 2}
          )
      ),
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.text = "Cancel",
               .variant = ButtonVariant::Ghost,
               .onClick = [choose] { choose(luciole::TransferConflictChoice::Cancel); }}
          ),
          ui::button(
              {.text = "Skip",
               .variant = ButtonVariant::Ghost,
               .onClick = [choose] { choose(luciole::TransferConflictChoice::Skip); }}
          ),
          ui::button(
              {.out = &keepBoth,
               .text = "Keep Both",
               .variant = ButtonVariant::Primary,
               .onClick = [choose] { choose(luciole::TransferConflictChoice::KeepBoth); }}
          ),
          ui::button(
              {.text = "Merge",
               .variant = ButtonVariant::Ghost,
               .visible = !archive && !restore && conflict->canMerge,
               .participatesInLayout = !archive && !restore && conflict->canMerge,
               .onClick = [choose] { choose(luciole::TransferConflictChoice::Merge); }}
          ),
          ui::button(
              {.text = "Replace",
               .enabled = !restore || (!conflict->sourceDirectory && !conflict->destinationDirectory),
               .variant = ButtonVariant::Destructive,
               .tooltip = restore && (conflict->sourceDirectory || conflict->destinationDirectory)
                   ? "Folder replacement is unavailable; keep both or skip."
                   : "Replace the existing item",
               .onClick = [choose] { choose(luciole::TransferConflictChoice::Replace); }}
          )
      )
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::TransferConflict, [this] {
      if (const auto shown = std::exchange(m_activeConflict, std::nullopt)) {
        if (auto* owner = m_page.jobs().find(shown->job)) {
          (void)owner->resolve(shown->conflict, luciole::TransferConflictChoice::Cancel);
        }
      }
    });
    m_activeConflict = active;
    m_page.popupDispatcher().setFocus(keepBoth->inputArea());
  } else {
    (void)job->resolve(conflict->id, luciole::TransferConflictChoice::Cancel);
  }
}

void TransferController::retry(luciole::JobId target) {
  auto* job = m_page.jobs().find(target);
  if (m_page.operationsBusy()
      || !job
      || job->kind() != JobKind::Transfer
      || job->service<luciole::TransferService>().retryable() == 0) {
    return;
  }
  auto& transfers = job->service<luciole::TransferService>();
  if (transfers.action() != luciole::TransferAction::Move || !m_page.clipboard().hasFiles()) {
    (void)transfers.retryFailed();
    return;
  }
  const auto id = job->id();
  const auto run = transfers.jobId();
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_page.clipboard().requestFiles([this, lifetime, id, run](std::optional<luciole::ClipboardFiles> selection) {
    if (lifetime.expired() || m_page.operationsBusy()) {
      return;
    }
    auto* job = m_page.jobs().find(id);
    if (!job || job->service<luciole::TransferService>().jobId() != run) {
      return;
    }
    auto& transfers = job->service<luciole::TransferService>();
    const bool claim = selection
        && selection->payload.cut
        && std::ranges::any_of(selection->payload.locations, [&transfers](const auto& location) {
                         return std::ranges::any_of(transfers.results(), [&](const auto& result) {
                           return result.source == location;
                         });
                       });
    if (claim && !m_page.clipboard().claimMove(selection->revision)) {
      return;
    }
    if (!transfers.retryFailed()) {
      if (claim) {
        m_page.clipboard().finishMove(*selection, {});
      }
    } else if (claim) {
      finishMove(*job, std::move(*selection));
    }
  });
}

std::shared_ptr<OperationJob> TransferController::startPayloadTransfer(
    luciole::FilePayload payload, const luciole::Location& destination, luciole::TransferAction action
) {
  if (std::ranges::any_of(payload.locations, [](const auto& location) { return location.isTrash(); })) {
    if (action != luciole::TransferAction::Move
        || !destination.isNative()
        || destination.isTrash()
        || !std::ranges::all_of(payload.locations, luciole::isTopLevelTrashItem)) {
      m_page.actionError("Move items directly from Trash into a local folder.");
      return nullptr;
    }
    auto job = m_page.jobs().create<luciole::RestoreService>();
    if (!job->service<luciole::RestoreService>().move(std::move(payload.locations), destination)) {
      m_page.actionError("Could not start moving the items out of Trash.");
      return nullptr;
    }
    m_page.setActionMessage({});
    return job;
  }
  if (payload.locations.empty()
      || !luciole::transferLocationSupported(destination)
      || std::ranges::any_of(payload.locations, [](const auto& location) {
           return !luciole::transferLocationSupported(location);
         })) {
    m_page.actionError("File transfers support local and mounted SFTP, SMB or WebDAV folders outside Trash.");
    return nullptr;
  }
  m_page.setActionMessage({});
  auto job = start({std::move(payload.locations), destination, action});
  if (!job) {
    m_page.actionError("Another file transfer is still finishing.");
  }
  return job;
}

void TransferController::updateOperationProgress() {
  if (!m_page.jobs().newest([](const OperationJob& job) {
        return (job.kind() == JobKind::BatchRename && job.service<luciole::BatchRenameService>().applying())
            || (job.kind() == JobKind::Permission && job.running());
      })) {
    m_operationProgress.stop();
  } else if (!m_operationProgress.active()) {
    // The tick checks again: a job that moved to another page sends this page no further change.
    m_operationProgress.startRepeating(std::chrono::milliseconds(100), [this] {
      m_progressTick();
      updateOperationProgress();
    });
  }
}

void TransferController::stopProgress() {
  m_operationProgress.stop();
  m_extraction.cancel();
  closeNewFolder();
}
