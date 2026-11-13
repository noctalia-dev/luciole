#include "browser/tab_workspace.h"
#include "test_check.h"

#include <algorithm>
#include <cmath>
#include <print>
#include <random>
#include <set>
#include <stdexcept>

namespace {
  void invariant(const luciole::TabWorkspace& workspace) {
    std::set<luciole::TabId> ids;
    std::size_t tabCount = 0;
    for (const auto& [id, window] : workspace.windows()) {
      tabCount += window.tabs.size();
      if (window.tabs.empty()) {
        TEST_CHECK(!window.selected && !window.primary && !window.active && !window.secondary);
      } else {
        TEST_CHECK(std::ranges::contains(window.tabs, window.selected));
        TEST_CHECK(window.active == window.primary || window.active == window.secondary);
        if (window.secondary) {
          TEST_CHECK(*window.secondary != window.primary);
        }
      }
      TEST_CHECK(std::isfinite(window.fraction) && window.fraction >= 0.2F && window.fraction <= 0.8F);
      for (auto tab : window.tabs) {
        const auto pages = workspace.pages(tab);
        TEST_CHECK(!pages.empty() && pages.size() <= 2);
        for (auto page : pages) {
          TEST_CHECK(ids.insert(page).second && workspace.owner(page) == id && workspace.tabFor(page) == tab);
        }
      }
    }
    TEST_CHECK(tabCount <= 32 && ids.size() <= 64);
    for (auto tab : workspace.closed()) {
      for (auto page : workspace.pages(tab)) {
        TEST_CHECK(!ids.contains(page));
      }
    }
    TEST_CHECK(workspace.closed().size() <= 16);
  }
  void workflows() {
    luciole::TabWorkspace workspace;
    const auto left = workspace.addWindow(), right = workspace.addWindow();
    const auto a = workspace.addTab(left);
    const auto auxiliary = workspace.addPane(left);
    TEST_CHECK(auxiliary && workspace.windows().at(left).tabs.size() == 1 && workspace.tabFor(auxiliary) == a);
    const auto b = workspace.addTab(left);
    TEST_CHECK(!workspace.windows().at(left).secondary);
    workspace.select(left, a);
    TEST_CHECK(workspace.windows().at(left).secondary == auxiliary && workspace.windows().at(left).active == auxiliary);
    TEST_CHECK(workspace.focus(left, a) && workspace.other(left) == auxiliary);
    TEST_CHECK(
        workspace.move(auxiliary, right, 0) && workspace.owner(a) == right && workspace.owner(auxiliary) == right
    );
    TEST_CHECK(workspace.windows().at(left).selected == b && !workspace.select(left, a));
    TEST_CHECK(workspace.close(auxiliary) && !workspace.owner(a) && workspace.reopen(left));
    TEST_CHECK(workspace.windows().at(left).secondary == auxiliary);
    TEST_CHECK(workspace.swap(left) && workspace.windows().at(left).active == a);
    TEST_CHECK(
        workspace.unsplit(left)
        && workspace.windows().at(left).tabs.size() == 2
        && workspace.windows().at(left).primary == a
        && !workspace.tabFor(auxiliary)
    );
    TEST_CHECK(workspace.move(a, left, 0) && workspace.windows().at(left).tabs.front() == a);
    invariant(workspace);
  }
  void backgroundTabs() {
    luciole::TabWorkspace workspace;
    const auto window = workspace.addWindow(), other = workspace.addWindow();
    const auto original = workspace.addTab(window, false);
    TEST_CHECK(original && workspace.windows().at(window).selected == original);
    const auto split = workspace.addPane(window);
    TEST_CHECK(workspace.resize(window, 0.35F));
    const auto before = workspace.windows().at(window);
    const auto background = workspace.addTab(window, false);
    const auto& after = workspace.windows().at(window);
    TEST_CHECK(background && after.tabs == std::vector({original, background}));
    TEST_CHECK(
        after.selected == before.selected
        && after.primary == before.primary
        && after.secondary == split
        && after.active == split
        && after.fraction == before.fraction
    );
    const auto firstOther = workspace.addTab(other, false);
    TEST_CHECK(firstOther && workspace.windows().at(window).selected == original);
    TEST_CHECK(workspace.close(background) && workspace.windows().at(window).active == split);
    TEST_CHECK(workspace.reopen(window) && workspace.windows().at(window).selected == background);
    TEST_CHECK(workspace.select(window, original) && workspace.windows().at(window).active == split);
    const auto foreground = workspace.addTab(window);
    TEST_CHECK(workspace.windows().at(window).selected == foreground && !workspace.windows().at(window).secondary);
    TEST_CHECK(workspace.select(window, original) && workspace.windows().at(window).active == split);
    invariant(workspace);
    while (workspace.addTab(window, false)) {
    }
    TEST_CHECK(workspace.windows().at(window).selected == original && workspace.windows().at(window).active == split);
    TEST_CHECK(!workspace.addTab(other, false) && !workspace.addTab(999, false));
    invariant(workspace);
  }
  void closeEitherPane() {
    luciole::TabWorkspace workspace;
    const auto window = workspace.addWindow();
    const auto original = workspace.addTab(window);
    const auto right = workspace.addPane(window);
    TEST_CHECK(
        workspace.closePane(window, original)
        && workspace.windows().at(window).primary == right
        && workspace.windows().at(window).tabs == std::vector{original}
    );
    const auto added = workspace.addPane(window);
    TEST_CHECK(workspace.closePane(window, added) && workspace.windows().at(window).primary == right);
    TEST_CHECK(!workspace.closePane(window, right));
    TEST_CHECK(workspace.close(right) && workspace.reopen(window) && workspace.windows().at(window).primary == right);
    const auto second = workspace.addWindow();
    TEST_CHECK(
        workspace.move(right, second, 0) && workspace.owner(original) == second && workspace.owner(right) == second
    );
    invariant(workspace);
  }
  void transitions() {
    luciole::TabWorkspace workspace;
    std::mt19937 random(42);
    for (int i = 0; i < 4; ++i) {
      workspace.addWindow();
    }
    for (int i = 0; i < 10000; ++i) {
      const auto window = 1 + random() % 4;
      const auto page = 1 + random() % 500;
      switch (random() % 10) {
      case 0:
        workspace.addTab(window, random() % 2 != 0);
        break;
      case 1:
        workspace.close(page);
        break;
      case 2:
        workspace.reopen(window);
        break;
      case 3:
        workspace.select(window, page);
        break;
      case 4:
        workspace.move(page, window, random() % 40);
        break;
      case 5:
        workspace.addPane(window);
        break;
      case 6:
        workspace.unsplit(window);
        break;
      case 7:
        workspace.swap(window);
        break;
      case 8:
        workspace.resize(window, static_cast<float>(random() % 400) / 100 - 1);
        break;
      case 9:
        workspace.focus(window, page);
        break;
      }
      invariant(workspace);
    }
    TEST_CHECK(!workspace.resize(1, NAN));
  }
} // namespace
int main() {
  try {
    workflows();
    backgroundTabs();
    closeEitherPane();
    transitions();
    std::println("tab workspace checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "tab workspace test: {}", error.what());
    return 1;
  }
}
