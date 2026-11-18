#include "app/page/operations_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_popup.h"
#include "app/page/page_context.h"
#include "clipboard/text_clipboard.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/operations_view.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "wayland/popup_surface.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <format>

OperationsController::OperationsController(
    PageContext& page, OperationHub& hub, std::function<void(std::uint64_t, bool)> route
)
    : m_page(page), m_hub(hub), m_route(std::move(route)) {}

std::unique_ptr<Button> OperationsController::buildButton() {
  return ui::button(
      {.out = &m_button,
       .glyph = "progress",
       .glyphSize = Style::baseGlyphSize,
       .variant = ButtonVariant::Ghost,
       .tooltip = "File operations",
       // A file picker starts no file jobs; progress of browsing windows does not belong in it.
       .visible = !m_page.choosing(),
       .onClick = [this] {
         if (m_page.activePopup().kind() == ActivePopup::Kind::Operations) {
           m_page.dismissPopup();
         } else {
           open();
         }
       }}
  );
}

void OperationsController::markDirty() {
  m_dirty = true;
  m_page.surface().requestUpdate();
}

float OperationsController::popupHeight(std::size_t count) const {
  return std::min(
      {520.0F, static_cast<float>(m_page.surface().height()) - 96.0F,
       160.0F + 116.0F * static_cast<float>(std::min(count, std::size_t{3}))}
  );
}

void OperationsController::onFrame() {
  if (!m_button || !std::exchange(m_dirty, false)) {
    return;
  }
  if (std::exchange(m_reopen, false)) {
    open();
  }
  if (const auto pending = std::exchange(m_pendingDetails, {})) {
    details(*pending);
  }
  auto activities = m_hub.activities();
  const auto active = std::ranges::count_if(activities, [](const auto& item) { return !item.finished; });
  const bool progressing = std::ranges::any_of(activities, [](const auto& item) {
    return !item.finished
        && (item.state == luciole::ActivityState::Preparing || item.state == luciole::ActivityState::Working);
  });
  const bool failed = std::ranges::any_of(activities, [](const auto& item) { return item.failed(); });
  m_button->setGlyph(active ? "progress" : failed ? "alert-circle" : "progress-check");
  m_button->setBadge(active ? std::to_string(active) : "");
  m_button->setTooltip(active ? std::format("File operations · {} active", active) : "File operations");
  if (m_view) {
    m_count = activities.size();
    m_clear->setEnabled(std::ranges::any_of(activities, [](const auto& item) { return item.finished; }));
    if (m_view->sync(std::move(activities))) {
      m_page.popupDispatcher().cancelPointerCapture();
    }
    m_page.popupSurface()->requestLayout();
    if (progressing && !m_progress.active()) {
      m_progress.startRepeating(std::chrono::milliseconds(200), [this] { markDirty(); });
    } else if (!progressing) {
      m_progress.stop();
    }
  }
}

void OperationsController::open() {
  if (!m_page.popupAvailable()) {
    return;
  }
  const auto activities = m_hub.activities();
  m_count = activities.size();
  auto view = std::make_unique<OperationsView>([this](std::uint64_t id, ActivityAction action) { act(id, action); });
  auto* shown = view.get();
  (void)view->sync(activities);
  Button* clear = nullptr;
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("File Operations", "progress", [this] { m_page.dismissPopup(); }), std::move(view),
      ui::separator({}),
      ui::row(
          {.justify = FlexJustify::End},
          ui::button(
              {.out = &clear,
               .text = "Clear finished",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Ghost,
               .onClick = [this] { m_hub.clearFinishedActivities(); }}
          )
      )
  );
  luciole::applyDialogStyle(*root);
  m_page.showPopup(
      std::move(root), nullptr, sceneRect(*m_button),
      std::min(484.0F, static_cast<float>(m_page.surface().width()) - 32.0F), popupHeight(activities.size()), true
  );
  if (!m_page.popupSurface()) {
    return;
  }
  m_parentSize = {m_page.surface().width(), m_page.surface().height()};
  m_view = shown;
  m_clear = clear;
  m_page.activePopup().bind(ActivePopup::Kind::Operations, [this] { closed(); });
  m_page.popupDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focus) {
    revealFocus(focus);
    if (m_page.popupSurface()) {
      m_page.popupSurface()->requestLayout();
    }
  });
  luciole::focusFirstDialogControl(m_page.popupDispatcher());
  markDirty();
}

void OperationsController::closed() {
  m_progress.stop();
  m_pendingDetails.reset();
  m_reopen = false;
  m_anchor.reset();
  m_view = nullptr;
  m_clear = nullptr;
  m_detailsScroll = nullptr;
  m_reportLabel = nullptr;
}

void OperationsController::act(std::uint64_t id, ActivityAction action) {
  if (action == ActivityAction::Cancel) {
    (void)m_hub.cancelActivity(id);
    markDirty();
  } else if (action == ActivityAction::Details) {
    m_pendingDetails = id;
    markDirty();
  } else {
    m_page.dismissPopup();
    m_route(id, action == ActivityAction::Retry);
  }
}

void OperationsController::details(std::uint64_t id) {
  const auto report = m_hub.activityReport(id);
  if (!report) {
    return;
  }
  ScrollView* scroll = nullptr;
  Label* label = nullptr;
  auto body = ui::scrollView(
      {.out = &scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F}
  );
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->addChild(
      ui::label({.out = &label, .text = *report, .maxWidth = 500.0F, .maxLines = 8192, .configure = [](Label& text) {
                   text.setUseMarkup(false);
                 }})
  );
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      luciole::dialogHeader("Operation Details", "info-circle", [this] { m_page.dismissPopup(); }), std::move(body),
      ui::separator({}),
      ui::row(
          {.justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.text = "Back",
               .variant = ButtonVariant::Ghost,
               .onClick =
                   [this] {
                     m_reopen = true;
                     markDirty();
                   }}
          ),
          ui::button(
              {.text = "Copy report",
               .enabled = m_page.clipboard().available(),
               .variant = ButtonVariant::Ghost,
               .onClick = [this, report] { m_page.clipboard().setClipboardText(*report); }}
          )
      )
  );
  luciole::applyDialogStyle(*root);
  m_page.showDialog(std::move(root), DialogWidth::Medium);
  if (m_page.popupSurface()) {
    m_detailsScroll = scroll;
    m_reportLabel = label;
    m_page.activePopup().bind(ActivePopup::Kind::OperationDetails, [this] { closed(); });
  }
}

void OperationsController::layoutDialog() {
  if (m_view) {
    const auto width =
        static_cast<std::uint32_t>(std::min(484.0F, static_cast<float>(m_page.surface().width()) - 32.0F));
    const auto height = static_cast<std::uint32_t>(popupHeight(m_count));
    auto* popup = m_page.popupSurface();
    const bool resized = popup->width() != width || popup->height() != height;
    if (resized) {
      (void)popup->resize(width, height, false);
    }
    const auto rect = sceneRect(*m_button);
    const std::array anchor{
        static_cast<int>(rect.x), static_cast<int>(rect.y), std::max(1, static_cast<int>(rect.width)),
        std::max(1, static_cast<int>(rect.height))
    };
    if (m_anchor != anchor) {
      m_anchor = anchor;
      (void)popup->repositionAnchor(
          {.anchorX = anchor[0],
           .anchorY = anchor[1],
           .anchorWidth = anchor[2],
           .anchorHeight = anchor[3],
           .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
           .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_LEFT,
           .constraintAdjustment =
               XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y},
          false
      );
    }
  }
  if (m_reportLabel) {
    // The dialog's own width; the popup surface also holds its shadow.
    m_reportLabel->setMaxWidth(std::max(1.0F, m_page.activePopup().root()->width() - 48.0F));
  }
}

void OperationsController::parentConfigured() {
  if (m_view && m_parentSize != std::pair(m_page.surface().width(), m_page.surface().height())) {
    // A toolbar popover closes on window resize; accepted jobs remain independent of its presentation.
    m_page.dismissPopup();
  }
}

void OperationsController::revealFocus(InputArea* focus) {
  if (m_view) {
    m_view->revealFocus(focus);
  }
  if (m_detailsScroll) {
    m_detailsScroll->requestScrollIntoView(focus);
  }
}
