#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "imaging/pixbuf_image_decoder.h"
#include "system/icon_resolver.h"
#include "system/icon_theme_poll_source.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <print>
#include <source_location>
#include <string_view>
#include <unordered_set>

namespace {
  class Pump {
  public:
    void until(
        std::unique_ptr<IconThemePollSource>* source, const std::function<bool()>& done,
        const std::source_location where = std::source_location::current()
    ) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          std::println(stderr, "icon theme wait timed out at {}:{}", where.file_name(), where.line());
          TEST_CHECK(false);
        }
        auto* icons = source != nullptr ? source->get() : nullptr;
        std::vector<pollfd> fds;
        const auto glibStart = m_glib.addPollFds(fds);
        const auto iconStart = icons != nullptr ? icons->addPollFds(fds) : fds.size();
        int timeout = m_glib.pollTimeoutMs();
        timeout = timeout < 0 ? 20 : std::min(timeout, 20);
        if (icons != nullptr) {
          timeout = std::min(timeout, icons->pollTimeoutMs());
        }
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout) >= 0);
        m_glib.dispatch(fds, glibStart);
        if (source != nullptr && *source) {
          (*source)->dispatch(fds, iconStart);
        }
      }
    }

  private:
    GlibPollSource m_glib;
  };

  void writeColor(const std::filesystem::path& path, const std::string& color) {
    std::ofstream(path)
        << "<svg xmlns='http://www.w3.org/2000/svg' width='48' height='48'>"
           "<path fill='"
        << color
        << "' d='M0 0h48v48H0z'/></svg>";
  }

  void run(const std::filesystem::path& root, bool hasSchema) {
    Pump pump;
    const luciole::GioPtr<GSettings> settings(IconResolver::createThemeSettings());
    TEST_CHECK((settings != nullptr) == hasSchema);
    if (settings) {
      TEST_CHECK(g_settings_set_string(settings.get(), "icon-theme", "Red"));
    }
    luciole::FileIconCache cache;
    const luciole::FileIconSource source{.names = {"folder"}, .filePath = {}, .desktopFile = {}};
    auto watcher = std::make_unique<IconThemePollSource>();
    int changes = 0;
    watcher->setChangeCallback([&] {
      ++changes;
      cache.invalidate();
    });
    writeColor(root / "data/icons" / (hasSchema ? "Red" : "Blue") / "scalable/places/folder.svg", "#00ff00");
    pump.until(&watcher, [&] { return !watcher->refreshing(); });
    changes = 0;
    TEST_CHECK(IconResolver::activeThemeName() == (hasSchema ? "Red" : "Blue"));
    const auto initialPaths = IconResolver::themeWatchPaths();
    TEST_CHECK(initialPaths == IconResolver::themeWatchPaths());
    const auto initialDirs = initialPaths->iconDirs;
    TEST_CHECK(std::unordered_set<std::string>(initialDirs.begin(), initialDirs.end()).size() == initialDirs.size());
    IconResolver other(true);
    TEST_CHECK(initialPaths == IconResolver::themeWatchPaths());
    const std::string initialIcon = other.resolve("folder", 48);
    auto image = cache.request(source, 48);
    pump.until(&watcher, [&] { return image->ready; });
    TEST_CHECK(image->pixels && image->pixels->rgba[1] == 255);

    if (settings) {
      TEST_CHECK(g_settings_set_string(settings.get(), "icon-theme", "Blue"));
      pump.until(&watcher, [&] { return changes > 0; });
    } else {
      std::ofstream(root / "config/gtk-3.0/settings.ini") << "[Settings]\ngtk-icon-theme-name=Red\n";
      pump.until(&watcher, [&] { return changes > 0; });
    }
    TEST_CHECK(initialPaths != IconResolver::themeWatchPaths());
    TEST_CHECK(initialPaths->iconDirs == initialDirs);
    TEST_CHECK(other.resolve("folder", 48) != initialIcon);
    TEST_CHECK(other.resolve("folder", 48).contains(hasSchema ? "/Blue/" : "/Red/"));
    const auto switched = cache.request(source, 48);
    TEST_CHECK(switched != image);
    pump.until(&watcher, [&] { return switched->ready; });
    TEST_CHECK(switched->pixels && switched->pixels->rgba[hasSchema ? 2 : 0] == 255);
    const std::string current = hasSchema ? "Blue" : "Red";
    const auto artwork = root / "data/icons" / current / "scalable/places/folder.svg";
    for (const bool atomic : {false, true}) {
      const auto before = changes;
      const auto heldPaths = IconResolver::themeWatchPaths();
      const auto heldDirs = heldPaths->iconDirs;
      const auto path = atomic ? artwork.parent_path() / "replacement.tmp" : artwork;
      writeColor(path, atomic ? "#0000ff" : "#00ff00");
      if (atomic) {
        std::filesystem::rename(path, artwork);
      }
      pump.until(&watcher, [&] { return changes > before; });
      TEST_CHECK(heldPaths->iconDirs == heldDirs);
      TEST_CHECK(heldDirs == IconResolver::themeWatchPaths()->iconDirs);
      image = cache.request(source, 48);
      pump.until(&watcher, [&] { return image->ready; });
      TEST_CHECK(image->pixels && image->pixels->rgba[atomic ? 2 : 1] == 255);
    }
    // Replacing the logical root must rearm its new target, not merely invalidate once.
    const auto logical = root / "data/icons" / current;
    const auto replacement = root / "artwork" / (current + "-replacement");
    std::filesystem::create_directories(replacement / "scalable/places");
    std::filesystem::copy_file(logical / "index.theme", replacement / "index.theme");
    writeColor(replacement / "scalable/places/folder.svg", "#ff00ff");
    const auto nextLink = root / "data/icons" / ("next-" + current);
    std::filesystem::create_directory_symlink(replacement, nextLink);
    auto before = changes;
    if (!std::filesystem::is_symlink(logical)) {
      std::filesystem::rename(logical, root / "artwork" / (current + "-original"));
    }
    std::filesystem::rename(nextLink, logical);
    pump.until(&watcher, [&] { return changes > before && !watcher->refreshing(); });
    image = cache.request(source, 48);
    pump.until(&watcher, [&] { return image->ready; });
    TEST_CHECK(image->pixels && image->pixels->rgba[0] == 255 && image->pixels->rgba[2] == 255);
    before = changes;
    writeColor(replacement / "scalable/places/folder.svg", "#00ffff");
    pump.until(&watcher, [&] { return changes > before && !watcher->refreshing(); });
    image = cache.request(source, 48);
    pump.until(&watcher, [&] { return image->ready; });
    TEST_CHECK(image->pixels && image->pixels->rgba[1] == 255 && image->pixels->rgba[2] == 255);

    // A directory declared by the index may appear only after initial setup.
    before = changes;
    std::ofstream(replacement / "index.theme")
        << "[Icon Theme]\nName=Replacement\nDirectories=scalable/places,future/nested\n"
           "[scalable/places]\nSize=48\nType=Scalable\n[future/nested]\nSize=48\nType=Scalable\n";
    pump.until(&watcher, [&] { return changes > before && !watcher->refreshing(); });
    const luciole::FileIconSource lateSource{.names = {"late"}, .filePath = {}, .desktopFile = {}};
    image = cache.request(lateSource, 48);
    pump.until(&watcher, [&] { return image->ready; });
    TEST_CHECK(!image->pixels);
    before = changes;
    std::filesystem::create_directories(replacement / "future/nested");
    writeColor(replacement / "future/nested/late.svg", "#00ff00");
    pump.until(&watcher, [&] { return changes > before && !watcher->refreshing(); });
    image = cache.request(lateSource, 48);
    pump.until(&watcher, [&] { return image->ready; });
    TEST_CHECK(image->pixels && image->pixels->rgba[1] == 255);

    // Overflow must refresh an in-place edit whose own notification was dropped.
    int queueLimit = 0;
    std::ifstream("/proc/sys/fs/inotify/max_queued_events") >> queueLimit;
    if (queueLimit > 0 && queueLimit <= 65536) {
      before = changes;
      const auto scratch = replacement / "scalable/places/burst.tmp";
      for (int i = 0; i < queueLimit / 3 + 32; ++i) {
        std::ofstream(scratch) << "x";
        std::filesystem::remove(scratch);
      }
      writeColor(replacement / "scalable/places/folder.svg", "#ff00ff");
      pump.until(&watcher, [&] { return changes > before && !watcher->refreshing(); });
      image = cache.request(source, 48);
      pump.until(&watcher, [&] { return image->ready; });
      TEST_CHECK(image->pixels && image->pixels->rgba[0] == 255 && image->pixels->rgba[2] == 255);
    } else {
      std::println("Inotify overflow fixture omitted: queue limit {} exceeds the bounded fixture", queueLimit);
    }

    // Edits queued behind an asynchronous refresh converge on the latest selection.
    before = changes;
    const std::string finalTheme = current == "Red" ? "Blue" : "Red";
    for (int i = 0; i < 8; ++i) {
      if (settings) {
        TEST_CHECK(
            g_settings_set_string(settings.get(), "icon-theme", i % 2 == 0 ? current.c_str() : finalTheme.c_str())
        );
      } else {
        std::ofstream(root / "config/gtk-3.0/settings.ini")
            << "[Settings]\ngtk-icon-theme-name="
            << (i % 2 == 0 ? current.c_str() : finalTheme.c_str())
            << "\n";
      }
    }
    pump.until(&watcher, [&] { return changes > before && !watcher->refreshing(); });
    TEST_CHECK(IconResolver::activeThemeName() == finalTheme);

    for (int i = 0; i < 8; ++i) {
      auto abandoned = std::make_unique<IconThemePollSource>();
      abandoned->setChangeCallback([] { TEST_CHECK(false); });
    }
    auto closing = std::make_unique<IconThemePollSource>();
    bool closed = false;
    closing->setChangeCallback([&] {
      closing.reset();
      closed = true;
    });
    pump.until(&closing, [&] { return closed; });

    if (settings) {
      TEST_CHECK(g_settings_set_string(settings.get(), "icon-theme", "Blue"));
      pump.until(&watcher, [&] { return !watcher->refreshing(); });
      const auto before = changes;
      std::ofstream(root / "config/gtk-3.0/settings.ini") << "[Settings]\ngtk-icon-theme-name=Red\n";
      TEST_CHECK(g_settings_set_string(settings.get(), "icon-theme", "Missing"));
      pump.until(&watcher, [&] { return changes > before; });
      TEST_CHECK(IconResolver::activeThemeName() == "Red");
      // Pending settings notifications cannot reach a destroyed watcher.
      TEST_CHECK(g_settings_set_string(settings.get(), "icon-theme", "Red"));
      watcher.reset();
      bool dispatched = false;
      g_idle_add(
          +[](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &dispatched
      );
      pump.until(nullptr, [&] { return dispatched; });
    }
  }
} // namespace

int main(int argc, char* argv[]) {
  TEST_CHECK(argc == 3);
  luciole::installPixbufImageDecoder();
  run(argv[1], std::string_view(argv[2]) == "schema");
}
