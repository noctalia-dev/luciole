#include "app/page/preview_controller.h"

#include "app/page/active_popup.h"
#include "app/page/application_chooser_controller.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "config/settings.h"
#include "core/deferred_call.h"
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
  auto* surface = m_page.popupSurface();
  const bool quick = m_quickPreview && surface;
  if ((!m_bounds && !quick) || !m_page.active() || m_page.overlay() || m_page.thumbnailsPaused()) {
    m_preview.clear();
    return;
  }
  auto& model = m_page.browser().model();
  const auto selected = model.selectedIds();
  const auto* entry = selected.size() == 1 && model.selectionAvailable() ? model.entry(selected.front()) : nullptr;
  if (entry && (entry->metadataPending || entry->metadataFailed)) {
    m_preview.clear();
    return;
  }
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
  const auto size = quick ? quickPreviewSize() : std::pair{m_bounds->width, m_bounds->height};
  const auto& renderer = quick ? surface->renderTarget().renderer() : m_page.surface().renderTarget().renderer();
  const int pixelSide = luciole::previewImagePixels(
      pixels ? pixels->width : 0, pixels ? pixels->height : 0, size.first, size.second, renderer.renderScale(), quick
  );
  m_preview.show(entry ? std::optional(*entry) : std::nullopt, pixelSide, prepared);
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
      m_page.dismissPopup();
      m_page.dispatchAction(Action::Open, request);
    }
  });
}

std::unique_ptr<FilePreviewView> PreviewController::makeView(bool quick) {
  return std::make_unique<FilePreviewView>(
      m_icons, quick,
      [this, quick] {
        if (quick) {
          m_page.dismissPopup();
        } else {
          m_page.dispatchAction(Action::ShowPreview);
        }
      },
      [this] { openPreviewed(false); }, [this] { openPreviewed(true); }
  );
}

std::pair<float, float> PreviewController::quickPreviewSize() const {
  return {
      m_page.dialogWidth(DialogWidth::Medium),
      std::max(1.0F, std::min(620.0F, static_cast<float>(m_page.surface().height()) - 48))
  };
}

void PreviewController::showQuickPreview() {
  const auto height = quickPreviewSize().second;
  auto view = makeView(true);
  auto* control = view.get();
  view->setFlexGrow(1);
  m_page.showDialog(
      ui::column({.align = FlexAlign::Stretch, .minHeight = height, .maxHeight = height}, std::move(view)),
      DialogWidth::Medium, PopupDismiss::OutsideClick
  );
  if (m_page.popupSurface()) {
    m_page.activePopup().bind(ActivePopup::Kind::QuickPreview, [this] { m_quickPreview = nullptr; });
    m_quickPreview = control;
    sync();
  }
}

bool PreviewController::quickPreviewKey(const KeyboardEvent& event) {
  if (!m_quickPreview || !event.pressed || event.preedit) {
    return false;
  }
  using K = luciole::KeybindCommand;
  const bool cancel = luciole::keybindMatches(K::Cancel, event.sym, event.modifiers);
  const bool up = luciole::keybindMatches(K::MoveUp, event.sym, event.modifiers);
  const bool down = luciole::keybindMatches(K::MoveDown, event.sym, event.modifiers);
  const bool left = luciole::keybindMatches(K::MoveLeft, event.sym, event.modifiers);
  const bool right = luciole::keybindMatches(K::MoveRight, event.sym, event.modifiers);
  const bool navigation = up || down || left || right;
  const bool quickPreview = luciole::shortcutMatches(Action::QuickPreview, event.sym, event.modifiers);
  if (!cancel && !navigation && !quickPreview && event.modifiers != 0) {
    return false;
  }
  if (cancel || quickPreview) {
    m_page.dismissPopup();
    return true;
  }
  // Navigation walks the files as in the view behind: one item left/right, one list row or grid row up/down.
  const bool vertical = up || down;
  const bool previous = left || up;
  if (navigation) {
    auto& model = m_page.browser().model();
    const auto& rows = model.rows();
    const auto selected = model.selectedIds();
    if (selected.size() == 1) {
      const auto found = std::ranges::find(rows, selected.front());
      if (found != rows.end()) {
        const auto stride = vertical && m_page.viewMode() == luciole::ViewMode::Grid && m_page.fileList()
            ? static_cast<std::ptrdiff_t>(std::max<std::size_t>(1, m_page.fileList()->layoutColumnCount()))
            : 1;
        // Clamped like the grid: Down from the last full row lands on the last item.
        const auto index = std::clamp(
            static_cast<std::ptrdiff_t>(found - rows.begin()) + (previous ? -stride : stride), std::ptrdiff_t{0},
            static_cast<std::ptrdiff_t>(rows.size()) - 1
        );
        model.select(rows[static_cast<std::size_t>(index)]);
      }
    }
    sync();
    m_page.popupSurface()->requestLayout();
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
    m_view->setFill(colorSpecFromRole(ColorRole::Surface, static_cast<float>(m_page.settings().backgroundOpacity)));
    m_view->sync(
        snapshot, m_bounds->width, m_bounds->height,
        snapshot.entry ? m_page.browser().model().typeLabel(snapshot.entry->id) : ""
    );
    // Releasing a shown fallback icon every layout would drop its texture before the async upload lands.
    if (!m_view->showsImage()) {
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

void PreviewController::layoutQuickPreview() {
  if (!m_quickPreview) {
    return;
  }
  // Before the dialog measures itself: the preview has a fixed height that follows the window.
  const auto [width, height] = quickPreviewSize();
  auto* root = m_page.activePopup().root();
  root->setMinHeight(height);
  root->setMaxHeight(height);
  const auto& snapshot = m_preview.snapshot();
  m_quickPreview->sync(
      snapshot, width, height, snapshot.entry ? m_page.browser().model().typeLabel(snapshot.entry->id) : ""
  );
  if (!m_quickPreview->showsImage()) {
    m_quickPreview->clear(m_page.popupSurface()->renderTarget().renderer());
  }
}
