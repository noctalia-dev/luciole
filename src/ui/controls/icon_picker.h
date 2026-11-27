#pragma once

#include "system/icon_resolver.h"
#include "ui/controls/flex.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole {
  class FileIconCache;
} // namespace luciole

class Button;
class InputArea;
class Input;
class Label;
class Select;
class VirtualGridView;
class IconGridAdapter;

struct IconPickerResult {
  std::string name;
};

// Luciole dialog for searching and choosing an icon-theme icon, with folder
// artwork first. Unlike GlyphPicker (Tabler glyphs for places), the tiles show
// real theme icons through the shared FileIconCache.
class IconPicker : public Flex {
public:
  explicit IconPicker(luciole::FileIconCache& icons, std::vector<ThemedIcon> names);
  ~IconPicker() override;

  void setTitle(std::string_view title);
  // Pre-select and scroll to an icon on first layout.
  void setInitialIcon(std::optional<std::string> name);

  InputArea* initialFocusArea() const noexcept;
  std::optional<IconPickerResult> currentResult() const;

  void setOnCancel(std::function<void()> callback) { m_onCancel = std::move(callback); }
  void setOnApply(std::function<void(const IconPickerResult&)> callback) { m_onApply = std::move(callback); }
  void setOnReset(std::function<void()> callback) { m_onReset = std::move(callback); }

  void setEnabled(bool enabled);
  void showError(std::string_view message);

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  void applyFilter(const std::string& filter);
  void syncSelection();

  luciole::FileIconCache* m_icons = nullptr;
  Label* m_title = nullptr;
  Input* m_searchInput = nullptr;
  Label* m_selectedName = nullptr;
  VirtualGridView* m_grid = nullptr;
  Button* m_applyButton = nullptr;
  Button* m_resetButton = nullptr;

  std::unique_ptr<IconGridAdapter> m_adapter;
  std::function<void()> m_onCancel;
  std::function<void(const IconPickerResult&)> m_onApply;
  std::function<void()> m_onReset;

  Select* m_scopeSelect = nullptr;
  std::vector<std::string> m_scopeOptions;
  // Raw theme-context names, one per m_scopeOptions entry between "Folders Only" and "Unthemed";
  // the dropdown shows translated labels, filtering compares against these.
  std::vector<std::string> m_scopeContexts;
  std::optional<std::string> m_pendingInitialIcon;
  bool m_pendingInitialApplied = false;
  bool m_enabled = true;
};
