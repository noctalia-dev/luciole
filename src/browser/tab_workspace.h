#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace luciole {
  using TabId = std::uint64_t;
  using WindowId = std::uint64_t;
  struct TabLayout {
    TabId primary = 0, active = 0;
    std::optional<TabId> secondary;
    float fraction = 0.5F;
  };
  struct TabWindow : TabLayout {
    std::vector<TabId> tabs;
    TabId selected = 0;
  };
  // Each logical tab owns one or two browser pages. A split is never a second tab.
  class TabWorkspace {
  public:
    static constexpr std::size_t tabLimit = 32;
    WindowId addWindow();
    // A background tab leaves the current tab and its split/focus untouched.
    // The first tab of an empty window is always activated.
    TabId addTab(WindowId id, bool activate = true);
    bool removeWindow(WindowId id);
    bool select(WindowId id, TabId tab);
    bool focus(WindowId id, TabId page);
    bool close(TabId page);
    bool reopen(WindowId id);
    bool move(TabId page, WindowId destination, std::size_t index);
    TabId addPane(WindowId id);
    bool unsplit(WindowId id);
    bool closePane(WindowId id, TabId page);
    std::optional<TabLayout> layout(TabId page) const;
    bool swap(WindowId id);
    bool resize(WindowId id, float value);
    std::optional<WindowId> owner(TabId page) const;
    std::optional<TabId> tabFor(TabId page) const;
    std::vector<TabId> pages(TabId page) const;
    const std::map<WindowId, TabWindow>& windows() const { return m_windows; }
    const std::vector<TabId>& closed() const { return m_closed; }
    std::optional<TabId> other(WindowId id) const;

  private:
    void save(TabWindow& window);
    void load(TabWindow& window, TabId tab);
    void remove(TabWindow& window, TabId tab);
    std::map<WindowId, TabWindow> m_windows;
    std::map<TabId, TabLayout> m_layouts;
    std::vector<TabId> m_closed;
    TabId m_nextTab = 0;
    WindowId m_nextWindow = 0;
  };
} // namespace luciole
