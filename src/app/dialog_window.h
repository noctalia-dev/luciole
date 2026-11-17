#pragma once

#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;
class AnimationManager;
class DialogWindows;
class Flex;
class RenderContext;
class TextInputService;
class WaylandConnection;

// What a dialog window shows and how big it opens.
struct DialogWindowOptions {
  std::string title;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Opening a window replaces the open window of the same non-empty group anywhere in the application.
  std::string group;
};

// A dialog in its own top-level window, parented to a browser window so the compositor keeps it above that window
// and usually floats it. Unlike a page popup, several can be open at once and the browser window stays usable.
//
// The owner keeps the window and destroys it from the closed callback at the latest. Closing is deferred to the
// next main loop iteration, so a button inside the window can close it safely.
class DialogWindow {
public:
  // `parent` is the browser window the dialog belongs to.
  DialogWindow(DialogWindows& windows, luciole::WindowSurface& parent);
  ~DialogWindow();
  DialogWindow(const DialogWindow&) = delete;
  DialogWindow& operator=(const DialogWindow&) = delete;

  // Shows `root` filling the window and focuses its first control. False when the window could not be created.
  bool open(std::unique_ptr<Flex> root, const DialogWindowOptions& options);
  // Moves the dialog over another browser window, for a page that moved there.
  void setParent(luciole::WindowSurface& parent);
  // Closes the window on the next main loop iteration; the closed callback runs then.
  void dismiss();

  // Runs once the window closed, whatever closed it. It may destroy this window.
  void setClosedCallback(std::function<void()> callback) { m_closed = std::move(callback); }
  // Sees each key before the window's own handling (Escape closes, Tab moves focus); true consumes it.
  void setKeyHandler(std::function<bool(const KeyboardEvent&)> handler) { m_keyHandler = std::move(handler); }
  // Sees each pointer event on the window or on a surface `setSurfaceOwner` claims; true consumes it.
  void setPointerHandler(std::function<bool(const PointerEvent&)> handler) { m_pointerHandler = std::move(handler); }
  // Claims extra surfaces for this window's input, such as a dropdown it opened.
  void setSurfaceOwner(std::function<bool(const wl_surface*)> owns) { m_ownsSurface = std::move(owns); }
  // Runs before each layout with the window's width.
  void setLayoutCallback(std::function<void(float width)> callback) { m_layout = std::move(callback); }

  InputDispatcher& dispatcher() { return m_dispatcher; }
  luciole::WindowSurface& surface() { return m_surface; }
  void requestLayout();

  // Input routing from DialogWindows.
  bool owns(const wl_surface* surface) const;
  void keyboard(const KeyboardEvent& event);
  void pointer(const PointerEvent& event);
  void focusChanged(bool entered);
  void outputChanged(wl_output* output);
  const std::string& group() const { return m_group; }

private:
  DialogWindows& m_windows;
  luciole::WindowSurface m_surface;
  luciole::WindowSurface* m_parent;
  InputDispatcher m_dispatcher;
  std::unique_ptr<Flex> m_root;
  std::string m_group;
  std::function<void()> m_closed;
  std::function<bool(const KeyboardEvent&)> m_keyHandler;
  std::function<bool(const PointerEvent&)> m_pointerHandler;
  std::function<bool(const wl_surface*)> m_ownsSurface;
  std::function<void(float)> m_layout;
  // Deferred closing checks it, so a window destroyed meanwhile is never touched.
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  bool m_dismissing = false;
};

// Every open dialog window: routes input, focus and output changes to them ahead of the browser windows. The
// application owns it and must keep it until every page, and so every dialog window, is gone.
class DialogWindows {
public:
  DialogWindows(
      WaylandConnection& connection, RenderContext& renderer, TextInputService& input, AnimationManager& animations
  );

  // True when the event belonged to a dialog window.
  bool keyboard(const wl_surface* focused, const KeyboardEvent& event);
  bool pointer(const PointerEvent& event);
  bool focus(const wl_surface* surface, bool entered);
  void outputChanged(wl_output* output);

private:
  friend class DialogWindow;
  void add(DialogWindow& window);
  void remove(DialogWindow& window);
  // Dismisses the other windows of `window`'s group.
  void claimGroup(DialogWindow& window);
  DialogWindow* find(const wl_surface* surface) const;

  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  TextInputService& m_input;
  AnimationManager& m_animations;
  std::vector<DialogWindow*> m_windows;
};

// Feeds a pointer event in surface coordinates to `dispatcher`.
void dispatchPointerEvent(InputDispatcher& dispatcher, const PointerEvent& event);
