#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class Button;
class GlyphGridAdapter;
class InputArea;
class Input;
class Label;
class VirtualGridView;
class TooltipPresenter;

struct GlyphPickerResult {
  std::string name;
};

// Luciole dialog for searching and choosing a glyph from the Tabler catalog.
class GlyphPicker : public Flex {
public:
  explicit GlyphPicker(std::function<void()> requestTooltipUpdate);
  ~GlyphPicker() override;

  void setTitle(std::string_view title);
  // Pre-select a glyph on first layout without changing the initial viewport.
  void setInitialGlyph(std::optional<std::string> name);

  InputArea* initialFocusArea() const noexcept;
  std::optional<GlyphPickerResult> currentResult() const;

  void setOnCancel(std::function<void()> callback) { m_onCancel = std::move(callback); }
  void setOnApply(std::function<void(const GlyphPickerResult&)> callback) { m_onApply = std::move(callback); }
  void setOnReset(std::function<void()> callback) { m_onReset = std::move(callback); }

  void setEnabled(bool enabled);
  void showError(std::string message);
  void tooltipPointer(float x, float y, bool arm);
  void syncTooltip(Renderer& renderer);

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void applyFilter(const std::string& filter);
  void syncSelection();

  Label* m_title = nullptr;
  Input* m_searchInput = nullptr;
  Label* m_selectedName = nullptr;
  VirtualGridView* m_grid = nullptr;
  Button* m_applyButton = nullptr;
  Button* m_resetButton = nullptr;
  TooltipPresenter* m_tooltip = nullptr;

  std::unique_ptr<GlyphGridAdapter> m_adapter;
  std::function<void()> m_onCancel;
  std::function<void(const GlyphPickerResult&)> m_onApply;
  std::function<void()> m_onReset;

  std::optional<std::string> m_pendingInitialGlyph;
  bool m_pendingInitialApplied = false;
  bool m_enabled = true;
  float m_pointerX = 0.0F, m_pointerY = 0.0F;
};
