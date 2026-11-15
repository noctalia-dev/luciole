#include "browser/places.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  struct Fixture {
    fs::path root;
    Fixture() {
      const luciole::GString path(g_dir_make_tmp("luciole-places-XXXXXX", nullptr));
      TEST_CHECK(path != nullptr);
      root = path.get();
      fs::create_directories(root / "home");
      fs::create_directories(root / "config");
      g_setenv("HOME", (root / "home").c_str(), TRUE);
      g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), TRUE);
      std::ofstream(root / "config/user-dirs.dirs")
          << "XDG_DESKTOP_DIR=\"$HOME\"\nXDG_DOCUMENTS_DIR=\"$HOME/My Documents\"\n"
             "XDG_MUSIC_DIR=\"$HOME/My Documents\"\nXDG_PICTURES_DIR=\"$HOME/Pictures <literal>\"\n"
             "XDG_VIDEOS_DIR=\"$HOME/Missing videos\"\nXDG_PUBLICSHARE_DIR=\"$HOME\"\n"
             "XDG_TEMPLATES_DIR=\"$HOME/Templates\"\nXDG_DOWNLOAD_DIR=\""
          << (root / "outside downloads").native()
          << "\"\n";
    }
    ~Fixture() { fs::remove_all(root); }
    luciole::Location location(const std::string& name) const {
      return luciole::Location::fromPath((root / name).native());
    }
    luciole::Bookmark pin(const std::string& name) const { return {name, location(name)}; }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int timeout = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    void ready(const luciole::PinService& service) {
      until([&] { return !service.busy(); });
      TEST_CHECK(service.editable());
    }

  private:
    GlibPollSource m_source;
  };
  void testPlacesAndPins() {
    Fixture fixture;
    Pump pump;
    const auto path = fixture.root / "state.toml";
    luciole::StateStore state(path);
    TEST_CHECK(state.load());
    const std::vector<luciole::SavedWindow> windows{
        {.tabs = {{.primary = fixture.location("home"), .secondary = std::nullopt}}}
    };
    TEST_CHECK(state.saveWindows(windows) == luciole::AtomicWriteState::Durable);
    luciole::PinService service(path);
    service.reload();
    pump.ready(service);
    const std::vector<luciole::SavedWindow> updatedWindows{
        {.tabs = {{.primary = fixture.location("restored-tab"), .secondary = std::nullopt}}}
    };
    TEST_CHECK(service.saveWindows(updatedWindows));
    pump.ready(service);
    TEST_CHECK(state.load() && state.state().windows == updatedWindows);
    const auto& places = service.userPlaces();
    TEST_CHECK(
        places.size() == 8
        && places.front().bookmark.label == "Home"
        && places.back().bookmark.location == luciole::Location::fromPath("/")
    );
    const auto documents =
        std::ranges::find_if(places, [](const auto& place) { return place.bookmark.label == "Documents"; });
    const auto downloads =
        std::ranges::find_if(places, [](const auto& place) { return place.bookmark.label == "Downloads"; });
    const auto videos =
        std::ranges::find_if(places, [](const auto& place) { return place.bookmark.label == "Videos"; });
    TEST_CHECK(documents != places.end() && documents->bookmark.location == fixture.location("home/My Documents"));
    TEST_CHECK(downloads != places.end() && downloads->bookmark.location == fixture.location("outside downloads"));
    TEST_CHECK(videos != places.end() && !fs::exists(fixture.root / "home/Missing videos"));

    TEST_CHECK(service.bookmarks().size() == places.size());
    const auto home = places.front().bookmark;
    const auto filesystem = places.back().bookmark;
    const auto existingPath = fixture.root / "existing/state.toml";
    fs::create_directories(existingPath.parent_path());
    {
      std::ofstream existing(existingPath);
      existing
          << "schema_version = 1\nbookmarks = [{label = 'My downloads', uri = '"
          << downloads->bookmark.location.uri()
          << "'}, {label = 'Project one', uri = '"
          << fixture.location("project-one").uri()
          << "'}, {label = 'Project two', uri = '"
          << fixture.location("project-two").uri()
          << "'}]\nwindows = []\n";
    }
    luciole::PinService existing(existingPath);
    existing.reload();
    pump.ready(existing);
    TEST_CHECK(
        existing.bookmarks().size() == places.size() + 2
        && existing.contains(home.location)
        && existing.contains(filesystem.location)
    );
    TEST_CHECK(
        std::ranges::count(existing.bookmarks(), downloads->bookmark.location, &luciole::Bookmark::location) == 1
        && existing.bookmarks()[2].label == "My downloads"
        && existing.bookmarks()[places.size()].label == "Project one"
        && existing.bookmarks().back().label == "Project two"
    );
    luciole::StateStore observed(existingPath);
    TEST_CHECK(observed.load() && !observed.state().placesCustomized && observed.state().bookmarks.size() == 3);
    TEST_CHECK(existing.remove(home.location));
    pump.ready(existing);
    luciole::PinService existingRestarted(existingPath);
    existingRestarted.reload();
    pump.ready(existingRestarted);
    TEST_CHECK(
        !existingRestarted.contains(home.location)
        && existingRestarted.contains(fixture.location("project-one"))
        && observed.load()
        && observed.state().placesCustomized
    );
    TEST_CHECK(existing.rename(filesystem, "Root <files> & storage"));
    pump.ready(existing);
    existingRestarted.reload();
    pump.ready(existingRestarted);
    const auto renamedRoot =
        std::ranges::find(existingRestarted.bookmarks(), filesystem.location, &luciole::Bookmark::location);
    TEST_CHECK(renamedRoot != existingRestarted.bookmarks().end() && renamedRoot->label == "Root <files> & storage");
    TEST_CHECK(!existing.rename(filesystem, "stale label") && !existing.busy());
    const auto renamed = *std::ranges::find(existing.bookmarks(), filesystem.location, &luciole::Bookmark::location);
    TEST_CHECK(existing.rename(renamed, renamed.label) && !existing.busy());
    for (const auto& invalid :
         {std::string{}, std::string(257, 'x'), std::string("line\nbreak"), std::string(1, static_cast<char>(0xff))}) {
      TEST_CHECK(!existing.rename(renamed, invalid) && !existing.busy());
    }
    const auto project =
        *std::ranges::find(existing.bookmarks(), fixture.location("project-one"), &luciole::Bookmark::location);
    const auto oldOrder = existing.bookmarks();
    TEST_CHECK(existing.rename(project, "资料 / <b>Project & work</b>"));
    pump.ready(existing);
    TEST_CHECK(existing.bookmarks().size() == oldOrder.size());
    for (std::size_t i = 0; i < oldOrder.size(); ++i) {
      TEST_CHECK(existing.bookmarks()[i].location == oldOrder[i].location);
    }
    TEST_CHECK(existing.move(project.location, 0));
    pump.ready(existing);
    TEST_CHECK(existing.add({project}, 0));
    pump.ready(existing);
    TEST_CHECK(existing.bookmarks().front().label == "资料 / <b>Project & work</b>");
    TEST_CHECK(existing.remove(project.location));
    pump.ready(existing);
    TEST_CHECK(!existing.rename(project, "recreated"));
    auto externalLabels = existing.bookmarks();
    std::ranges::find(externalLabels, filesystem.location, &luciole::Bookmark::location)->label = "Another writer";
    TEST_CHECK(observed.load() && observed.saveBookmarks(externalLabels) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(existing.rename(renamed, "My root"));
    pump.ready(existing);
    TEST_CHECK(!existing.error().empty() && existing.bookmarks() == externalLabels);

    const luciole::Bookmark trash{"Trash", luciole::Location::trash()};
    const auto beforeTrashEdits = service.bookmarks();
    TEST_CHECK(
        !service.remove(trash.location)
        && !service.removeMany({places.front().bookmark.location, trash.location})
        && !service.rename(trash, "Changed")
        && service.bookmarks() == beforeTrashEdits
    );
    TEST_CHECK(service.move(trash.location, 0));
    pump.ready(service);
    service.reload();
    pump.ready(service);
    TEST_CHECK(service.bookmarks().front() == trash);
    TEST_CHECK(service.add({trash}, service.bookmarks().size()));
    pump.ready(service);
    TEST_CHECK(service.bookmarks().back() == trash);

    TEST_CHECK(service.remove(home.location));
    pump.ready(service);
    service.reload();
    pump.ready(service);
    TEST_CHECK(!service.contains(home.location));
    TEST_CHECK(service.move(filesystem.location, 0));
    pump.ready(service);
    TEST_CHECK(service.bookmarks().front() == filesystem);
    TEST_CHECK(service.add({{"home folder", home.location}}, 1));
    pump.ready(service);
    TEST_CHECK(service.bookmarks()[1] == home);
    std::vector<luciole::Location> defaults;
    for (const auto& pin : service.bookmarks()) {
      if (!pin.location.isTrash()) {
        defaults.push_back(pin.location);
      }
    }
    TEST_CHECK(service.removeMany(defaults));
    pump.ready(service);
    luciole::PinService restarted(path);
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK(restarted.bookmarks() == std::vector<luciole::Bookmark>{trash});

    TEST_CHECK(service.add({fixture.pin("a"), fixture.pin("b"), fixture.pin("c")}, 0));
    pump.ready(service);
    TEST_CHECK(service.error().empty() && service.bookmarks().size() == 4);
    TEST_CHECK(service.add({{"replacement label", fixture.location("b")}, fixture.pin("b")}, 0));
    pump.ready(service);
    TEST_CHECK(service.bookmarks().size() == 4 && service.bookmarks()[0] == fixture.pin("b"));
    TEST_CHECK(service.move(fixture.location("c"), 0));
    pump.ready(service);
    TEST_CHECK(service.bookmarks()[0] == fixture.pin("c") && service.bookmarks()[1] == fixture.pin("b"));
    TEST_CHECK(service.removeMany({fixture.location("a"), fixture.location("b")}));
    pump.ready(service);
    TEST_CHECK((service.bookmarks() == std::vector{fixture.pin("c"), trash}));
    TEST_CHECK(
        state.load() && state.state().windows == updatedWindows && state.state().bookmarks == service.bookmarks()
    );

    TEST_CHECK(service.add({{"line\nbreak", fixture.location("controls")}}, 1));
    pump.ready(service);
    TEST_CHECK(service.bookmarks()[1].label == "line break");
    const auto before = service.bookmarks();
    TEST_CHECK(service.add({{"private", luciole::Location::fromUri("sftp://user:secret@example.org/folder")}}, 2));
    pump.ready(service);
    TEST_CHECK(!service.error().empty() && !service.error().contains("secret") && service.bookmarks() == before);

    luciole::StateStore foreign(path);
    TEST_CHECK(foreign.load());
    auto external = foreign.state().bookmarks;
    external.push_back(fixture.pin("external"));
    TEST_CHECK(foreign.saveBookmarks(external) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(service.add({fixture.pin("not-committed")}, 0));
    pump.ready(service);
    TEST_CHECK(
        !service.error().empty()
        && service.bookmarks() == external
        && !service.contains(fixture.location("not-committed"))
    );
    TEST_CHECK(service.add({fixture.pin("retry")}, 0));
    pump.ready(service);
    TEST_CHECK(service.error().empty() && service.bookmarks().front() == fixture.pin("retry"));

    std::ofstream(path) << "schema_version = 999\n";
    service.reload();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!service.editable() && !service.error().empty() && !service.add({fixture.pin("unsafe")}, 0));
    std::ifstream invalid(path);
    TEST_CHECK(std::string((std::istreambuf_iterator<char>(invalid)), {}) == "schema_version = 999\n");

    auto dying = std::make_unique<luciole::PinService>(fixture.root / "other/state.toml");
    bool destroyed = false;
    dying->setChangedCallback([&] {
      if (!dying->busy()) {
        dying.reset();
        destroyed = true;
      }
    });
    dying->reload();
    pump.until([&] { return destroyed; });
  }
} // namespace

int main() {
  try {
    testPlacesAndPins();
    std::println("Places and pin checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Places test: {}", error.what());
    return 1;
  }
}
