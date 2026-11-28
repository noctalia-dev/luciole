#pragma once

#include "core/timer_manager.h"
#include "model/directory_options.h"
#include "model/job_activity.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class Button;
class InputArea;
class Label;
struct LayoutRect;
class OperationHub;
class OperationsView;
class PageContext;
class ProgressBar;
class ScrollView;
enum class ActivityAction;

// Application-wide job presentation. Services and accepted jobs outlive this view.
class OperationsController {
public:
  OperationsController(PageContext& page, OperationHub& hub, std::function<void(std::uint64_t, bool)> route);
  std::unique_ptr<Button> buildButton();
  void setButtonMode(luciole::OperationsButton mode);
  void markDirty();
  void onFrame();
  void open();
  void layoutDialog();
  void parentConfigured();
  void revealFocus(InputArea* focus);

private:
  void act(std::uint64_t id, ActivityAction action);
  void details(std::uint64_t id);
  void refreshDetails();
  void closed();
  float popupHeight(std::size_t count) const;
  // The toolbar button, or the main menu button while the toolbar button is hidden.
  LayoutRect popupAnchor() const;

  PageContext& m_page;
  OperationHub& m_hub;
  std::function<void(std::uint64_t, bool)> m_route;
  Button *m_button = nullptr, *m_clear = nullptr;
  OperationsView* m_view = nullptr;
  ScrollView* m_detailsScroll = nullptr;
  Label* m_reportLabel = nullptr;
  Label* m_infoLabel = nullptr;
  ProgressBar* m_detailsBar = nullptr;
  std::optional<std::uint64_t> m_detailsId;
  bool m_detailsLive = false;
  Timer m_progress;
  std::size_t m_count = 0;
  std::optional<std::array<int, 4>> m_anchor;
  std::optional<std::uint64_t> m_pendingDetails;
  bool m_reopen = false;
  std::pair<std::uint32_t, std::uint32_t> m_parentSize;
  luciole::OperationsButton m_buttonMode = luciole::OperationsButton::Always;
  bool m_dirty = true;
};
