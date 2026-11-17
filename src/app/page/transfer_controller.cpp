#include "app/page/transfer_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/ui_phase.h"
#include "fs/gio_ptr.h"
#include "operations/trash_item.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_error_text.h"
#include "ui/builders.h"
#include "ui/dialogs/dialog_chrome.h"
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

TransferController::TransferController(PageContext& page, std::function<void()> progressTick)
    : m_page(page), m_progressTick(std::move(progressTick)) {}

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
  m_page.clipboard().requestFiles([this, lifetime, request, scope,
                                   destination =
                                       std::move(destination)](std::optional<luciole::ClipboardFiles> selection) {
    if (lifetime.expired()) {
      return;
    }
    const bool pending = std::exchange(m_pendingFilePaste, false);
    if (!pending || !m_page.visible() || !m_page.requestCurrent(request, scope)) {
      return;
    }
    if (!selection) {
      m_page.actionError("The clipboard changed or its file data could not be read.");
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
  const auto choose = [this, lifetime, pending, context](luciole::TransferAction action) {
    if (pending->chosen) {
      return;
    }
    if (lifetime.expired()) {
      pending->cancel();
      return;
    }
    if (!m_page.visible()
        || m_page.operationsBusy()
        || context != m_page.browser().contextSerial()
        || !pending->options.select
        || !pending->options.select(action)) {
      pending->cancel();
      m_page.dismissPopup();
      return;
    }
    pending->chosen = true;
    m_page.dismissPopup();
    resolvedDrop(std::move(pending->payload), pending->destination, action, std::move(pending->finish));
  };
  auto root = ui::column({.align = FlexAlign::Stretch});
  luciole::applyDialogStyle(*root);
  root->addChild(luciole::dialogHeader("Choose Drop Action", "drag-drop", [this] { m_page.dismissPopup(); }));
  Button* focus = nullptr;
  const auto add = [&](const char* label, const char* glyph, luciole::TransferAction action, bool enabled) {
    auto button = ui::button(
        {.text = label,
         .glyph = glyph,
         .contentAlign = ButtonContentAlign::Start,
         .variant = ButtonVariant::Ghost,
         .onClick = [choose, action] { choose(action); }}
    );
    button->setEnabled(enabled);
    if (enabled && !focus) {
      focus = button.get();
    }
    root->addChild(std::move(button));
  };
  add("Copy Here", "copy", luciole::TransferAction::Copy, pending->options.canCopy);
  add("Move Here", "arrows-move", luciole::TransferAction::Move, pending->options.canMove);
  add(pending->payload.locations.size() == 1 ? "Create Link Here" : "Create Links Here", "link",
      luciole::TransferAction::Link,
      pending->options.canCopy && std::ranges::all_of(pending->payload.locations, [&](const auto& source) {
        return luciole::transferLinkTarget(source, destination).has_value();
      }));
  root->addChild(ui::separator({}));
  root->addChild(ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = [this] {
                               m_page.dismissPopup();
                             }}));
  m_page.showDialog(std::move(root), DialogWidth::Small);
  if (m_page.popupSurface()) {
    m_page.activePopup().bind(ActivePopup::Kind::DropAction, [pending] { pending->cancel(); });
    if (focus) {
      m_page.popupDispatcher().setFocus(focus->inputArea());
    }
  }
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
          "Item already exists", "alert-triangle", [choose] { choose(luciole::TransferConflictChoice::Cancel); }
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
               .text = "Keep both",
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
}
