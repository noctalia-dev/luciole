#include "app/page/preview_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/application_chooser_controller.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "config/settings.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "platform/desktop_integration.h"
#include "render/render_context.h"
#include "thumbnails/preview_service.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/browser/preview_layout.h"
#include "ui/builders.h"
#include "ui/controls/file_preview_view.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <utility>

using Action = luciole::BrowserAction;

PreviewController::PreviewController(
    PageContext& page, luciole::PreviewService& preview, luciole::ThumbnailService& thumbnails,
    luciole::FileIconCache& icons, RenderContext& renderer, ApplicationChooserController& applications
)
    : m_page(page), m_preview(preview), m_thumbnails(thumbnails), m_icons(icons), m_renderer(renderer),
      m_applications(applications) {}

void PreviewController::sync() {
  auto* surface = m_page.menuSurface();
  const bool quick = m_quick && surface;
  if ((!m_bounds && !quick) || !m_page.active() || m_page.overlay() || m_page.thumbnailsPaused()) {
    m_preview.clear();
    return;
  }
  auto& model = m_page.browser().model();
  const auto selected = model.selectedIds();
  const auto* entry = selected.size() == 1 && model.selectionAvailable() ? model.entry(selected.front()) : nullptr;
  const auto prepared = entry ? m_thumbnails.prepared(*entry) : nullptr;
  const auto& current = m_preview.snapshot();
  const auto image = entry
          && current.entry
          && entry->id == current.entry->id
          && entry->location() == current.entry->location()
          && current.image
          && current.image->pixels
      ? current.image
      : prepared;
  const auto* pixels = image && image->pixels ? &*image->pixels : nullptr;
  const float width = quick ? static_cast<float>(surface->width()) : m_bounds->width;
  const float height = quick ? static_cast<float>(surface->height()) : m_bounds->height;
  const auto& renderer = quick ? surface->renderTarget().renderer() : m_page.surface().renderTarget().renderer();
  const auto layout = luciole::previewImageLayout(
      pixels ? pixels->width : 0, pixels ? pixels->height : 0, width, height, renderer.renderScale(), quick
  );
  m_preview.show(entry ? std::optional(*entry) : std::nullopt, layout.pixels, m_page.settings().thumbnails, prepared);
}

void PreviewController::openPreviewed(bool with) {
  const auto entry = m_preview.snapshot().entry;
  if (!entry) {
    return;
  }
  const auto request = m_page.requestFor({*entry});
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime, request, entry, with] {
    if (lifetime.expired() || !m_page.active()) {
      return;
    }
    if (!m_page.actionState(with ? Action::OpenWith : Action::Open, request).enabled) {
      m_page.actionError("The previewed item changed. Select it again.");
      return;
    }
    if (with) {
      m_applications.show(*entry);
    } else {
      m_page.dismissMenu();
      m_page.dispatchAction(Action::Open, request);
    }
  });
}

std::unique_ptr<FilePreviewView> PreviewController::makeView(bool quick) {
  return std::make_unique<FilePreviewView>(
      m_icons,
      [this, quick] {
        if (quick) {
          m_page.dismissMenu();
        } else {
          m_page.dispatchAction(Action::ShowPreview);
        }
      },
      [this] { openPreviewed(false); }, [this] { openPreviewed(true); }
  );
}

void PreviewController::showQuick() {
  auto& surface = m_page.surface();
  const float width = std::min(588.0F, static_cast<float>(surface.width()) - 32);
  const float height = std::min(620.0F, static_cast<float>(surface.height()) - 48);
  auto view = makeView(true);
  auto* control = view.get();
  view->setFlexGrow(1);
  auto root = ui::column({.align = FlexAlign::Stretch, .height = height}, std::move(view));
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface()) {
    m_page.activeDialog().bind(ActiveDialog::Kind::QuickPreview, [this] { m_quick = nullptr; });
    m_quick = control;
    sync();
  }
}

bool PreviewController::quickKey(const KeyboardEvent& event) {
  if (!m_quick || !event.pressed || event.preedit || event.modifiers != 0) {
    return false;
  }
  if (luciole::shortcutMatches(Action::QuickPreview, event.sym, event.modifiers) || KeySymbol::isEscape(event.sym)) {
    m_page.dismissMenu();
    return true;
  }
  if (KeySymbol::isLeft(event.sym) || KeySymbol::isRight(event.sym)) {
    auto& model = m_page.browser().model();
    const auto& rows = model.rows();
    const auto selected = model.selectedIds();
    if (selected.size() == 1) {
      const auto found = std::ranges::find(rows, selected.front());
      if (found != rows.end()) {
        const auto index = static_cast<std::ptrdiff_t>(found - rows.begin()) + (KeySymbol::isLeft(event.sym) ? -1 : 1);
        if (index >= 0 && static_cast<std::size_t>(index) < rows.size()) {
          model.select(rows[static_cast<std::size_t>(index)]);
        }
      }
    }
    sync();
    m_page.menuSurface()->requestLayout();
    return true;
  }
  return false;
}

void PreviewController::layoutPane(bool show) {
  auto& surface = m_page.surface();
  if (m_bounds && show) {
    if (!m_view) {
      auto view = makeView(false);
      m_view = view.get();
      view->setParticipatesInLayout(false);
      view->setZIndex(20);
      m_page.sceneRoot().addChild(std::move(view));
    }
    const auto& snapshot = m_preview.snapshot();
    m_view->setVisible(true);
    m_view->sync(
        snapshot, m_bounds->width, m_bounds->height,
        snapshot.entry ? m_page.browser().model().typeLabel(snapshot.entry->id) : ""
    );
    if (!snapshot.image || !snapshot.image->pixels) {
      m_view->clear(surface.renderTarget().renderer());
    }
    m_view->arrange(surface.renderTarget().renderer(), *m_bounds);
  } else if (m_view) {
    m_view->setVisible(false);
    m_view->clear(surface.renderTarget().renderer());
  }
}

void PreviewController::hidePane() {
  m_preview.clear();
  if (m_view) {
    m_view->setVisible(false);
  }
  if (m_view && m_renderer.makeCurrent(m_page.surface().renderTarget())) {
    m_view->clear(m_page.surface().renderTarget().renderer());
  }
}

void PreviewController::layoutQuick() {
  if (!m_quick) {
    return;
  }
  auto* surface = m_page.menuSurface();
  auto& renderer = surface->renderTarget().renderer();
  const auto width = static_cast<float>(surface->width());
  const auto height = static_cast<float>(surface->height());
  const auto& snapshot = m_preview.snapshot();
  m_quick->sync(
      snapshot, width, height, snapshot.entry ? m_page.browser().model().typeLabel(snapshot.entry->id) : "", true
  );
  if (!snapshot.image || !snapshot.image->pixels) {
    m_quick->clear(renderer);
  }
  m_page.activeDialog().root()->arrange(renderer, {0, 0, width, height});
}
