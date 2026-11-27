#include "ui/controls/icon_picker.h"

#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "system/icon_resolver.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>

namespace {
  constexpr float kIconPickerHeight = 540.0F;
  constexpr float kTileIconSize = 40.0F;
} // namespace

class IconGridAdapter : public VirtualGridAdapter {
public:
  enum class Scope { All, Folders, Context, Unthemed };

  IconGridAdapter(luciole::FileIconCache& icons, std::vector<ThemedIcon> iconsIn)
      : m_icons(&icons), m_master(std::move(iconsIn)) {
    // Folder artwork first, then everything else alphabetically.
    const auto folder = [](const ThemedIcon& icon) { return icon.name.starts_with("folder"); };
    std::ranges::stable_sort(m_master, [&](const ThemedIcon& a, const ThemedIcon& b) {
      if (folder(a) != folder(b)) {
        return folder(a) > folder(b);
      }
      return a.name < b.name;
    });
    m_visible.reserve(m_master.size());
    rebuildVisible({}, Scope::All, {});
  }

  // Scope dropdown entries after "All Icons" and "Folders Only": one per
  // theme context present, with "Unthemed" last for icons found only outside
  // any indexed theme directory.
  std::vector<std::string> contexts() const {
    std::vector<std::string> out;
    for (const auto& icon : m_master) {
      if (!icon.context.empty() && !std::ranges::contains(out, icon.context)) {
        out.push_back(icon.context);
      }
    }
    std::ranges::sort(out);
    return out;
  }

  std::size_t itemCount() const override { return m_visible.size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    luciole::FileIcon* iconRaw = nullptr;
    auto tile = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .padding = 0.0F,
        .configure = [](Flex& flex) {
          flex.setRadius(Style::radiusMd);
          flex.clearBorder();
        },
    });
    auto icon = std::make_unique<luciole::FileIcon>(*m_icons, kTileIconSize);
    iconRaw = icon.get();
    tile->addChild(std::move(icon));
    tile->setUserData(iconRaw);
    return tile;
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (index >= m_visible.size()) {
      tile.setVisible(false);
      return;
    }
    const std::string& name = m_master[m_visible[index]].name;
    auto* flex = static_cast<Flex*>(&tile);
    auto* icon = static_cast<luciole::FileIcon*>(flex->userData());

    luciole::FileIconSource source;
    source.names = {name};
    if (selected) {
      flex->setFill(colorSpecFromRole(ColorRole::Primary));
      if (icon != nullptr) {
        icon->bind(source, colorSpecFromRole(ColorRole::OnPrimary));
      }
    } else if (hovered) {
      flex->setFill(colorSpecFromRole(ColorRole::Hover));
      if (icon != nullptr) {
        icon->bind(source, colorSpecFromRole(ColorRole::OnHover));
      }
    } else {
      flex->clearFill();
      if (icon != nullptr) {
        icon->bind(source, colorSpecFromRole(ColorRole::OnSurface));
      }
    }
  }

  void releaseTile(Node& tile, Renderer& renderer) override {
    if (auto* flex = dynamic_cast<Flex*>(&tile)) {
      if (auto* icon = static_cast<luciole::FileIcon*>(flex->userData())) {
        icon->releaseFile(renderer);
      }
    }
  }

  std::string itemTooltip(std::size_t index) const override {
    return index < m_visible.size() ? m_master[m_visible[index]].name : std::string{};
  }

  // Folders are folder* names, plus the user-* and network-* names that live
  // in a Places context (other Places entries, like map points of interest,
  // are app icons, not folder artwork).
  static bool isFolderIcon(const ThemedIcon& icon) {
    if (icon.name.starts_with("folder")) {
      return true;
    }
    if (icon.context == "Places" && (icon.name.starts_with("user-") || icon.name.starts_with("network-"))) {
      return true;
    }
    return false;
  }

  void rebuildVisible(std::string_view filter, Scope scope, std::string_view context) {
    m_visible.clear();
    const std::string needle = StringUtils::toLower(filter);
    const bool filterByName = !needle.empty();
    for (std::size_t i = 0; i < m_master.size(); ++i) {
      const auto& icon = m_master[i];
      switch (scope) {
      case Scope::All:
        break;
      case Scope::Folders:
        if (!isFolderIcon(icon)) {
          continue;
        }
        break;
      case Scope::Context:
        if (icon.context != context) {
          continue;
        }
        break;
      case Scope::Unthemed:
        if (!icon.context.empty()) {
          continue;
        }
        break;
      }
      if (filterByName && !StringUtils::toLower(icon.name).contains(needle)) {
        continue;
      }
      m_visible.push_back(i);
    }
  }

  std::optional<std::size_t> indexOfName(std::string_view name) const {
    for (std::size_t i = 0; i < m_visible.size(); ++i) {
      if (m_master[m_visible[i]].name == name) {
        return i;
      }
    }
    return std::nullopt;
  }

  const std::string& entryAt(std::size_t visibleIndex) const { return m_master[m_visible[visibleIndex]].name; }

private:
  luciole::FileIconCache* m_icons = nullptr;
  std::vector<ThemedIcon> m_master;
  std::vector<std::size_t> m_visible;
};

IconPicker::IconPicker(luciole::FileIconCache& icons, std::vector<ThemedIcon> names) : m_icons(&icons) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  addChild(
      luciole::dialogHeader(
          "Choose Folder Icon", "folder",
          [this] {
            if (m_onCancel) {
              m_onCancel();
            }
          },
          &m_title
      )
  );

  m_adapter = std::make_unique<IconGridAdapter>(*m_icons, std::move(names));
  m_scopeOptions.emplace_back(i18n::text("All Icons"));
  m_scopeOptions.emplace_back(i18n::text("Folders Only"));
  for (const auto& context : m_adapter->contexts()) {
    m_scopeContexts.push_back(context);
    m_scopeOptions.emplace_back(i18n::text(context));
  }
  m_scopeOptions.emplace_back(i18n::text("Unthemed"));
  addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm,
          },
          ui::input({
              .out = &m_searchInput,
              .placeholder = i18n::text("Search icons by name"),
              .clearButtonEnabled = true,
              .flexGrow = 1.0F,
              .onChange = [this](const std::string& value) { applyFilter(value); },
          }),
          ui::select({
              .out = &m_scopeSelect,
              .options = m_scopeOptions,
              .selectedIndex = 0,
              .width = 180.0F,
              .onSelectionChanged = [this](std::size_t, std::string_view) {
                applyFilter(m_searchInput != nullptr ? m_searchInput->value() : std::string{});
              },
          })
      )
  );

  addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .contentScale = 1.0F,
          .minCellWidth = 64.0F,
          .squareCells = true,
          .columnGap = Style::spaceXs,
          .rowGap = Style::spaceXs,
          .overscanRows = 2,
          .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged = [this](std::optional<std::size_t>) { syncSelection(); },
      })
  );

  addChild(
      ui::label({
          .out = &m_selectedName,
          .text = "Select an icon",
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  );

  addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::End,
              .gap = Style::spaceSm,
          },
          ui::button({
              .out = &m_resetButton,
              .text = "Reset to Default",
              .variant = ButtonVariant::Ghost,
              .onClick =
                  [this] {
                    if (m_enabled && m_onReset) {
                      m_onReset();
                    }
                  },
          }),
          ui::spacer(),
          ui::button({
              .text = "Cancel",
              .variant = ButtonVariant::Ghost,
              .minWidth = 80.0F,
              .onClick =
                  [this] {
                    if (m_onCancel) {
                      m_onCancel();
                    }
                  },
          }),
          ui::button({
              .out = &m_applyButton,
              .text = i18n::text("Use Icon"),
              .variant = ButtonVariant::Primary,
              .minWidth = 88.0F,
              .onClick = [this] {
                if (!m_enabled || !m_onApply) {
                  return;
                }
                const auto result = currentResult();
                if (result.has_value()) {
                  m_onApply(*result);
                }
              },
          })
      )
  );

  syncSelection();
}

IconPicker::~IconPicker() {
  // Detach the adapter before m_grid (a child) gets destroyed by ~Node, since
  // VirtualGridView's pool tiles were minted by m_adapter and reference it.
  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
}

void IconPicker::setTitle(std::string_view title) {
  if (m_title != nullptr) {
    m_title->setText(title.empty() ? i18n::text("Choose Folder Icon") : std::string(title));
  }
}

void IconPicker::setInitialIcon(std::optional<std::string> name) {
  m_pendingInitialIcon = std::move(name);
  m_pendingInitialApplied = false;
  markLayoutDirty();
}

InputArea* IconPicker::initialFocusArea() const noexcept {
  return m_searchInput != nullptr ? m_searchInput->inputArea() : nullptr;
}

void IconPicker::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_searchInput != nullptr) {
    m_searchInput->setEnabled(enabled);
  }
  if (m_scopeSelect != nullptr) {
    m_scopeSelect->setEnabled(enabled);
  }
  if (m_applyButton != nullptr) {
    m_applyButton->setEnabled(enabled && currentResult().has_value());
  }
  if (m_resetButton != nullptr) {
    m_resetButton->setEnabled(enabled);
  }
  setOpacity(enabled ? 1.0F : 0.55F);
}

void IconPicker::showError(std::string_view message) {
  if (m_selectedName != nullptr) {
    m_selectedName->setText(message);
    m_selectedName->setColor(colorSpecFromRole(ColorRole::Error));
  }
}

std::optional<IconPickerResult> IconPicker::currentResult() const {
  if (m_grid == nullptr || m_adapter == nullptr) {
    return std::nullopt;
  }
  const auto idx = m_grid->selectedIndex();
  if (!idx.has_value() || *idx >= m_adapter->itemCount()) {
    return std::nullopt;
  }
  return IconPickerResult{.name = m_adapter->entryAt(*idx)};
}

void IconPicker::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);

  if (!m_pendingInitialApplied && m_pendingInitialIcon.has_value() && m_grid != nullptr && m_adapter != nullptr) {
    if (const auto idx = m_adapter->indexOfName(*m_pendingInitialIcon); idx.has_value()) {
      m_grid->setSelectedIndex(idx);
      m_grid->scrollToIndex(*idx);
    }
    m_pendingInitialApplied = true;
  }
}

LayoutSize IconPicker::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  auto measured = Flex::doMeasure(renderer, constraints);
  // VirtualGridView reports no intrinsic height. Advertise the picker viewport here
  // instead of setting a hard minimum, so Luciole can still shrink it to fit short windows.
  measured.height = std::max(measured.height, kIconPickerHeight);
  return constraints.constrain(measured);
}

void IconPicker::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void IconPicker::applyFilter(const std::string& filter) {
  if (m_adapter == nullptr || m_grid == nullptr) {
    return;
  }
  // Scope options are All Icons, Folders Only, one per theme context, then Unthemed.
  const std::size_t selected = m_scopeSelect != nullptr ? m_scopeSelect->selectedIndex() : 0;
  const std::size_t options = m_scopeOptions.size();
  auto scope = IconGridAdapter::Scope::All;
  std::string_view context;
  if (selected == 1 && options > 1) {
    scope = IconGridAdapter::Scope::Folders;
  } else if (options > 2 && selected == options - 1) {
    scope = IconGridAdapter::Scope::Unthemed;
  } else if (selected >= 2 && selected + 1 < options) {
    scope = IconGridAdapter::Scope::Context;
    context = m_scopeContexts[selected - 2];
  }
  const auto previousResult = currentResult();
  m_adapter->rebuildVisible(filter, scope, context);
  // Drop selection if the previously selected name is no longer visible.
  if (previousResult.has_value()) {
    if (const auto idx = m_adapter->indexOfName(previousResult->name); idx.has_value()) {
      m_grid->setSelectedIndex(idx, false);
    } else {
      m_grid->setSelectedIndex(std::nullopt);
    }
  }
  m_grid->notifyDataChanged();
  m_grid->scrollView().setScrollOffset(0.0F);
}

void IconPicker::syncSelection() {
  const auto result = currentResult();
  if (m_applyButton != nullptr) {
    m_applyButton->setEnabled(m_enabled && result.has_value());
  }
  if (m_selectedName != nullptr) {
    m_selectedName->setText(result ? result->name : i18n::text("Select an icon"));
    m_selectedName->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  }
}
