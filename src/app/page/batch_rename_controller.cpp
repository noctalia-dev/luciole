#include "app/page/batch_rename_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "clipboard/text_clipboard.h"
#include "operations/batch_rename_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/file_error_text.h"
#include "ui/controls/input.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/batch_rename_dialog.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

BatchRenameController::BatchRenameController(PageContext& page) : m_page(page) {}

void BatchRenameController::open(const std::vector<luciole::DirectoryEntry>& targets) {
  if (!m_page.actionState(luciole::BrowserAction::Rename, m_page.requestFor(targets)).enabled
      || !m_page.targetsCurrent(targets)) {
    m_page.actionError("This selection cannot be renamed right now.");
    return;
  }
  auto root = std::make_unique<BatchRenameDialog>(
      targets.size(),
      [this](luciole::BatchRenameOptions options) {
        if (m_job) {
          (void)m_job->service<luciole::BatchRenameService>().preview(std::move(options));
        }
      },
      [this] {
        if (m_job && m_job->service<luciole::BatchRenameService>().apply()) {
          m_page.jobs().follow(*m_job);
        }
      },
      [this] {
        if (m_job && m_job->service<luciole::BatchRenameService>().applying()) {
          m_job->cancel();
        } else {
          m_page.dismissMenu();
        }
      },
      [this] { m_page.dismissMenu(); },
      [this] {
        if (m_job) {
          m_page.clipboard().setClipboardText(m_job->service<luciole::BatchRenameService>().snapshot().report);
        }
      },
      [this] {
        if (auto* surface = m_page.menuSurface()) {
          surface->requestUpdate();
        }
      }
  );
  auto* dialog = root.get();
  m_page.showDialog(std::move(root), DialogWidth::Large);
  if (m_page.menuSurface()) {
    m_dialog = dialog;
    m_page.activeDialog().bind(ActiveDialog::Kind::BatchRename, [this] {
      if (m_dialog) {
        // Renaming goes on for the page that takes the job over once this page left its window.
        if (m_page.jobs().attached() || !m_job->service<luciole::BatchRenameService>().applying()) {
          m_job->cancel();
        }
        m_dialog = nullptr;
        m_page.focusFileList();
      }
      m_job.reset();
    });
    m_reported = false;
    m_page.menuDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
      if (m_dialog) {
        m_dialog->reveal(focused);
        m_page.menuSurface()->requestLayout();
      }
    });
    m_page.menuDispatcher().setFocus(dialog->firstInput()->inputArea());
    dialog->firstInput()->selectAll();
    m_job = m_page.jobs().create<luciole::BatchRenameService>(JobScope::Page);
    if (!m_job->service<luciole::BatchRenameService>().prepare(targets)) {
      m_page.actionError("Another rename is still finishing.");
      m_page.dismissMenu();
    }
  }
}

void BatchRenameController::progressTick() {
  m_dirty = m_job && m_job->service<luciole::BatchRenameService>().applying();
}

void BatchRenameController::changed(OperationJob& job) {
  if (&job == m_job.get()) {
    m_dirty = true;
  } else if (!job.running() && job.takeResult()) {
    // A job whose dialog closed.
    report(job);
  }
}

void BatchRenameController::onFrame() {
  if (std::exchange(m_dirty, false) && m_job) {
    sync();
  }
}

void BatchRenameController::sync() {
  const auto& service = m_job->service<luciole::BatchRenameService>();
  const auto& snapshot = service.snapshot();
  if (m_dialog) {
    m_dialog->sync(snapshot, service.completed(), m_page.clipboard().available());
    m_page.menuSurface()->requestLayout();
  }
  if (snapshot.phase == luciole::BatchRenamePhase::Finished && !std::exchange(m_reported, true)) {
    report(*m_job);
  }
}

void BatchRenameController::report(const OperationJob& job) {
  const auto& snapshot = job.service<luciole::BatchRenameService>().snapshot();
  if (snapshot.phase == luciole::BatchRenamePhase::Finished) {
    const auto count = std::ranges::count(
        *snapshot.results, luciole::BatchRenameOutcome::Renamed, &luciole::BatchRenameResult::outcome
    );
    std::string message = job.historyReplay() == HistoryReplay::Undo ? std::format("{} renames undone.", count)
        : job.historyReplay() == HistoryReplay::Redo                 ? std::format("{} renames redone.", count)
                                                                     : std::format("{} items renamed.", count);
    const auto problem =
        std::ranges::find_if(*snapshot.results, [](const auto& result) { return result.error.has_value(); });
    if (problem != snapshot.results->end()) {
      message += " " + luciole::fileErrorText(problem->error);
    }
    m_page.actionError(std::move(message));
    auto& browser = m_page.browser();
    if (snapshot.parent == browser.model().snapshot().location) {
      m_reveal.clear();
      for (const auto& result : *snapshot.results) {
        if (result.actual) {
          m_reveal.push_back(*result.actual);
        }
      }
      browser.refresh();
      m_revealContext = browser.contextSerial();
    }
  } else if (snapshot.phase == luciole::BatchRenamePhase::Error) {
    m_page.actionError(
        (job.historyReplay() == HistoryReplay::Undo       ? "Could not undo batch rename: "
             : job.historyReplay() == HistoryReplay::Redo ? "Could not redo batch rename: "
                                                          : "Could not rename: ")
        + luciole::fileErrorText(snapshot.error)
    );
  }
}

void BatchRenameController::pointer(float x, float y, bool hover) {
  if (m_dialog) {
    m_dialog->tooltipPointer(x, y, hover);
  }
}

void BatchRenameController::syncTooltip(Renderer& renderer) {
  if (m_dialog) {
    m_dialog->syncTooltip(renderer);
  }
}

void BatchRenameController::revealRenamed(VirtualGridView& list) {
  auto& browser = m_page.browser();
  const auto& model = browser.model();
  if (!m_reveal.empty() && model.snapshot().status != luciole::DirectoryStatus::Loading) {
    if (m_revealContext == browser.contextSerial()) {
      std::unordered_set<std::string> uris;
      for (const auto& target : m_reveal) {
        uris.insert(target.uri());
      }
      std::vector<luciole::EntryId> ids;
      for (const auto id : model.rows()) {
        if (const auto* entry = model.entry(id); entry && uris.contains(entry->location().uri())) {
          ids.push_back(id);
        }
      }
      browser.model().replaceSelection(ids);
      if (!ids.empty()) {
        const auto found = std::ranges::find(model.rows(), ids.front());
        list.scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
      }
    }
    // Staging emits several monitor events. Keep reconciling while the results
    // are open, then leave the final selection under normal user control.
    if (!m_dialog || m_revealContext != browser.contextSerial()) {
      m_reveal.clear();
    }
  }
}
