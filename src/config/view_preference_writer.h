#pragma once

#include "config/settings.h"

#include <functional>
#include <memory>

namespace luciole {
  // Serial worker writes, coalescing rapid changes to the latest value per key.
  class ViewPreferenceWriter {
  public:
    explicit ViewPreferenceWriter(ConfigPaths paths);
    ~ViewPreferenceWriter();
    ViewPreferenceWriter(const ViewPreferenceWriter&) = delete;
    ViewPreferenceWriter& operator=(const ViewPreferenceWriter&) = delete;
    void save(ViewMode mode);
    void saveInteraction(ActivationMode activation, TypingMode typing);
    void saveGridIconSize(int size);
    void saveListIconSize(int size);
    void saveSidebarWidth(int width);
    void saveShowSidebar(bool visible);
    void saveShowHidden(bool visible);
    void saveSort(SortOptions options);
    void saveDetailsColumns(DetailsColumnVisibility visibility);
    void saveColumnOrder(DetailsColumnOrder order);
    void saveColumnWidths(DetailsColumnWidths widths);
    void submit(const std::vector<OverrideEdit>& edits);
    void resetAll();
    void reload();
    void startWatching();
    const std::optional<Settings>& settings() const;
    const std::optional<Settings>& inheritedSettings() const;
    bool busy() const;
    void setChangedCallback(std::function<void(const std::string&)> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
