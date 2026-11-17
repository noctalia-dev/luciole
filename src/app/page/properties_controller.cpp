#include "app/page/properties_controller.h"

#include "app/operation_hub.h"
#include "app/page/page_context.h"
#include "app/page/properties_window.h"
#include "browser/browser_controller.h"
#include "operations/permission_service.h"
#include "ui/controls/permission_editor.h"

#include <algorithm>

namespace {
  // Each window reads its targets with its own job; this bounds that work and the open windows.
  constexpr std::size_t kMaxWindows = 16;
} // namespace

PropertiesController::PropertiesController(PageContext& page, luciole::FileIconCache& icons)
    : m_page(page), m_icons(icons) {}

PropertiesController::~PropertiesController() = default;

void PropertiesController::show(const std::vector<luciole::DirectoryEntry>& targets, bool captureCurrentLocation) {
  if (targets.empty()) {
    return;
  }
  // A closed window's job may still apply changes; a new window would read the items mid-change.
  if (m_page.jobs().newest([this](const OperationJob& job) {
        return job.kind() == JobKind::Permission && job.running() && owner(job) == nullptr;
      })) {
    m_page.actionError("A Properties request is still finishing.");
    return;
  }
  if (m_windows.size() >= kMaxWindows) {
    m_page.actionError("Close a Properties window first.");
    return;
  }
  auto window = std::make_unique<PropertiesWindow>(m_page, m_icons, [this](PropertiesWindow& closed) {
    std::erase_if(m_windows, [&closed](const auto& item) { return item.get() == &closed; });
  });
  if (window->show(targets, captureCurrentLocation)) {
    m_windows.push_back(std::move(window));
  } else {
    window->close();
    m_page.actionError("Properties could not open a window.");
  }
}

void PropertiesController::changed(OperationJob& job) {
  if (auto* window = owner(job)) {
    window->jobChanged();
  } else {
    reportClosed(job);
  }
}

void PropertiesController::progressTick() {
  for (const auto& window : m_windows) {
    window->progressTick();
  }
}

void PropertiesController::updateApply() {
  for (const auto& window : m_windows) {
    window->updateApply();
  }
}

void PropertiesController::iconsReady() {
  for (const auto& window : m_windows) {
    window->requestLayout();
  }
}

void PropertiesController::setParent(luciole::WindowSurface& parent) {
  for (const auto& window : m_windows) {
    window->setParent(parent);
  }
}

void PropertiesController::closeAll() {
  for (const auto& window : m_windows) {
    window->close();
  }
  m_windows.clear();
}

PropertiesWindow* PropertiesController::owner(const OperationJob& job) const {
  const auto found = std::ranges::find_if(m_windows, [&job](const auto& window) { return window->owns(job); });
  return found == m_windows.end() ? nullptr : found->get();
}

void PropertiesController::reportClosed(OperationJob& job) {
  const auto& permissions = job.service<luciole::PermissionService>();
  const auto& snapshot = permissions.snapshot();
  if (permissions.busy() || !job.takeResult()) {
    return;
  }
  if ((snapshot.phase == luciole::PermissionPhase::Ready || snapshot.phase == luciole::PermissionPhase::Cancelled)
      && snapshot.applied) {
    m_page.browser().refresh();
  }
  if (const auto status = permissionStatusText(snapshot); !status.empty()) {
    m_page.actionError(status);
  }
}
