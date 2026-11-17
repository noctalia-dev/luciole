#include "browser/miller_columns.h"

#include <ranges>

void luciole::MillerColumns::notify() const {
  if (m_changed) {
    m_changed();
  }
}

std::unique_ptr<luciole::MillerColumns::Column>
luciole::MillerColumns::makeColumn(BrowserController& source, const Location& location, bool inheritHistory) {
  auto column = std::make_unique<Column>(Column{
      ++m_nextId,
      std::make_unique<BrowserController>(
          source.model().sort(), source.model().filter().showHidden, source.model().filter().showBackups
      ),
      location, std::nullopt, defaultWidth
  });
  column->browser->setSearchRecursive(source.searchRecursive());
  if (inheritHistory) {
    column->browser->duplicateFrom(source);
  }
  column->browser->setViewDefaults(
      ViewMode::Miller, inheritHistory ? column->browser->viewport().iconSize : source.viewport().iconSize
  );
  column->browser->setChangedCallback([this] { notify(); });
  column->browser->setFileActivatedCallback([this](const Location& target) {
    if (m_activate) {
      m_activate(target);
    }
  });
  return column;
}

void luciole::MillerColumns::start(BrowserController& source, bool showParents) {
  const auto location = source.model().snapshot().location;
  if (!location) {
    return;
  }
  m_showParents = showParents;
  buildChain(makeColumn(source, *location));
}

void luciole::MillerColumns::buildChain(std::unique_ptr<Column> current) {
  std::vector<Location> parents;
  auto file = current->location;
  while (m_showParents && parents.size() + 1 < maxColumns) {
    auto parent = file.parent();
    if (!parent || file == *parent) {
      break;
    }
    parents.push_back(*parent);
    file = std::move(*parent);
  }
  clear();
  for (const auto& parent : parents | std::views::reverse) {
    auto column = makeColumn(*current->browser, parent, false);
    column->seedPath = true;
    column->explicitPath = !m_columns.empty();
    m_columns.push_back(std::move(column));
  }
  current->identity.reset();
  current->explicitPath = !m_columns.empty();
  current->seedPath = false;
  current->seedRevision = 0;
  current->pathEntry.reset();
  m_columns.push_back(std::move(current));
  m_focused = m_columns.size() - 1;
  notify();
}

void luciole::MillerColumns::trim(std::size_t count) {
  while (m_columns.size() > count) {
    auto column = std::move(m_columns.back());
    m_columns.pop_back();
    column->browser->setChangedCallback({});
    column->browser->setFileActivatedCallback({});
    column->browser->stop();
    m_retired.push_back(std::move(column));
  }
  m_focused = m_columns.empty() ? 0 : std::min(m_focused, m_columns.size() - 1);
}
void luciole::MillerColumns::clear() { trim(0); }
void luciole::MillerColumns::collectRetired() { m_retired.clear(); }

bool luciole::MillerColumns::focus(std::size_t index) {
  load(index);
  if (index >= m_columns.size() || index == m_focused) {
    return false;
  }
  m_focused = index;
  notify();
  return true;
}

void luciole::MillerColumns::load(std::size_t index) {
  if (index < m_columns.size() && m_columns[index]->browser->model().snapshot().status == DirectoryStatus::Idle) {
    m_columns[index]->browser->open(m_columns[index]->location);
  }
}

void luciole::MillerColumns::interact(std::size_t index) {
  if (index < m_columns.size() && m_columns[index]->seedPath) {
    m_columns[index]->seedPath = false;
    notify();
  }
}

void luciole::MillerColumns::synchronize() {
  if (!enabled()) {
    return;
  }
  // Address-bar, history, search-result and sidebar navigation can replace a
  // column's location. Reuse the controller carrying that visit's history.
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    const auto target = m_columns[i]->browser->model().snapshot().location;
    if (!target || *target == m_columns[i]->location) {
      continue;
    }
    std::optional<std::size_t> ancestor;
    for (std::size_t j = 0; j < i; ++j) {
      if (m_columns[j]->location == *target) {
        ancestor = j;
      }
    }
    auto moved = std::move(m_columns[i]);
    m_columns.erase(m_columns.begin() + static_cast<std::ptrdiff_t>(i));
    moved->location = *target;
    if (ancestor) {
      moved->identity = m_columns[*ancestor]->identity;
      moved->explicitPath = m_columns[*ancestor]->explicitPath;
      moved->seedPath = false;
      moved->pathEntry.reset();
      trim(*ancestor);
      m_columns.push_back(std::move(moved));
      m_focused = *ancestor;
      notify();
    } else {
      buildChain(std::move(moved));
    }
    break;
  }
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    auto& browser = *m_columns[i]->browser;
    const auto& model = browser.model();
    if (!browser.searchQuery().empty()) {
      if (i + 1 < m_columns.size()) {
        trim(i + 1);
        notify();
      }
      continue;
    }
    if (model.snapshot().status == DirectoryStatus::Loading || model.snapshot().status == DirectoryStatus::Idle) {
      continue;
    }
    auto& parent = *m_columns[i];
    if (parent.seedPath && i + 1 < m_columns.size()) {
      auto& child = *m_columns[i + 1];
      if (!model.selection().selected.empty()) {
        parent.seedPath = false;
      } else {
        if (parent.seedRevision != model.snapshot().generation) {
          parent.seedRevision = model.snapshot().generation;
          parent.pathEntry.reset();
          const auto& entries = model.snapshot().entries;
          for (std::size_t source = 0; source < entries.size(); ++source) {
            if (entries.location(source) != child.location) {
              continue;
            }
            const auto& entry = *model.entry(entries.brief(source).id);
            if (entry.metadataPending) {
              break;
            }
            auto identity = entry.identity();
            if (child.identity && child.identity != identity) {
              parent.seedPath = false;
              break;
            }
            child.identity = std::move(identity);
            if (entry.type == luciole::FileType::Directory || entry.symlink) {
              parent.pathEntry = entry.id;
            }
            break;
          }
        }
        if (parent.pathEntry && browser.model().select(*parent.pathEntry)) {
          parent.seedPath = false;
        }
        // Hidden or unreadable ancestors must not discard the requested folder.
        if (parent.seedPath) {
          continue;
        }
      }
    }
    const auto& selected = model.selection().selected;
    const auto* entry = selected.size() == 1 ? model.entry(*selected.begin()) : nullptr;
    if (entry && entry->metadataPending) {
      continue;
    }
    const bool folder = entry && entry->isFolderLink() && !entry->symlink && browser.searchQuery().empty();
    if (i + 1 < m_columns.size()) {
      auto& child = *m_columns[i + 1];
      const auto location = entry ? std::optional(entry->navigationLocation()) : std::nullopt;
      const auto identity = entry ? entry->identity() : std::nullopt;
      if (child.explicitPath && !child.identity && entry && location == child.location) {
        child.identity = identity;
      }
      const bool linkedPath = child.explicitPath && entry && entry->symlink;
      if ((!folder && !linkedPath) || location != child.location || identity != child.identity) {
        trim(i + 1);
        notify();
      }
    }
    if (i != m_focused || !folder || i + 1 < m_columns.size()) {
      continue;
    }
    const auto destination = entry->navigationLocation();
    const auto identity = entry->identity();
    const bool cycle = std::ranges::any_of(m_columns, [&](const auto& column) {
      return column->location == destination || (identity && column->identity == identity);
    });
    if (cycle) {
      continue;
    }
    auto child = makeColumn(browser, destination);
    child->identity = identity;
    child->width = m_columns[i]->width;
    child->fixedWidth = m_columns[i]->fixedWidth;
    child->browser->open(destination);
    if (m_columns.size() == maxColumns) {
      auto first = std::move(m_columns.front());
      m_columns.erase(m_columns.begin());
      first->browser->setChangedCallback({});
      first->browser->setFileActivatedCallback({});
      first->browser->stop();
      m_retired.push_back(std::move(first));
      --m_focused;
    }
    m_columns.push_back(std::move(child));
    notify();
    break;
  }
}

bool luciole::MillerColumns::enter() {
  synchronize();
  return focus(m_focused + 1);
}
