#include "browser/tab_workspace.h"

#include <algorithm>
#include <cmath>

luciole::WindowId luciole::TabWorkspace::addWindow() {
  if (m_windows.size() >= 8) {
    return 0;
  }
  const auto id = ++m_nextWindow;
  m_windows.emplace(id, TabWindow{});
  return id;
}
void luciole::TabWorkspace::save(TabWindow& window) {
  if (window.selected) {
    m_layouts.at(window.selected) = static_cast<const TabLayout&>(window);
  }
}
void luciole::TabWorkspace::load(TabWindow& window, TabId tab) {
  window.selected = tab;
  static_cast<TabLayout&>(window) = tab ? m_layouts.at(tab) : TabLayout{};
}
luciole::TabId luciole::TabWorkspace::addTab(WindowId id, bool activate) {
  auto found = m_windows.find(id);
  if (found == m_windows.end()) {
    return 0;
  }
  std::size_t count = 0;
  for (const auto& [key, window] : m_windows) {
    (void)key;
    count += window.tabs.size();
  }
  if (count >= tabLimit) {
    return 0;
  }
  const auto tab = ++m_nextTab;
  m_layouts.emplace(tab, TabLayout{.primary = tab, .active = tab, .secondary = std::nullopt});
  auto& window = found->second;
  save(window);
  window.tabs.push_back(tab);
  if (activate || !window.selected) {
    load(window, tab);
  }
  return tab;
}
std::optional<luciole::TabId> luciole::TabWorkspace::tabFor(TabId page) const {
  for (const auto& [id, layout] : m_layouts) {
    if (page == id || page == layout.primary || page == layout.secondary) {
      return id;
    }
  }
  return std::nullopt;
}
std::vector<luciole::TabId> luciole::TabWorkspace::pages(TabId page) const {
  const auto tab = tabFor(page);
  if (!tab) {
    return {};
  }
  const auto& layout = m_layouts.at(*tab);
  std::vector<TabId> result{layout.primary};
  if (layout.secondary) {
    result.push_back(*layout.secondary);
  }
  return result;
}
std::optional<luciole::WindowId> luciole::TabWorkspace::owner(TabId page) const {
  const auto tab = tabFor(page);
  if (!tab) {
    return std::nullopt;
  }
  for (const auto& [id, window] : m_windows) {
    if (std::ranges::contains(window.tabs, *tab)) {
      return id;
    }
  }
  return std::nullopt;
}
bool luciole::TabWorkspace::select(WindowId id, TabId tab) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || !std::ranges::contains(found->second.tabs, tab)) {
    return false;
  }
  save(found->second);
  load(found->second, tab);
  return true;
}
bool luciole::TabWorkspace::focus(WindowId id, TabId page) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || (page != found->second.primary && page != found->second.secondary)) {
    return false;
  }
  found->second.active = page;
  save(found->second);
  return true;
}
void luciole::TabWorkspace::remove(TabWindow& window, TabId tab) {
  save(window);
  const auto found = std::ranges::find(window.tabs, tab);
  const auto index = static_cast<std::size_t>(found - window.tabs.begin());
  window.tabs.erase(found);
  if (window.selected == tab) {
    load(window, window.tabs.empty() ? 0 : window.tabs[std::min(index, window.tabs.size() - 1)]);
  }
}
bool luciole::TabWorkspace::close(TabId page) {
  const auto id = owner(page), tab = tabFor(page);
  if (!id || !tab) {
    return false;
  }
  remove(m_windows.at(*id), *tab);
  m_closed.push_back(*tab);
  if (m_closed.size() > 16) {
    m_layouts.erase(m_closed.front());
    m_closed.erase(m_closed.begin());
  }
  return true;
}
bool luciole::TabWorkspace::reopen(WindowId id) {
  if (m_closed.empty() || !m_windows.contains(id)) {
    return false;
  }
  std::size_t count = 0;
  for (const auto& [key, window] : m_windows) {
    (void)key;
    count += window.tabs.size();
  }
  if (count >= tabLimit) {
    return false;
  }
  const auto tab = m_closed.back();
  m_closed.pop_back();
  auto& window = m_windows.at(id);
  save(window);
  window.tabs.push_back(tab);
  load(window, tab);
  return true;
}
bool luciole::TabWorkspace::move(TabId page, WindowId destination, std::size_t index) {
  const auto origin = owner(page), tab = tabFor(page);
  if (!origin || !tab || !m_windows.contains(destination)) {
    return false;
  }
  if (*origin == destination) {
    auto& tabs = m_windows.at(destination).tabs;
    std::erase(tabs, *tab);
    tabs.insert(tabs.begin() + static_cast<std::ptrdiff_t>(std::min(index, tabs.size())), *tab);
    return true;
  }
  remove(m_windows.at(*origin), *tab);
  auto& window = m_windows.at(destination);
  save(window);
  window.tabs.insert(window.tabs.begin() + static_cast<std::ptrdiff_t>(std::min(index, window.tabs.size())), *tab);
  load(window, *tab);
  return true;
}
luciole::TabId luciole::TabWorkspace::addPane(WindowId id) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || !found->second.selected || found->second.secondary) {
    return 0;
  }
  const auto page = ++m_nextTab;
  found->second.secondary = page;
  found->second.active = page;
  save(found->second);
  return page;
}
bool luciole::TabWorkspace::unsplit(WindowId id) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end()) {
    return false;
  }
  auto& window = found->second;
  window.primary = window.active;
  window.secondary.reset();
  save(window);
  return true;
}
bool luciole::TabWorkspace::closePane(WindowId id, TabId page) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || !found->second.secondary) {
    return false;
  }
  auto& window = found->second;
  if (page == window.primary) {
    window.primary = *window.secondary;
  } else if (page != window.secondary) {
    return false;
  }
  window.secondary.reset();
  window.active = window.primary;
  save(window);
  return true;
}
std::optional<luciole::TabLayout> luciole::TabWorkspace::layout(TabId page) const {
  const auto tab = tabFor(page);
  if (!tab) {
    return std::nullopt;
  }
  return m_layouts.at(*tab);
}
bool luciole::TabWorkspace::swap(WindowId id) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || !found->second.secondary) {
    return false;
  }
  std::swap(found->second.primary, *found->second.secondary);
  save(found->second);
  return true;
}
bool luciole::TabWorkspace::resize(WindowId id, float value) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || !std::isfinite(value)) {
    return false;
  }
  found->second.fraction = std::clamp(value, 0.2F, 0.8F);
  save(found->second);
  return true;
}
std::optional<luciole::TabId> luciole::TabWorkspace::other(WindowId id) const {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || !found->second.secondary) {
    return std::nullopt;
  }
  return found->second.active == found->second.primary ? found->second.secondary : std::optional(found->second.primary);
}
bool luciole::TabWorkspace::removeWindow(WindowId id) {
  const auto found = m_windows.find(id);
  if (found == m_windows.end() || !found->second.tabs.empty()) {
    return false;
  }
  m_windows.erase(found);
  return true;
}
