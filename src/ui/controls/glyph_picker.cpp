#include "ui/controls/glyph_picker.h"

#include "cursor-shape-v1-client-protocol.h"
#include "render/text/glyph_registry.h"
#include "ui/builders.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

namespace {
  constexpr float kGlyphPickerHeight = 540.0F;
}

class GlyphGridAdapter : public VirtualGridAdapter {
public:
  struct Entry {
    std::string name;
  };

  GlyphGridAdapter() {
    const auto& tabler = GlyphRegistry::tablerGlyphMetadata();
    const auto& aliases = GlyphRegistry::aliases();
    std::unordered_set<std::string> seen;
    seen.reserve(tabler.size() + aliases.size());
    m_master.reserve(tabler.size() + aliases.size());

    for (const auto& [name, target] : aliases) {
      if (seen.insert(name).second) {
        if (const auto it = tabler.find(std::string(target)); it != tabler.end()) {
          m_master.push_back({name});
        }
      }
    }
    for (const auto& glyph : tabler) {
      const auto& name = glyph.first;
      if (seen.insert(name).second) {
        m_master.push_back({name});
      }
    }
    std::ranges::sort(m_master, {}, &Entry::name);
    m_visible.reserve(m_master.size());
    rebuildVisible({});
  }

  std::size_t itemCount() const override { return m_visible.size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    Glyph* glyphRaw = nullptr;
    auto tile = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .padding = 0.0F,
        .configure = [](Flex& flex) {
          flex.setRadius(Style::radiusMd);
          flex.clearBorder();
        },
    });
    tile->addChild(
        ui::glyph({
            .out = &glyphRaw,
            .glyphSize = 28.0F,
        })
    );
    tile->setUserData(glyphRaw);
    return tile;
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (index >= m_visible.size()) {
      tile.setVisible(false);
      return;
    }
    const Entry& entry = m_master[m_visible[index]];

    auto* flex = static_cast<Flex*>(&tile);
    auto* glyph = static_cast<Glyph*>(flex->userData());

    if (selected) {
      flex->setFill(colorSpecFromRole(ColorRole::Primary));
      if (glyph != nullptr) {
        glyph->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      }
    } else if (hovered) {
      flex->setFill(colorSpecFromRole(ColorRole::Hover));
      if (glyph != nullptr) {
        glyph->setColor(colorSpecFromRole(ColorRole::OnHover));
      }
    } else {
      flex->clearFill();
      if (glyph != nullptr) {
        glyph->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
    }
    if (glyph != nullptr) {
      glyph->setGlyph(entry.name);
    }
  }

  std::string itemTooltip(std::size_t index) const override {
    return index < m_visible.size() ? m_master[m_visible[index]].name : std::string{};
  }

  void rebuildVisible(std::string_view filter) {
    m_visible.clear();
    const std::string needle = StringUtils::toLower(filter);
    const bool filterByName = !needle.empty();
    for (std::size_t i = 0; i < m_master.size(); ++i) {
      if (!filterByName || m_master[i].name.contains(needle)) {
        m_visible.push_back(i);
      }
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

  const Entry& entryAt(std::size_t visibleIndex) const { return m_master[m_visible[visibleIndex]]; }

private:
  std::vector<Entry> m_master;
  std::vector<std::size_t> m_visible;
};

GlyphPicker::GlyphPicker(std::function<void()> requestTooltipUpdate) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  luciole::applyDialogStyle(*this);
  addChild(
      luciole::dialogHeader(
          "Choose icon", "apps",
          [this] {
            if (m_onCancel) {
              m_onCancel();
            }
          },
          &m_title
      )
  );

  m_adapter = std::make_unique<GlyphGridAdapter>();
  addChild(
      ui::input({
          .out = &m_searchInput,
          .placeholder = "Search icons by name",
          .clearButtonEnabled = true,
          .onChange = [this](const std::string& value) { applyFilter(value); },
      })
  );

  addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .contentScale = 1.0F,
          .minCellWidth = 56.0F,
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
              .text = "Reset to default",
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
              .text = "Use icon",
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

  auto tooltip = std::make_unique<TooltipPresenter>(std::move(requestTooltipUpdate));
  m_tooltip = tooltip.get();
  addChild(std::move(tooltip));

  syncSelection();
}

GlyphPicker::~GlyphPicker() {
  // Detach the adapter before m_grid (a child) gets destroyed by ~Node, since
  // VirtualGridView's pool tiles were minted by m_adapter and reference it.
  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
}

void GlyphPicker::setTitle(std::string_view title) {
  if (m_title != nullptr) {
    m_title->setText(title.empty() ? "Choose icon" : std::string(title));
  }
}

void GlyphPicker::setInitialGlyph(std::optional<std::string> name) {
  m_pendingInitialGlyph = std::move(name);
  m_pendingInitialApplied = false;
  markLayoutDirty();
}

InputArea* GlyphPicker::initialFocusArea() const noexcept {
  return m_searchInput != nullptr ? m_searchInput->inputArea() : nullptr;
}

void GlyphPicker::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_searchInput != nullptr) {
    m_searchInput->setEnabled(enabled);
  }
  if (m_applyButton != nullptr) {
    m_applyButton->setEnabled(enabled && currentResult().has_value());
  }
  if (m_resetButton != nullptr) {
    m_resetButton->setEnabled(enabled);
  }
  setOpacity(enabled ? 1.0F : 0.55F);
}

void GlyphPicker::showError(std::string_view message) {
  if (m_selectedName != nullptr) {
    m_selectedName->setText(message);
    m_selectedName->setColor(colorSpecFromRole(ColorRole::Error));
  }
}

void GlyphPicker::tooltipPointer(float x, float y, bool arm) {
  m_pointerX = x;
  m_pointerY = y;
  if (m_tooltip == nullptr) {
    return;
  }
  if (arm) {
    m_tooltip->resume();
  } else {
    m_tooltip->dismiss();
  }
}

void GlyphPicker::syncTooltip(Renderer& renderer) {
  if (m_tooltip != nullptr) {
    m_tooltip->sync(renderer, *this, m_pointerX, m_pointerY, {width(), height()});
  }
}

std::optional<GlyphPickerResult> GlyphPicker::currentResult() const {
  if (m_grid == nullptr || m_adapter == nullptr) {
    return std::nullopt;
  }
  const auto idx = m_grid->selectedIndex();
  if (!idx.has_value() || *idx >= m_adapter->itemCount()) {
    return std::nullopt;
  }
  const auto& entry = m_adapter->entryAt(*idx);
  return GlyphPickerResult{.name = entry.name};
}

void GlyphPicker::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);

  if (!m_pendingInitialApplied && m_pendingInitialGlyph.has_value() && m_grid != nullptr && m_adapter != nullptr) {
    if (const auto idx = m_adapter->indexOfName(*m_pendingInitialGlyph); idx.has_value()) {
      m_grid->setSelectedIndex(idx, false);
    }
    m_pendingInitialApplied = true;
  }
}

LayoutSize GlyphPicker::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  auto measured = Flex::doMeasure(renderer, constraints);
  // VirtualGridView reports no intrinsic height. Advertise the picker viewport here
  // instead of setting a hard minimum, so Luciole can still shrink it to fit short windows.
  measured.height = std::max(measured.height, kGlyphPickerHeight);
  return constraints.constrain(measured);
}

void GlyphPicker::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void GlyphPicker::applyFilter(const std::string& filter) {
  if (m_adapter == nullptr || m_grid == nullptr) {
    return;
  }
  const auto previousResult = currentResult();
  m_adapter->rebuildVisible(filter);
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

void GlyphPicker::syncSelection() {
  const auto result = currentResult();
  if (m_applyButton != nullptr) {
    m_applyButton->setEnabled(m_enabled && result.has_value());
  }
  if (m_selectedName != nullptr) {
    m_selectedName->setText(result ? result->name : "Select an icon");
    m_selectedName->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  }
}
