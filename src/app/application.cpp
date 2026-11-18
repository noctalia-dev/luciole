#include "app/application.h"

#include "app/file_manager_service.h"
#include "app/instance_service.h"
#include "app/library_setup.h"
#include "app/main_loop.h"
#include "app/startup_trace.h"
#include "app/workspace_application.h"
#include "browser/location_presentation.h"
#include "clipboard/text_clipboard.h"
#include "config/settings.h"
#include "core/files/file_watcher.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "event_loop/deferred_call_poll_source.h"
#include "event_loop/glib_poll_source.h"
#include "event_loop/timer_poll_source.h"
#include "fs/gio_ptr.h"
#include "platform/desktop_integration.h"
#include "platform/file_chooser_portal.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "render/text/system_font.h"
#include "system/icon_theme_poll_source.h"
#include "ui/controls/input.h"
#include "ui/controls/permission_editor.h"
#include "ui/style.h"
#include "wayland/text_input_service.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <array>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <print>
#include <string>
#include <utility>

namespace {
  void stopRequested(int) { g_stopRequested = 1; }

  class KeyRepeatPollSource final : public PollSource {
  public:
    explicit KeyRepeatPollSource(WaylandConnection& seat) : m_seat(seat) {}
    int pollTimeoutMs() const override { return m_seat.repeatPollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>&, std::size_t) override { m_seat.repeatTick(); }

  private:
    void doAddPollFds(std::vector<pollfd>&) override {}
    WaylandConnection& m_seat;
  };

  void initializeKeys() {
    const std::array<std::pair<KeybindAction, std::uint32_t>, 8> keys{{
        {KeybindAction::Cancel, XKB_KEY_Escape},
        {KeybindAction::Left, XKB_KEY_Left},
        {KeybindAction::Right, XKB_KEY_Right},
        {KeybindAction::Up, XKB_KEY_Up},
        {KeybindAction::Down, XKB_KEY_Down},
        {KeybindAction::TabNext, XKB_KEY_Tab},
        {KeybindAction::Delete, XKB_KEY_Delete},
        {KeybindAction::Save, XKB_KEY_s},
    }};
    for (const auto& [action, sym] : keys) {
      KeybindMatcher::setMatcher(action, [action, sym](std::uint32_t key, std::uint32_t modifiers) {
        return key == sym && modifiers == (action == KeybindAction::Save ? KeyMod::Ctrl : 0U);
      });
    }
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t key, std::uint32_t modifiers) {
      return modifiers == 0 && KeySymbol::isEnterOrSpace(key);
    });
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t sym, std::uint32_t modifiers) {
      return modifiers == 0 && KeySymbol::isEnterOrSpace(sym);
    });
    KeybindMatcher::setMatcher(KeybindAction::Copy, [](std::uint32_t sym, std::uint32_t modifiers) {
      return sym == XKB_KEY_c && modifiers == KeyMod::Ctrl;
    });
    KeybindMatcher::setMatcher(KeybindAction::TabPrevious, [](std::uint32_t sym, std::uint32_t modifiers) {
      return KeySymbol::isTab(sym) && modifiers == KeyMod::Shift;
    });
  }

  // Relative arguments (`xdg-open .`) name paths under the launching shell's directory.
  std::optional<luciole::Location> launchLocation(const std::optional<std::string>& text) {
    if (!text) {
      return std::nullopt;
    }
    const luciole::GString cwd(g_get_current_dir());
    return luciole::resolveLocationText(*text, luciole::Location::fromPath(cwd.get()));
  }

  // Applies the system (GTK) font family and size to all text; true if it changed.
  bool applySystemFont(RenderContext& renderer) {
    if (!text::refreshSystemFont()) {
      return false;
    }
    renderer.setTextFontFamily(text::resolveFontFamily(""));
    renderer.setTextScale(text::currentSystemFont().textScale(Style::fontSizeBody));
    return true;
  }

} // namespace

int luciole::runApplication(const std::optional<std::string>& initialLocation, StartupMode mode) {
  startupTrace("main");
#ifdef __GLIBC__
  // Every short-lived worker thread (GIO tasks, decoders, Mesa) would otherwise get its own arena and keep its freed
  // memory resident; two arenas hold the same work with several MB less RSS.
  mallopt(M_ARENA_MAX, 2);
  // A fixed threshold keeps large listing arrays mmapped, so they return to the system when freed; glibc's dynamic
  // threshold would otherwise grow and leave them in the heap (measured: 14 MiB less at 100,000 entries).
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
  initLogLevelFromEnvironment();
  std::optional<luciole::Location> location;
  try {
    location = launchLocation(initialLocation);
  } catch (const std::exception& error) {
    std::println(stderr, "luciole: {}", error.what());
    return EXIT_FAILURE;
  }
  // A chooser service never claims the single-instance name: it must not receive ordinary launches.
  std::optional<luciole::InstanceService> instance;
  if (mode != StartupMode::FileChooserService) {
    instance.emplace();
  }
  startupTrace("instance-ready");
  if (mode == StartupMode::Normal && instance->role() == luciole::InstanceService::Role::Secondary) {
    const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
    if (instance->forward(location, token != nullptr ? token : "")) {
      return EXIT_SUCCESS;
    }
  }
  initializeKeys();
  Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t modifiers) {
    return modifiers == 0 && KeySymbol::isEnter(sym);
  });
  g_stopRequested = 0;
  const auto previousTerm = std::signal(SIGTERM, stopRequested);
  const auto previousInt = std::signal(SIGINT, stopRequested);
  int result = EXIT_SUCCESS;
  try {
    ConfigService config(ConfigPaths::fromEnvironment());
    if (!config.reload()) {
      throw std::runtime_error(config.issues().front().format());
    }
    luciole::initializeLibrary();
    Style::setCornerRadiusScale(static_cast<float>(config.settings().browser.cornerRadiusScale));
    GlibPollSource glib;
    TextInputService textInput;
    WaylandConnection connection;
    if (!connection.connect()) {
      throw std::runtime_error("could not connect to Wayland: " + connection.describeDisplayError());
    }
    startupTrace("wayland-connected");
    connection.setTextInputService(&textInput);
    auto& desktop = connection.extension<luciole::DesktopIntegration>();
    WaylandTextClipboard clipboard(
        connection.display(), desktop.dataDeviceManager(), connection.seat(),
        [&connection] { return connection.lastKeyboardSurface() ? connection.lastInputSerial() : 0; },
        connection.compositor(), connection.shm()
    );
    Input::setTextClipboard(&clipboard);
    struct ClipboardBinding {
      ~ClipboardBinding() { Input::setTextClipboard(nullptr); }
    } clipboardBinding;
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    applySystemFont(renderer);
    startupTrace("graphics-ready");
    WorkspaceApplication application(connection, renderer, textInput, clipboard, config.settings().browser);
    application.imageTextures().initialize(&graphics);
    if (mode == StartupMode::Normal) {
      application.start(std::move(location));
    }
    startupTrace("workspace-ready");
    std::optional<luciole::FileManagerService> fileManager;
    if (instance) {
      instance->setHandler([&application](const luciole::InstanceRequest& request) { application.open(request); });
      fileManager.emplace(
          [&application](const luciole::FileManagerRequest& request) { application.show(request); },
          mode == StartupMode::FileManagerActivation
      );
    }
    // Declared after `application` so it goes first and flushes the replies of the windows' last calls.
    std::optional<luciole::FileChooserService> chooser;
    if (mode == StartupMode::FileChooserService) {
      chooser.emplace([&application](
                          luciole::FileChooserRequest request, const std::shared_ptr<luciole::FileChooserCall>& call
                      ) { application.choose(std::move(request), call); });
    }
    connection.setPointerEventCallback([&application, &desktop](const PointerEvent& event) {
      desktop.pointer(event);
      application.pointer(event);
    });
    connection.setKeyboardEventCallback([&application](const KeyboardEvent& event) { application.keyboard(event); });
    connection.setKeyboardModifiersCallback([&clipboard](std::uint32_t) { clipboard.refreshDrag(); });
    connection.setKeyboardFocusCallback([&application, &clipboard](wl_surface* surface, bool entered) {
      application.focus(surface, entered);
      if (entered) {
        clipboard.keyboardFocusEntered();
      }
    });
    connection.setOutputChangeCallback([&application, &connection] {
      for (const auto& output : connection.outputs()) {
        application.outputChanged(output.output);
      }
    });
    DeferredCallPollSource deferred;
    TimerPollSource timers;
    KeyRepeatPollSource repeat(connection);
    IconThemePollSource icons;
    icons.setChangeCallback([&application] { application.iconsChanged(); });
    FileWatcher fontFiles;
    for (const auto& file : text::systemFontWatchFiles()) {
      fontFiles.watch(
          file,
          [&application, &renderer] {
            if (applySystemFont(renderer)) {
              application.fontChanged();
            }
          },
          FileWatcher::WatchTrigger::WriteCompleted
      );
    }
    FileWatchPollSource fontWatch(fontFiles);
    MainLoop loop(
        connection, {&deferred, &timers, &repeat, &glib, &icons, &fontWatch, &application.imageTextures()},
        [&application, &fileManager, &instance, &chooser] {
          const bool running = application.running();
          if ((fileManager && fileManager->awaitingRequest()) || (chooser && chooser->awaitingRequest())) {
            return g_stopRequested == 0;
          }
          if (!application.hasWindows()) {
            if (fileManager) {
              fileManager->release();
            }
            if (instance) {
              instance->release();
            }
            if (chooser) {
              chooser->release();
            }
          }
          return running;
        }
    );
    loop.run();
    if (instance) {
      instance->release();
    }
    connection.setOutputChangeCallback({});
    connection.setPointerEventCallback({});
    connection.setKeyboardEventCallback({});
    connection.setKeyboardModifiersCallback({});
    connection.setKeyboardFocusCallback({});
    textInput.cleanup();
    connection.setTextInputService(nullptr);
  } catch (const std::exception& error) {
    std::println(stderr, "luciole: {}", error.what());
    result = EXIT_FAILURE;
  }
  std::signal(SIGTERM, previousTerm);
  std::signal(SIGINT, previousInt);
  return result;
}
