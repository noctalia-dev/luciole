#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class InputArea;
class InputDispatcher;
class Label;
class Node;

namespace luciole {

  // Centered dialog root: Surface fill, 1px OutlineVariant border, radiusLg, spaceMd padding and gap.
  void applyDialogStyle(Flex& root);

  // Primary glyph, title (fontSizeTitle, SemiBold, one line), optional actions before the Ghost close button;
  // then a separator. `titleOut` receives the title label so callers can retitle the dialog.
  std::unique_ptr<Flex> dialogHeader(
      const std::string& title, const std::string& glyph, std::function<void()> close, Label** titleOut = nullptr,
      std::unique_ptr<Node> actions = nullptr
  );

  // Tab focus key of the header close button. Dialogs never give it initial focus, so it does not open highlighted.
  inline constexpr std::string_view kDialogCloseFocusKey = "dialog-close";
  bool isDialogClose(const InputArea* area);
  // Focuses the dialog's first tab stop other than the header close button; leaves nothing focused when there is none.
  void focusFirstDialogControl(InputDispatcher& dispatcher);

} // namespace luciole
