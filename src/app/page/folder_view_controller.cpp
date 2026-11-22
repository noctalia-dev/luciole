#include "app/page/folder_view_controller.h"

#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/folder_views.h"
#include "browser/miller_columns.h"

FolderViewController::FolderViewController(
    PageContext& page, luciole::FolderViewService* views, luciole::BrowserController& browser,
    luciole::MillerColumns& miller
)
    : m_page(page), m_views(views), m_browser(browser), m_miller(miller) {}

bool FolderViewController::enabled() const { return m_views != nullptr && m_views->enabled(); }

bool FolderViewController::customized() const {
  const auto view = m_page.browser().folderView();
  return view && !view->empty();
}

bool FolderViewController::sortCustomized() const {
  const auto view = m_page.browser().folderView();
  return view && view->sort;
}

std::optional<luciole::Location> FolderViewController::target() const {
  return enabled() ? m_page.browser().model().snapshot().location : std::nullopt;
}

bool FolderViewController::saveSort(const luciole::SortOptions& sort) {
  const auto folder = target();
  if (!folder) {
    return false;
  }
  // The service's changed callback applies it to every page showing this folder.
  m_views->setSort(*folder, sort);
  return true;
}

bool FolderViewController::saveViewMode(luciole::ViewMode mode) {
  if (mode == luciole::ViewMode::Miller || m_page.viewMode() == luciole::ViewMode::Miller) {
    return false;
  }
  const auto folder = target();
  if (!folder) {
    return false;
  }
  m_views->setMode(*folder, mode);
  return true;
}

void FolderViewController::reset(const luciole::Location& folder) { m_views->reset(folder); }

void FolderViewController::apply() {
  if (m_views == nullptr) {
    return;
  }
  const auto before = m_page.browser().model().sort();
  m_browser.reapplyFolderView();
  for (const auto& column : m_miller.columns()) {
    column->browser->reapplyFolderView();
  }
  if (m_page.browser().model().sort() != before) {
    m_page.sortChanged();
  }
  if (!m_miller.enabled()) {
    m_page.showViewMode(m_page.browser().viewport().mode);
  }
  m_page.requestSync();
}
