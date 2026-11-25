#include "app/page/create_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "app/page/submenu_controller.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "clipboard/text_clipboard.h"
#include "core/ui_phase.h"
#include "fs/filename.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/browser/file_error_text.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/dialogs/link_dialog.h"
#include "ui/dialogs/name_dialog.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
  std::string createErrorText(const std::optional<luciole::FileError>& error) {
    return error ? luciole::nameErrorText(*error, "The destination folder is no longer available.") : std::string{};
  }
} // namespace

CreateController::CreateController(PageContext& page, luciole::TemplateCatalog& templates, SubmenuController& submenus)
    : m_page(page), m_templates(templates), m_submenus(submenus) {}

bool CreateController::canCreateItem() const {
  return m_page.actionState(luciole::BrowserAction::CreateFolder, m_page.requestFor({})).enabled;
}

void CreateController::showDocumentMenu(
    const luciole::ActionRequest& request, luciole::TemplateListing listing, bool loading
) {
  if (!m_page.actionState(luciole::BrowserAction::NewDocument, request).enabled) {
    return;
  }
  auto items = std::make_shared<std::vector<luciole::DocumentTemplate>>(std::move(listing.items));
  const auto& emptyFile = luciole::ActionRegistry::definition(luciole::BrowserAction::CreateFile);
  const auto shortcut = luciole::actionShortcutLabel(luciole::BrowserAction::CreateFile);
  std::vector<ContextMenuControlEntry> entries{
      {.id = -1,
       .label = std::string(emptyFile.label),
       .glyph = std::string(emptyFile.glyph),
       .shortcut = shortcut.empty() ? std::nullopt : std::optional(shortcut)},
      {.label = "", .separator = true}
  };
  for (std::size_t index = 0; index < items->size(); ++index) {
    entries.push_back({.id = static_cast<int>(index), .label = (*items)[index].label, .glyph = "template"});
  }
  if (loading || items->empty() || listing.error) {
    entries.push_back(
        {.label = loading    ? "Loading templates…"
             : listing.error ? luciole::fileErrorText(*listing.error)
                             : "No document templates",
         .enabled = false}
    );
  }
  if (listing.truncated) {
    entries.push_back({.label = "Template list limit reached", .enabled = false});
  }
  m_submenus.show(
      std::move(entries),
      [this, request, items](int id) {
        if (!m_page.actionState(luciole::BrowserAction::NewDocument, request).enabled) {
          return;
        }
        if (id == -1) {
          openCreateItem(*request.folder, luciole::CreateKind::EmptyFile);
        } else if (id >= 0 && static_cast<std::size_t>(id) < items->size()) {
          openCreateItem(*request.folder, luciole::CreateKind::Document, (*items)[static_cast<std::size_t>(id)]);
        }
      },
      300.0F
  );
}

void CreateController::openDocumentMenu(const luciole::ActionRequest& request) {
  if (!m_page.actionState(luciole::BrowserAction::NewDocument, request).enabled) {
    return;
  }
  showDocumentMenu(request, {}, true);
  const auto generation = m_submenus.generation();
  m_templates.load([this, request, generation](luciole::TemplateListing listing) {
    if (generation == m_submenus.generation()) {
      showDocumentMenu(request, std::move(listing));
    }
  });
}

void CreateController::openCreateItem(
    luciole::Location parent, luciole::CreateKind kind, std::optional<luciole::DocumentTemplate> source
) {
  if (!canCreateItem() || m_page.browser().model().snapshot().location != parent) {
    m_page.actionError("Open an available writable folder to create a file or folder.");
    return;
  }
  const bool document = kind == luciole::CreateKind::Document;
  const auto display = source ? luciole::filenameDisplayText(source->name) : std::string{};
  auto editor = std::make_unique<NameDialog>(
      m_page.dialogWidth(DialogWidth::Small),
      kind == luciole::CreateKind::Folder ? "New Folder"
          : document                      ? "New Document"
                                          : "New File",
      kind == luciole::CreateKind::Folder ? "folder-plus" : "file-plus",
      source                                    ? display
          : kind == luciole::CreateKind::Folder ? "New Folder"
                                                : "New File",
      source ? "From " + source->label : "", luciole::filenameProblem,
      [this](std::string name) { submitCreateItem(std::move(name)); }, [this] { m_page.dismissPopup(); }, "Create",
      "Creating…"
  );
  auto* control = editor.get();
  m_page.showDialog(std::move(editor), DialogWidth::Small);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::CreateItem, [this] { closeCreation(); });
    m_createEditor = control;
    m_createParent = std::move(parent);
    m_creationPreparing = true;
    control->setSaving(true);
    m_job = m_page.jobs().create<luciole::CreateService>(JobScope::Page);
    auto& creator = m_job->service<luciole::CreateService>();
    if (!(source ? creator.prepareTemplate(*m_createParent, std::move(*source))
                 : creator.prepare(*m_createParent, kind))) {
      control->showError("Another creation is still finishing.");
    }
  }
}

void CreateController::openImagePaste(luciole::ClipboardImage image, luciole::Location parent) {
  if (!canCreateItem() || m_page.browser().model().snapshot().location != parent) {
    m_page.actionError("Open an available writable folder to paste an image.");
    return;
  }
  auto editor = std::make_unique<NameDialog>(
      m_page.dialogWidth(DialogWidth::Small), "Paste Image", "photo", "Pasted image." + image.extension, "",
      luciole::filenameProblem, [this](std::string name) { submitCreateItem(std::move(name)); },
      [this] { m_page.dismissPopup(); }, "Save", "Saving…"
  );
  auto* control = editor.get();
  m_page.showDialog(std::move(editor), DialogWidth::Small);
  if (m_page.popupSurface() != nullptr) {
    m_page.activePopup().bind(ActivePopup::Kind::CreateItem, [this] { closeCreation(); });
    m_createEditor = control;
    m_createParent = std::move(parent);
    m_creationPreparing = true;
    control->setSaving(true);
    m_job = m_page.jobs().create<luciole::CreateService>(JobScope::Page);
    auto& creator = m_job->service<luciole::CreateService>();
    if (!creator.prepareImage(*m_createParent, std::move(image.bytes))) {
      control->showError("Another creation is still finishing.");
    }
  }
}

void CreateController::submitCreateItem(std::string name) {
  if (m_createEditor == nullptr || !m_createParent || !m_job) {
    return;
  }
  auto& browser = m_page.browser();
  if (browser.model().snapshot().location != m_createParent || !browser.searchQuery().empty()) {
    m_createEditor->showError("The destination changed. Close this dialog and try again.");
    return;
  }
  auto& creator = m_job->service<luciole::CreateService>();
  if (creator.create(std::move(name))) {
    m_page.jobs().follow(*m_job);
    m_createEditor->setSaving(true);
  } else {
    m_createEditor->showError(
        creator.snapshot().error ? createErrorText(creator.snapshot().error)
                                 : "The destination is unavailable. Close this dialog and try again."
    );
  }
}

void CreateController::openLinkDialog(const luciole::DirectoryEntry& target) {
  if (!m_page.actionState(luciole::BrowserAction::CreateLink, m_page.requestFor({target})).enabled
      || !m_page.targetsCurrent({target})) {
    m_page.actionError("This item cannot be linked right now.");
    return;
  }
  const auto location = target.location();
  const auto parent = location.parent();
  if (!parent) {
    m_page.actionError("The source has no containing folder.");
    return;
  }
  const auto& base = *parent;
  const float width = m_page.dialogWidth(DialogWidth::Medium);
  auto root = std::make_unique<LinkDialog>(
      width, luciole::locationEditText(location), "Link to " + target.displayName(), luciole::locationEditText(base),
      target.type == luciole::FileType::Regular && !target.symlink,
      [this, base](std::string name, const std::string& path, luciole::CreateKind kind) {
        if (!m_linkDialog || !m_job) {
          return;
        }
        try {
          const auto parent = luciole::resolveLocationText(path, base);
          auto& creator = m_job->service<luciole::CreateService>();
          if (creator.createLink(parent, std::move(name), kind)) {
            m_page.jobs().follow(*m_job);
            m_linkDialog->setSaving(true);
          } else {
            m_linkDialog->showError(
                creator.snapshot().error ? createErrorText(creator.snapshot().error)
                                         : "The source is unavailable. Close this dialog and try again."
            );
          }
        } catch (const std::exception& error) {
          m_linkDialog->showError(error.what());
        }
        m_page.popupSurface()->requestLayout();
      },
      [this] { m_page.dismissPopup(); }
  );
  auto* dialog = root.get();
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface()) {
    m_page.activePopup().bind(ActivePopup::Kind::Link, [this] { closeCreation(); });
    m_linkDialog = dialog;
    m_linkPreparing = true;
    m_page.popupDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
      if (m_linkDialog) {
        m_linkDialog->reveal(focused);
        m_page.popupSurface()->requestLayout();
      }
    });
    dialog->setSaving(true);
    m_job = m_page.jobs().create<luciole::CreateService>(JobScope::Page);
    if (!m_job->service<luciole::CreateService>().prepareLink(target)) {
      dialog->setSaving(false);
      dialog->showError("Another creation is still finishing.");
    }
  }
}

void CreateController::changed(OperationJob& job) {
  if (&job == m_job.get()) {
    m_dirty = true;
  } else if (!job.running() && job.takeResult()) {
    // A job whose dialog closed.
    sync(job);
  }
}

void CreateController::onFrame() {
  if (std::exchange(m_dirty, false) && m_job) {
    sync(*m_job);
  }
}

void CreateController::sync(const OperationJob& job) {
  const auto& creator = job.service<luciole::CreateService>();
  if (creator.busy()) {
    return;
  }
  const bool dialog = &job == m_job.get();
  auto* createEditor = dialog ? m_createEditor : nullptr;
  auto* linkDialog = dialog ? m_linkDialog : nullptr;
  const auto& snapshot = creator.snapshot();
  if (snapshot.status == luciole::CreateStatus::Succeeded || snapshot.status == luciole::CreateStatus::Partial) {
    auto message = createErrorText(snapshot.error);
    if (job.historyReplay() != HistoryReplay::None) {
      message = (snapshot.retireHistory                           ? "Creation reversal needs attention."
                     : job.historyReplay() == HistoryReplay::Undo ? "Creation undone."
                                                                  : "Creation redone.")
          + (message.empty() ? std::string{} : " " + message);
    }
    if (linkDialog && !snapshot.error && snapshot.parent) {
      message = "Link created in " + luciole::locationEditText(*snapshot.parent);
    }
    m_page.setActionMessage(std::move(message));
    auto& browser = m_page.browser();
    if (browser.model().snapshot().location == snapshot.parent) {
      std::vector<luciole::Location> reveal;
      if (snapshot.destination && !snapshot.removed) {
        reveal.push_back(*snapshot.destination);
      }
      m_page.revealOnReload(std::move(reveal));
      browser.refresh();
    }
    if (createEditor != nullptr || linkDialog != nullptr) {
      m_createEditor = nullptr;
      m_page.dismissPopup();
    }
  } else if (linkDialog) {
    linkDialog->setSaving(false);
    linkDialog->showError(createErrorText(snapshot.error));
    if (std::exchange(m_linkPreparing, false)) {
      m_page.popupDispatcher().setFocus(linkDialog->nameInput()->inputArea());
      linkDialog->nameInput()->selectAll();
    }
    m_page.popupSurface()->requestLayout();
  } else if (createEditor == nullptr) {
    if (snapshot.status == luciole::CreateStatus::Error) {
      m_page.actionError(
          (job.historyReplay() == HistoryReplay::Undo       ? "Could not undo creation: "
               : job.historyReplay() == HistoryReplay::Redo ? "Could not redo creation: "
                                                            : "Could not create: ")
          + createErrorText(snapshot.error)
      );
    }
  } else if (snapshot.status == luciole::CreateStatus::Ready || snapshot.status == luciole::CreateStatus::Error) {
    createEditor->setSaving(false);
    createEditor->showError(createErrorText(snapshot.error));
    if (std::exchange(m_creationPreparing, false)) {
      m_page.popupDispatcher().setFocus(createEditor->input()->inputArea());
      if (snapshot.kind == luciole::CreateKind::Document || snapshot.kind == luciole::CreateKind::Image) {
        const auto& value = createEditor->input()->value();
        createEditor->input()->selectRange(0, luciole::filenameStemLength(value, false));
      } else {
        createEditor->input()->selectAll();
      }
    }
    m_page.popupSurface()->requestLayout();
  }
}

void CreateController::closeCreation() {
  if (m_createParent || m_linkDialog) {
    // Creating goes on for the page that takes the job over once this page left its window.
    if (m_job && (m_page.jobs().attached() || !m_job->running())) {
      m_job->cancel();
    }
    m_page.focusFileList();
  }
  m_job.reset();
  m_createEditor = nullptr;
  m_linkDialog = nullptr;
  m_linkPreparing = false;
  m_createParent.reset();
  m_creationPreparing = false;
}
