#pragma once

#include "browser/directory_model.h"
#include "config/state_store.h"
#include "fs/directory_session.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace luciole {
  class PinService;
} // namespace luciole

struct KeyboardEvent;
class Box;
class Flex;
class GlyphPicker;
class Input;
class Label;
class NameDialog;
class Node;
class OperationJob;
class PageContext;
class Renderer;
class TextInputClient;

// Single-item inline rename of a file or folder, and renaming a place or heading in the sidebar.
class RenameController {
public:
  RenameController(PageContext& page, luciole::PinService& pins);

  // Attaches the frameless inline editor and its error note to `filePane`.
  void buildInlineEditor(Node& filePane);
  // True while a file or folder is being renamed inline.
  bool editingFile() const { return m_fileRenameTarget.has_value(); }
  // True while an inline rename commit is queued or in flight.
  bool submittingFile() const { return m_fileRenameSubmitting; }
  // True while the inline editor is active and covers the scene point.
  bool inlineContains(float x, float y) const;
  // The inline input as a TextInputClient while focused, or null.
  TextInputClient* focusedTextInputClient() const;

  void openFileRename(luciole::DirectoryEntry target);
  // Notes that XKB dead-key preedit began on the inline editor, distinguishing it from Wayland IME composition.
  void notePreeditKey();
  // Keyboard hook while editingFile(): handles Escape, repeated Rename (F2) selection cycling, and Up/Down navigation.
  bool key(const KeyboardEvent& event);
  // FocusNext / FocusPrevious (Tab / Shift+Tab) hook while editingFile(): steps to the next or previous item.
  bool tabKey(bool reverse);
  // Cancels inline rename without committing changes.
  void cancelFileRename(bool restoreFocus);
  // Drops any deferred neighbor rename waiting on a refresh or metadata fetch.
  void cancelPendingResume();
  // Commits a valid changed inline rename on focus loss or view change, otherwise cancels cleanly.
  void commitOrCancelInline();
  // BrowserPage::syncBrowser() hook: tracks model updates, resumes pending neighbor steps, or cancels vanished targets.
  void syncInlineTarget();
  // BrowserPage::syncBrowser() hook after setSelectedIndex(): applies a queued scroll-into-view for the renaming item.
  void applyPendingReveal();
  // BrowserPage::layoutFrame() hook: places the frameless inline input and error note over the target label.
  void layoutInline(Renderer& renderer);

  void openPlaceRename(const luciole::Bookmark& bookmark);
  void openPlaceIconPicker(const luciole::Bookmark& bookmark);
  // Adds or renames the heading of the Places separator in front of row `before`.
  void openSeparatorHeading(std::size_t before);
  // Owned rename job change: sync the editor on the next frame, or report a job whose editor closed.
  void changed(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // BrowserPage::prepareFrame() hook before syncBrowser(): applies completed place edits to their dialogs.
  void syncPlaceEditors();
  void placeIconPointer(float x, float y, bool hover);
  void syncPlaceIconTooltip(Renderer& renderer);

private:
  struct PendingNextTarget {
    luciole::EntryId id = 0;
    luciole::Location location;
    int direction = 0;
    std::size_t stride = 1;
  };
  struct PendingResume {
    luciole::EntryId id = 0;
    luciole::Location location;
    std::uint64_t contextSerial = 0;
    int direction = 0;
    std::size_t stride = 1;
    std::weak_ptr<OperationJob> retiringJob;
    std::optional<std::uint64_t> scannedGeneration = std::nullopt;
  };

  bool targetStillCurrent(const luciole::DirectoryEntry& target) const;
  bool resolveActiveTarget();
  std::optional<luciole::EntryId>
  findAdjacentTarget(std::size_t rowIndex, int direction, std::size_t stride, const OperationJob* ignoreJob) const;
  void advancePendingResume();
  void beginInlineRename(luciole::DirectoryEntry target);
  void submitInlineRename(std::string name, std::optional<PendingNextTarget> nextTarget);
  bool detachPendingRename(std::string name);
  void navigateInlineRename(int direction, bool byRow);
  void resumeSteppingFrom(std::size_t rowIndex, int direction, std::size_t stride);
  void handleInlineFocusLoss();
  void closeInlineUi(bool restoreFocus, bool preserveResume = false);
  void releaseInlineJob();
  void showInlineError(std::string error);
  void clearInlineError();
  void syncInlineErrorVisual();
  void ensureTargetVisible();
  void syncFileRename(OperationJob& job);
  void submitPlaceRename(std::string label);
  void submitPlaceIcon(std::string iconName);
  bool refreshPlaceIconTarget();

  PageContext& m_page;
  luciole::PinService& m_pins;
  // The inline editor's job: checks the target, then renames it.
  std::shared_ptr<OperationJob> m_job;
  Node* m_inlineClip = nullptr;
  Input* m_inlineInput = nullptr;
  Flex* m_inlineErrorBanner = nullptr;
  Box* m_inlineErrorBar = nullptr;
  Label* m_inlineErrorLabel = nullptr;
  std::optional<luciole::DirectoryEntry> m_fileRenameTarget;
  std::optional<std::size_t> m_inlineRowIndex;
  std::optional<std::uint64_t> m_targetScannedGeneration;
  std::optional<std::string> m_pendingSubmitName;
  std::optional<PendingNextTarget> m_pendingNextTarget;
  std::optional<PendingResume> m_resumeNextTarget;
  std::string m_inlineHint;
  std::string m_inlineError;
  bool m_fileRenameValidUtf8 = true;
  bool m_fileRenameSubmitting = false;
  bool m_closingInline = false;
  bool m_xkbPreedit = false;
  bool m_revealPending = false;
  bool m_inlineClipped = false;
  bool m_dirty = false;
  NameDialog* m_placeEditor = nullptr;
  GlyphPicker* m_placeIconPicker = nullptr;
  std::optional<luciole::Bookmark> m_placeRenameTarget;
  std::optional<std::string> m_pendingPlaceLabel;
  std::optional<luciole::Bookmark> m_placeIconTarget;
  std::optional<std::string> m_pendingPlaceIcon;
  NameDialog* m_headingEditor = nullptr;
};
