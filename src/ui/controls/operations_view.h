#pragma once

#include "model/job_activity.h"
#include "ui/controls/flex.h"

#include <functional>
#include <vector>

class ScrollView;
class InputArea;
enum class ActivityAction { Cancel, Review, Retry, Details, Pause, Resume };

class OperationsView final : public Flex {
public:
  using Action = std::function<void(std::uint64_t, ActivityAction)>;
  explicit OperationsView(Action action);
  ~OperationsView() override;
  // Stages snapshots. Row structure is changed only in Layout; progress updates retain existing controls.
  bool sync(std::vector<luciole::JobActivity> activities);
  void revealFocus(InputArea* focus);

private:
  class Row;
  void syncRows();
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  Action m_action;
  ScrollView* m_scroll = nullptr;
  std::vector<luciole::JobActivity> m_activities;
  std::vector<Row*> m_rows;
  bool m_structureDirty = true;
};
