#include "app/page/archive_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "config/keybinds.h"
#include "config/settings.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "fs/archive_type.h"
#include "fs/filename.h"
#include "operations/archive_service.h"
#include "platform/desktop_integration.h"
#include "platform/file_launcher.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_error_text.h"
#include "ui/controls/archive_view.h"
#include "ui/dialogs/archive_dialog.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <ranges>
#include <utility>

ArchiveController::ArchiveController(
    PageContext& page, luciole::FileLauncher& launcher, luciole::FileIconCache& icons, WaylandConnection& connection
)
    : m_page(page), m_launcher(launcher), m_icons(icons), m_connection(connection) {}

ArchiveController::~ArchiveController() {
  for (const auto& frame : std::views::reverse(m_frames)) {
    frame.job->service<luciole::ArchiveService>().shutdown();
  }
  for (const auto& job : m_retiredBrowsers) {
    job->service<luciole::ArchiveService>().shutdown();
  }
  if (m_rootBrowser) {
    m_rootBrowser->service<luciole::ArchiveService>().shutdown();
  }
}

void ArchiveController::compress(const std::vector<luciole::DirectoryEntry>& targets) {
  std::vector<luciole::Location> sources;
  sources.reserve(targets.size());
  for (const auto& target : targets) {
    sources.push_back(target.location());
  }
  openDialog(std::move(sources), {}, true);
}

void ArchiveController::extractTo(std::vector<luciole::Location> archives) {
  openDialog(std::move(archives), {}, false);
}

void ArchiveController::extractHere(std::vector<luciole::Location> archives) {
  if (archives.empty()) {
    return;
  }
  const bool started = archives.size() == 1
      ? extract(archives.front(), destination(archives.front()), {})
      : m_page.jobs().create<luciole::ArchiveService>()->service<luciole::ArchiveService>().extractMany(
            std::move(archives)
        );
  if (!started) {
    m_page.actionError("Could not start extraction.");
  }
}

void ArchiveController::browse(const luciole::Location& archive) {
  close();
  m_retiredBrowsers.reserve(m_retiredBrowsers.size() + m_frames.size());
  for (const auto& frame : m_frames | std::views::drop(1)) {
    m_retiredBrowsers.push_back(frame.job);
  }
  m_frames.clear();
  if (!m_rootBrowser) {
    m_rootBrowser = m_page.jobs().create<luciole::ArchiveService>(JobScope::Page);
  }
  m_browser = m_rootBrowser;
  m_frames.push_back({m_browser, archive, std::nullopt});
  if (m_browser->service<luciole::ArchiveService>().inspect(archive)) {
    m_wanted = true;
    m_dirty = true;
  } else {
    m_page.actionError("Could not read this archive.");
  }
}

void ArchiveController::openNested(luciole::ArchiveMember member) {
  if (!m_wanted || !m_browser) {
    return;
  }
  m_browser = m_page.jobs().create<luciole::ArchiveService>(JobScope::Page);
  m_frames.push_back({m_browser, member.location, std::nullopt});
  auto& frame = m_frames.back();
  if (!m_browser->service<luciole::ArchiveService>().inspect(frame.source, std::move(member.lease))) {
    visit(m_frames.size() - 2);
    m_page.actionError("Could not read this archive.");
  }
  if (m_view) {
    m_page.dispatcher().setFocus(m_view->focusArea());
  }
  m_dirty = true;
}

void ArchiveController::visit(std::size_t index) {
  if (index + 1 >= m_frames.size()) {
    return;
  }
  m_pendingMember.reset();
  while (m_frames.size() > index + 1) {
    m_frames.back().job->cancel();
    m_retiredBrowsers.push_back(m_frames.back().job);
    m_frames.pop_back();
  }
  m_browser = m_frames.back().job;
  if (m_view) {
    m_page.dispatcher().setFocus(m_view->focusArea());
  }
  m_restorePosition = true;
  m_dirty = true;
}

bool ArchiveController::extract(
    const luciole::Location& source, const luciole::Location& destination, std::vector<std::string> selected
) {
  // The archive browser's catalog of `source`, if any, guards against a changed archive.
  auto expected = m_browser ? m_browser->service<luciole::ArchiveService>().catalog() : nullptr;
  return m_page.jobs().create<luciole::ArchiveService>()->service<luciole::ArchiveService>().extract(
      source, destination, std::move(selected), std::move(expected)
  );
}

OperationJob* ArchiveController::shown() const {
  auto* running =
      m_page.jobs().newest([](const OperationJob& job) { return job.kind() == JobKind::Archive && job.running(); });
  return running ? running : m_browser.get();
}

void ArchiveController::changed(OperationJob& job) {
  auto& archives = job.service<luciole::ArchiveService>();
  m_dirty = true;
  if (m_wanted && &job == m_browser.get()) {
    if (auto member = archives.takeOpenedMember()) {
      if (member->archive) {
        m_pendingMember = std::move(member);
      } else {
        m_launcher.open(member->location);
      }
    }
  }
  const bool retired =
      std::ranges::any_of(m_retiredBrowsers, [&job](const auto& browser) { return browser.get() == &job; });
  if (!retired && !archives.busy() && job.takeResult()) {
    std::string message = luciole::fileErrorText(archives.error());
    const bool finished = !archives.results().empty();
    if (finished && message.empty()) {
      message = "Archive operation complete.";
    }
    m_page.setActionMessage(std::move(message));
    if (finished) {
      m_page.browser().refresh();
    }
  }
}

void ArchiveController::onFrame() {
  if (auto member = std::exchange(m_pendingMember, std::nullopt)) {
    openNested(std::move(*member));
  }
  if (std::exchange(m_dirty, false)) {
    sync();
  }
  // Creating the archive overlay closes old popups. Present a fast worker's
  // password request afterward so that creation cannot cancel the new dialog.
  syncPassword();
}

luciole::Location ArchiveController::containingFolder(const luciole::Location& source) const {
  if (!m_frames.empty() && m_frames.back().source == source) {
    return m_frames.front().source.parent().value_or(m_frames.front().source);
  }
  return source.parent().value_or(source);
}
luciole::Location ArchiveController::destination(const luciole::Location& source) const {
  return containingFolder(source).child(luciole::archiveFolderName(source));
}

void ArchiveController::openDialog(
    std::vector<luciole::Location> sources, std::vector<std::string> selected, bool compress
) {
  if (sources.empty() || m_page.jobs().newest([](const OperationJob& job) {
        return job.kind() == JobKind::Archive && job.running();
      })) {
    return;
  }
  const auto formats = luciole::archiveFormats();
  if (compress && formats.empty()) {
    m_page.actionError("No archive writer is available.");
    return;
  }
  const auto source = sources.front();
  const auto parent = source.parent();
  if (!parent) {
    m_page.actionError("The source has no destination parent.");
    return;
  }
  const auto base = compress ? *parent : containingFolder(source);
  const auto archiveCount = compress ? 1 : sources.size();
  const bool batch = archiveCount > 1;
  const std::string name = (sources.size() == 1 ? source.displayName().value_or("Archive") : "Archive")
      + (formats.empty() ? ".zip" : std::string(formats.front().suffix));
  auto root = std::make_unique<ArchiveDialog>(
      compress, name, luciole::locationEditText(compress || batch ? base : destination(source)),
      [this, sources = std::move(sources), selected = std::move(selected), base, compress](
          const std::string& name, const std::string& path, luciole::ArchiveFormat format,
          luciole::ArchiveCompressionLevel level, const std::string& password
      ) -> std::string {
        if (path.empty()) {
          return "Enter a destination folder.";
        }
        auto destination = luciole::resolveLocationText(path, base);
        if (!destination.nativePath()) {
          return "Choose a native destination folder.";
        }
        if (compress) {
          if (auto problem = luciole::filenameProblem(name)) {
            return *problem;
          }
          const auto formats = luciole::archiveFormats();
          const auto found = std::ranges::find(formats, format, &luciole::ArchiveFormatInfo::format);
          if (found == formats.end() || !name.ends_with(found->suffix)) {
            return "Use the filename extension for the selected format.";
          }
          destination = destination.child(name);
        }
        const bool started = compress
            ? m_page.jobs().create<luciole::ArchiveService>()->service<luciole::ArchiveService>().compress(
                  sources, destination, format, level, password
              )
            : sources.size() > 1
            ? m_page.jobs().create<luciole::ArchiveService>()->service<luciole::ArchiveService>().extractMany(
                  sources, destination
              )
            : extract(sources.front(), destination, selected);
        if (!started) {
          return "Could not start the archive operation.";
        }
        m_page.dismissPopup();
        return {};
      },
      [this] { m_page.dismissPopup(); }, archiveCount
  );
  auto* dialog = root.get();
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface()) {
    m_page.popupDispatcher().setFocus(dialog->focusArea());
    m_page.popupDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
      if (auto* form = dynamic_cast<ArchiveDialog*>(m_page.activePopup().root())) {
        form->reveal(focused);
      }
    });
  }
}

void ArchiveController::syncPassword() {
  if (!m_page.popupAvailable()) {
    return;
  }
  if (m_passwordDialog) {
    const auto* job = m_page.jobs().find(m_passwordDialog->job);
    const auto pending = job ? job->service<luciole::ArchiveService>().passwordRequest() : std::nullopt;
    if (!pending || pending->id != m_passwordDialog->request) {
      m_passwordDialog.reset();
      m_page.dismissPopup();
    }
    return;
  }
  if (m_page.popupSurface()) {
    return;
  }
  auto* job = m_page.jobs().oldest([](const OperationJob& candidate) {
    return candidate.kind() == JobKind::Archive
        && candidate.service<luciole::ArchiveService>().passwordRequest().has_value();
  });
  const auto request = job ? job->service<luciole::ArchiveService>().passwordRequest() : std::nullopt;
  if (!request) {
    return;
  }
  const PasswordDialog active{job->id(), request->id};
  auto root = std::make_unique<ArchivePasswordDialog>(
      request->source.displayName().value_or("Archive"), request->retry,
      [this, active](std::string password) {
        auto* owner = m_page.jobs().find(active.job);
        if (owner && owner->service<luciole::ArchiveService>().providePassword(active.request, std::move(password))) {
          m_passwordDialog.reset();
          m_page.dismissPopup();
          return true;
        }
        return false;
      },
      [this] { m_page.dismissPopup(); }, request->format
  );
  auto* dialog = root.get();
  m_page.showDialog(std::move(root), DialogWidth::Small);
  if (m_page.popupSurface()) {
    m_passwordDialog = active;
    m_page.activePopup().bind(ActivePopup::Kind::ArchivePassword, [this] {
      if (const auto active = std::exchange(m_passwordDialog, std::nullopt)) {
        if (auto* owner = m_page.jobs().find(active->job)) {
          owner->cancel();
        }
      }
    });
    m_page.popupDispatcher().setFocus(dialog->focusArea());
  } else {
    job->cancel();
  }
}

void ArchiveController::deferClose() {
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime] {
    if (!lifetime.expired()) {
      close();
    }
  });
}

void ArchiveController::close() {
  m_wanted = false;
  m_pendingMember.reset();
  for (const auto& frame : m_frames) {
    frame.job->cancel();
  }
  m_restorePosition = false;
  if (!m_view) {
    return;
  }
  m_page.closePopup();
  m_page.dispatcher().cancelPointerCapture();
  m_page.focusFileList();
  auto* view = std::exchange(m_view, nullptr);
  auto removed = m_page.sceneRoot().removeChild(view);
  m_page.surface().requestLayout();
}

void ArchiveController::sync() {
  UiPhaseScope phase(UiPhase::Layout);
  if (m_wanted && !m_view) {
    m_page.prepareOverlay();
    auto root = std::make_unique<ArchiveView>(
        m_icons, [this] { return m_connection.keyboardModifiers(); }, [this] { deferClose(); },
        [this](std::vector<std::string> selected) {
          if (const auto* catalog =
                  m_browser ? m_browser->service<luciole::ArchiveService>().catalog().get() : nullptr) {
            openDialog({catalog->source}, std::move(selected), false);
          }
        },
        [this](std::vector<std::string> selected) {
          const auto* catalog = m_browser ? m_browser->service<luciole::ArchiveService>().catalog().get() : nullptr;
          if (catalog && !extract(catalog->source, destination(catalog->source), std::move(selected))) {
            m_page.actionError("Could not start extraction.");
          }
        },
        [this](const std::string& path) {
          if (m_browser && !m_browser->service<luciole::ArchiveService>().busy()) {
            m_frames.back().position = m_view->position();
            (void)m_browser->service<luciole::ArchiveService>().openMember(path);
          }
        },
        [this] {
          if (m_browser && !m_frames.empty()) {
            const auto& frame = m_frames.back();
            (void)m_browser->service<luciole::ArchiveService>().inspect(frame.source);
          }
        },
        [this] {
          if (auto* job = shown()) {
            job->cancel();
          }
        },
        [this](std::size_t index) { visit(index); }
    );
    m_view = root.get();
    root->setFill(colorSpecFromRole(ColorRole::Surface, static_cast<float>(m_page.settings().backgroundOpacity)));
    root->setParticipatesInLayout(false);
    root->setZIndex(1000);
    m_page.sceneRoot().addChild(std::move(root));
    m_page.dispatcher().cancelPointerCapture();
    m_page.dispatcher().setFocus(m_view->focusArea());
  }
  if (const auto* job = shown(); m_view && job) {
    const auto& archives = job->service<luciole::ArchiveService>();
    std::vector<std::string> names;
    names.reserve(m_frames.size());
    for (const auto& frame : m_frames) {
      names.push_back(frame.source.displayName().value_or("Archive"));
    }
    m_view->sync(
        m_browser ? m_browser->service<luciole::ArchiveService>().catalog() : nullptr, archives.phase(),
        archives.items(), archives.bytes(), archives.error(), names
    );
    if (m_restorePosition && m_browser && m_browser->service<luciole::ArchiveService>().catalog()) {
      if (m_frames.back().position) {
        m_view->restore(*m_frames.back().position);
      }
      m_restorePosition = false;
    }
    m_page.surface().requestLayout();
  }
}

bool ArchiveController::key(const KeyboardEvent& event) {
  if (!m_view) {
    return false;
  }
  if (event.pressed && !event.preedit) {
    if (!event.repeat && luciole::keybindMatches(luciole::KeybindCommand::Cancel, event.sym, event.modifiers)) {
      deferClose();
    } else if (!event.repeat && luciole::keybindMatches(luciole::KeybindCommand::Back, event.sym, event.modifiers)) {
      if (m_frames.size() > 1) {
        visit(m_frames.size() - 2);
      }
    } else if (
        !event.repeat && luciole::keybindMatches(luciole::KeybindCommand::FocusNext, event.sym, event.modifiers)
    ) {
      (void)m_page.dispatcher().cycleTabFocusInSubtree(m_view, false);
    } else if (
        !event.repeat && luciole::keybindMatches(luciole::KeybindCommand::FocusPrevious, event.sym, event.modifiers)
    ) {
      (void)m_page.dispatcher().cycleTabFocusInSubtree(m_view, true);
    } else if (m_page.dispatcher().focusedArea() != m_view->focusArea() || !m_view->key(event.sym, event.modifiers)) {
      m_page.dispatcher().keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    }
  } else {
    m_page.dispatcher().keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  }
  m_page.surface().requestLayout();
  return true;
}

void ArchiveController::layout(Renderer& renderer, LayoutRect bounds) {
  if (m_view) {
    m_view->arrange(renderer, bounds);
  }
}
