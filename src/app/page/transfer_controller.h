#pragma once

#include "browser/action_registry.h"
#include "browser/job_routing.h"
#include "clipboard/text_clipboard.h"
#include "core/timer_manager.h"
#include "fs/transfer_types.h"
#include "platform/extraction_launcher.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace luciole {
  struct TransferRequest;
} // namespace luciole

class Button;
class OperationJob;
class PageContext;

// Copying and moving files: clipboard paste, drops and transfers to another page, the conflict dialog shared with
// archive and restore jobs, retrying failed items, and the progress tick for operation dialogs. It works
// on the jobs the page owns, including the ones it took over from a closed page.
class TransferController {
public:
  // `progressTick` runs every 100 ms while an owned batch/permission dialog needs progress.
  TransferController(PageContext& page, std::function<void()> progressTick);

  // Pastes the clipboard's files into the request's folder once the clipboard answers.
  void paste(const luciole::ActionRequest& request);
  // Pastes the clipboard's files into the request's single target, a folder, once the clipboard answers.
  void pasteInto(const luciole::ActionRequest& request);
  // True from a paste request until the clipboard answers.
  bool pastePending() const { return m_pendingFilePaste; }
  void cancelPaste() { m_pendingFilePaste = false; }
  bool extractionPending() const { return m_extraction.busy(); }
  void cancelExtraction() { m_extraction.cancel(); }
  void
  extractDrop(luciole::ExtractionOffer offer, const luciole::Location& destination, luciole::DropCompletion finish);
  // Copies or moves the request's targets into `destination`.
  void transferTo(const luciole::ActionRequest& request, const luciole::Location& destination, bool copy);
  // Starts a transfer job that copies or moves `sources` into `destination`.
  void transfer(
      std::vector<luciole::Location> sources, const luciole::Location& destination, luciole::TransferAction action
  );
  // Transfers a dropped payload; `finish` reports whether every item arrived.
  void receiveDrop(
      luciole::FilePayload payload, const luciole::Location& destination, luciole::DropOptions options,
      luciole::DropCompletion finish
  );
  // Transfer job change: reports the outcome once the job finishes.
  void transferChanged(OperationJob& job);
  // Shows the oldest pending restore, archive or transfer conflict, or closes the dialog once it is resolved.
  // Frame slot in BrowserPage::prepareFrame() and pumpJobs().
  void syncConflict();
  // Keeps the shown conflict pending when its dialog closes, for a page that is deactivated.
  void keepConflict() { m_activeConflict.reset(); }
  // Runs the progress tick for owned batch rename and permission work. Every owned job change and every tick call this.
  void updateOperationProgress();
  // Retries a completed owned transfer, preserving the clipboard move claim workflow.
  void retry(luciole::JobId target);

  // Page teardown: stop the tick.
  void stopProgress();

private:
  struct ActiveConflict {
    std::uint64_t job = 0;
    std::uint64_t conflict = 0;
  };

  // Pastes into `destination` once the clipboard answers and `request` is still current for `scope`.
  void paste(const luciole::ActionRequest& request, luciole::ActionScope scope, luciole::Location destination);
  std::shared_ptr<OperationJob> start(luciole::TransferRequest request);
  std::shared_ptr<OperationJob> startPayloadTransfer(
      luciole::FilePayload payload, const luciole::Location& destination, luciole::TransferAction action
  );
  void chooseDrop(
      luciole::FilePayload payload, const luciole::Location& destination, luciole::DropOptions options,
      luciole::DropCompletion finish
  );
  void resolvedDrop(
      luciole::FilePayload payload, const luciole::Location& destination, luciole::TransferAction action,
      luciole::DropCompletion finish
  );
  void trashDrop(luciole::FilePayload payload, const luciole::DropOptions& options, luciole::DropCompletion finish);
  void finishMove(OperationJob& job, luciole::ClipboardFiles selection);

  PageContext& m_page;
  std::function<void()> m_progressTick;
  Timer m_operationProgress;
  bool m_pendingFilePaste = false;
  luciole::ExtractionLauncher m_extraction;
  std::optional<ActiveConflict> m_activeConflict;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
