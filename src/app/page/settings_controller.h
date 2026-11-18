#pragma once

#include "config/settings.h"

#include <functional>
#include <optional>
#include <string>

namespace luciole {
  class ThumbnailService;
  class ViewPreferenceWriter;
} // namespace luciole

struct KeyboardEvent;
class SettingsDialog;
class PageContext;

// Settings: the settings dialog and applying saved preference changes to the page.
class SettingsController final {
public:
  SettingsController(
      PageContext& page, luciole::ViewPreferenceWriter& preferences, luciole::ThumbnailService& thumbnails,
      std::function<void()> clearThumbnails
  );

  // Opens the Settings dialog, or closes it when it is already open.
  void toggle();
  // True while the Settings dialog is shown.
  bool open() const { return m_view != nullptr; }
  // Preferences were saved or reloaded; `error` is empty when that worked.
  void preferencesChanged(const std::string& error);
  // Thumbnail cache state changed: sync on the next frame.
  void markDirty() { m_dirty = true; }
  // The last preference load or save error; empty when there is none.
  const std::string& error() const { return m_error; }
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // Dialog layout hook with the dialog's width: compact navigation on narrow windows.
  void layoutDialog(float width);
  // Keyboard hook for the dialog popup: the Settings shortcut closes it. False lets the dialog handle it.
  bool key(const KeyboardEvent& event);

private:
  void sync();
  // Dialog close: forgets the view.
  void teardown();

  PageContext& m_page;
  luciole::ViewPreferenceWriter& m_preferences;
  luciole::ThumbnailService& m_thumbnails;
  std::function<void()> m_clearThumbnails;
  SettingsDialog* m_view = nullptr;
  std::optional<luciole::Settings> m_pending;
  std::string m_error;
  bool m_dirty = false;
};
