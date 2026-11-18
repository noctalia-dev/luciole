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
    luciole::Bookmark pin(const std::string& name) const { return {name, location(name), {}}; }
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
  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  }
  void replace(const fs::path& path, const std::string& text) {
    write(path.string() + ".new", text);
    fs::rename(path.string() + ".new", path);
  }
  void testSharedDocument(const Fixture& fixture) {
    const auto path = fixture.root / "document/bookmarks";
    const auto legacy = fixture.root / "document/.gtk-bookmarks";
    const auto a = fixture.pin("with space/资料");
    const luciole::Bookmark remote{"Server work", luciole::Location::fromUri("sftp://user@example.org/missing"), {}};
    const auto raw = fixture.location(std::string("raw-\xff"));
    const std::string foreign = "not-an-absolute-uri keep this\nsftp://u:secret@example.org/private hidden\n";
    const std::string initial = a.location.uri()
        + " "
        + a.label
        + '\n'
        + remote.location.uri()
        + " "
        + remote.label
        + '\n'
        + raw.uri()
        + '\n'
        + a.location.uri()
        + " duplicate\n"
        + foreign;
    write(legacy, initial);
    luciole::SharedBookmarks store(path, legacy);
    TEST_CHECK(store.load() && store.bookmarks().size() == 3 && store.bookmarks()[0] == a);
    TEST_CHECK(store.bookmarks()[1] == remote && store.bookmarks()[2].location == raw);
    TEST_CHECK(store.save(store.bookmarks()).state == luciole::AtomicWriteState::Unchanged && !fs::exists(path));
    auto changed = store.bookmarks();
    changed[1].label = "资料 / <Server & work>";
    TEST_CHECK(store.save(changed).state == luciole::AtomicWriteState::Durable);
    TEST_CHECK(read(legacy) == initial && read(path).contains(foreign) && read(path).contains(raw.uri() + '\n'));
    TEST_CHECK(store.load() && store.bookmarks() == changed);
    const auto saved = read(path);
    TEST_CHECK(store.save(changed).state == luciole::AtomicWriteState::Unchanged && read(path) == saved);
    auto external = changed;
    external.push_back(fixture.pin("external"));
    replace(path, saved + external.back().location.uri() + " external\n");
    changed.erase(changed.begin());
    TEST_CHECK(store.save(changed).state == luciole::AtomicWriteState::NotCommitted);
    TEST_CHECK(store.load() && store.bookmarks().size() == 4);
    changed = store.bookmarks();
    std::erase_if(changed, [&](const auto& pin) { return pin.location == a.location; });
    TEST_CHECK(
        store.save(changed).state == luciole::AtomicWriteState::Durable && !read(path).contains(a.location.uri())
    );
    TEST_CHECK(store.load() && store.bookmarks() == changed);
    const auto unchanged = read(path);
    changed.push_back({"Unsafe", luciole::Location::fromUri("sftp://u:secret@example.org/private"), {}});
    const auto refused = store.save(changed);
    TEST_CHECK(
        refused.state == luciole::AtomicWriteState::NotCommitted
        && !refused.error.contains("secret")
        && read(path) == unchanged
    );
    fs::permissions(path, fs::perms::owner_read, fs::perm_options::replace);
    TEST_CHECK(store.load());
    changed = store.bookmarks();
    changed.push_back(fixture.pin("readonly"));
    TEST_CHECK(store.save(changed).state == luciole::AtomicWriteState::NotCommitted && read(path) == unchanged);
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add);
    const auto linked = fixture.root / "linked/bookmarks";
    fs::create_directories(linked.parent_path());
    fs::create_symlink(path, linked);
    luciole::SharedBookmarks symlink(linked, {});
    TEST_CHECK(symlink.load() && symlink.bookmarks() == store.bookmarks());
    TEST_CHECK(symlink.save(changed).state == luciole::AtomicWriteState::NotCommitted && fs::is_symlink(linked));
    write(path, std::string(256 * 1024 + 1, 'x'));
    TEST_CHECK(!store.load() && !store.error().empty());
    write(
        path, a.location.uri() + " Primary\n" + a.location.uri() + " Duplicate\n" + remote.location.uri() + " Server\n"
    );
    TEST_CHECK(store.load());
    changed = store.bookmarks();
    std::ranges::reverse(changed);
    TEST_CHECK(store.save(changed).state == luciole::AtomicWriteState::Durable);
    TEST_CHECK(store.load() && store.bookmarks() == changed);
    fs::remove(path);
    TEST_CHECK(store.load() && store.bookmarks().empty());
    // Modern deletion does not resurrect a legacy document already superseded in this process.
    TEST_CHECK(read(legacy) == initial);
    std::string crowded;
    for (std::size_t i = 0; i < luciole::StateStore::bookmarkLimit + 2; ++i) {
      crowded += fixture.location("overflow-" + std::to_string(i)).uri() + " overflow\n";
    }
    write(path, crowded);
    TEST_CHECK(store.load() && store.bookmarks().size() == luciole::StateStore::bookmarkLimit);
    changed = store.bookmarks();
    changed.front().label = "Edited";
    TEST_CHECK(store.save(changed).state == luciole::AtomicWriteState::Durable);
    TEST_CHECK(read(path).contains(fixture.location("overflow-257").uri()));
    Pump pump;
    luciole::PinService crowdedPlaces(fixture.root / "crowded/state.toml", path, {});
    crowdedPlaces.reload();
    pump.ready(crowdedPlaces);
    const auto first = store.bookmarks().front().location;
    TEST_CHECK(crowdedPlaces.remove(first));
    pump.ready(crowdedPlaces);
    TEST_CHECK(crowdedPlaces.error().empty() && !read(path).contains(first.uri() + ' '));
  }
  void testExistingNetworkLayout(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "network-layout/state.toml";
    const auto shared = fixture.root / "network-layout/bookmarks";
    const auto pin = fixture.pin("existing");
    write(
        path,
        "schema_version = 1\nplaces_customized = true\nbookmarks = [{label='existing', uri='"
            + pin.location.uri()
            + "'}]\n"
    );
    write(shared, pin.location.uri() + " existing\n");
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    const auto network = luciole::Location::network();
    TEST_CHECK(service.bookmarks().front() == pin && service.bookmarks().back().location == network);
    TEST_CHECK(service.move(network, 0));
    pump.ready(service);
    TEST_CHECK(read(shared) == pin.location.uri() + " existing\n");
    luciole::PinService restarted(path, shared, {});
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK(restarted.bookmarks().front().location == network);
    // An existing shared Network bookmark must not create a second built-in row.
    replace(shared, read(shared) + network.uri() + " Network\n");
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK(std::ranges::count(restarted.bookmarks(), network, &luciole::Bookmark::location) == 1);
  }
  void testPlacesAndPins(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "main/state.toml";
    const auto sharedPath = fixture.root / "main/gtk-3.0/bookmarks";
    const auto stale = fixture.pin("old-luciole-pin");
    luciole::StateStore state(path);
    TEST_CHECK(state.load());
    const std::vector<luciole::SavedWindow> windows{
        {.tabs = {{.primary = fixture.location("home"), .secondary = std::nullopt}}}
    };
    TEST_CHECK(state.saveWindows(windows) == luciole::AtomicWriteState::Durable);
    // Old private pins are not imported, even when the shared file is absent.
    write(path, "schema_version = 1\nbookmarks = [{label='Old pin', uri='" + stale.location.uri() + "'}]\n");
    luciole::PinService service(path, sharedPath, {});
    service.reload();
    pump.ready(service);
    TEST_CHECK(!service.contains(stale.location) && !fs::exists(sharedPath));
    const auto& places = service.userPlaces();
    TEST_CHECK(
        places.size() == 8
        && places.front().bookmark.label == "Home"
        && places.back().bookmark.location == luciole::Location::network()
    );
    const auto documents = std::ranges::find_if(places, [](const auto& p) { return p.bookmark.label == "Documents"; });
    TEST_CHECK(documents != places.end() && documents->bookmark.location == fixture.location("home/My Documents"));
    TEST_CHECK(std::ranges::any_of(places, [&](const auto& p) {
      return p.bookmark.label == "Downloads" && p.bookmark.location == fixture.location("outside downloads");
    }));
    TEST_CHECK(service.bookmarks().size() == places.size());
    const auto home = places.front().bookmark;
    const auto downloads =
        std::ranges::find(places, std::string("Downloads"), [](const auto& p) { return p.bookmark.label; })->bookmark;
    const luciole::Bookmark trash{"Trash", luciole::Location::trash(), {}};
    const luciole::Bookmark network{"Network", luciole::Location::network(), {}};
    TEST_CHECK(service.contains(network.location));
    TEST_CHECK(!service.remove(network.location) && !service.rename(network, "changed"));
    TEST_CHECK(!service.setIcon(network, "star"));
    const auto a = fixture.pin("a");
    const auto b = fixture.pin("b");
    TEST_CHECK(service.add({a, b, a}, service.bookmarks().size()));
    pump.ready(service);
    TEST_CHECK(service.contains(a.location) && service.contains(b.location));
    TEST_CHECK(read(sharedPath) == a.location.uri() + " a\n" + b.location.uri() + " b\n");
    TEST_CHECK(!read(sharedPath).contains(home.location.uri()) && !read(sharedPath).contains(trash.location.uri()));
    luciole::PinService peer(fixture.root / "peer/state.toml", sharedPath, {});
    peer.reload();
    pump.ready(peer);
    TEST_CHECK(peer.contains(a.location));
    TEST_CHECK(service.rename(a, "资料 / <Project & work>"));
    pump.ready(service);
    pump.until([&] {
      return !peer.busy() && std::ranges::any_of(peer.bookmarks(), [&](const auto& pin) {
        return pin.location == a.location && pin.label == "资料 / <Project & work>";
      });
    });
    const auto renamed = *std::ranges::find(service.bookmarks(), a.location, &luciole::Bookmark::location);
    TEST_CHECK(!service.rename(a, "stale") && service.rename(renamed, renamed.label));
    for (const auto& invalid :
         {std::string{}, std::string(257, 'x'), std::string("line\nbreak"), std::string(1, static_cast<char>(0xff))}) {
      TEST_CHECK(!service.rename(renamed, invalid));
    }
    const auto sharedBeforeMove = read(sharedPath);
    TEST_CHECK(service.move(network.location, 0));
    pump.ready(service);
    TEST_CHECK(service.bookmarks().front() == network && read(sharedPath) == sharedBeforeMove);
    {
      luciole::PinService reloaded(path, sharedPath, {});
      reloaded.reload();
      pump.ready(reloaded);
      TEST_CHECK(reloaded.bookmarks().front() == network);
    }
    TEST_CHECK(service.move(b.location, 0));
    pump.ready(service);
    luciole::SharedBookmarks observed(sharedPath, {});
    TEST_CHECK(observed.load() && observed.bookmarks().front() == b);
    TEST_CHECK(service.remove(home.location));
    pump.ready(service);
    TEST_CHECK(service.rename(downloads, "Root <files> & storage"));
    pump.ready(service);
    TEST_CHECK(service.move(trash.location, 0));
    pump.ready(service);
    TEST_CHECK(!service.remove(trash.location) && !service.rename(trash, "changed"));
    luciole::PinService restarted(path, sharedPath, {});
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK(!restarted.contains(home.location) && restarted.bookmarks().front() == trash);
    TEST_CHECK(std::ranges::any_of(restarted.bookmarks(), [&](const auto& pin) {
      return pin.location == downloads.location && pin.label == "Root <files> & storage";
    }));
    // Icon overrides stay private and follow their URI when another application reorders or relabels rows.
    const auto sharedBeforeIcon = read(sharedPath);
    const auto pinned = *std::ranges::find(service.bookmarks(), a.location, &luciole::Bookmark::location);
    TEST_CHECK(service.setIcon(pinned, "star"));
    pump.ready(service);
    TEST_CHECK(read(sharedPath) == sharedBeforeIcon && !service.setIcon(pinned, "heart"));
    replace(
        sharedPath,
        fixture.pin("first").location.uri()
            + " First\n"
            + fixture.pin("second").location.uri()
            + " Second\n"
            + b.location.uri()
            + " b\n"
            + a.location.uri()
            + " Relabelled\n"
    );
    pump.until([&] { return !service.busy() && service.contains(fixture.location("first")); });
    const auto carried = std::ranges::find(service.bookmarks(), a.location, &luciole::Bookmark::location);
    TEST_CHECK(carried != service.bookmarks().end() && carried->label == "Relabelled" && carried->iconName == "star");
    // External reorder, relabel and deletion override the private layout cache.
    replace(
        sharedPath,
        a.location.uri() + " From Nemo\n" + fixture.pin("missing remote mount").location.uri() + " Missing\n"
    );
    pump.until([&] {
      return !service.busy()
          && !service.contains(b.location)
          && service.contains(fixture.location("missing remote mount"));
    });
    TEST_CHECK(std::ranges::any_of(service.bookmarks(), [&](const auto& pin) {
      return pin.location == a.location && pin.label == "From Nemo";
    }));
    // An edit submitted before the monitor dispatches must not overwrite another writer.
    const auto external = read(sharedPath) + fixture.pin("concurrent").location.uri() + " Concurrent\n";
    replace(sharedPath, external);
    TEST_CHECK(service.add({fixture.pin("not-committed")}, 0));
    pump.ready(service);
    TEST_CHECK(
        !service.error().empty()
        && !service.contains(fixture.location("not-committed"))
        && service.contains(fixture.location("concurrent"))
        && read(sharedPath) == external
    );
    TEST_CHECK(service.remove(a.location));
    pump.ready(service);
    TEST_CHECK(!read(sharedPath).contains(a.location.uri()));
    service.reload();
    pump.ready(service);
    TEST_CHECK(!service.contains(a.location));
    fs::remove(sharedPath);
    pump.until([&] { return !service.busy() && !service.contains(fixture.location("concurrent")); });
    TEST_CHECK(service.add({fixture.pin("recreated")}, 0));
    pump.ready(service);
    TEST_CHECK(read(sharedPath).contains("recreated"));
    TEST_CHECK(service.saveWindows(windows));
    pump.ready(service);
    TEST_CHECK(state.load() && state.state().windows == windows);
    const auto beforeUnsafe = service.bookmarks();
    TEST_CHECK(service.add({{"private", luciole::Location::fromUri("sftp://u:secret@example.org/private"), {}}}, 0));
    pump.ready(service);
    TEST_CHECK(!service.error().empty() && !service.error().contains("secret") && service.bookmarks() == beforeUnsafe);
    write(path, "schema_version = 999\n");
    service.reload();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!service.editable() && !service.error().empty() && read(path) == "schema_version = 999\n");

    // A previously missing gtk directory and atomic replacement of that directory are watched.
    const auto newPath = fixture.root / "new-parent/deeper/gtk-3.0/bookmarks";
    luciole::PinService missing(fixture.root / "new-state/state.toml", newPath, {});
    missing.reload();
    pump.ready(missing);
    write(newPath, a.location.uri() + " Newly created\n");
    pump.until([&] { return !missing.busy() && missing.contains(a.location); });
    fs::rename(newPath.parent_path(), newPath.parent_path().string() + ".old");
    write(newPath, b.location.uri() + " Replacement directory\n");
    pump.until([&] { return !missing.busy() && missing.contains(b.location) && !missing.contains(a.location); });
    const auto linkedPath = fixture.root / "managed/bookmarks";
    fs::create_directories(linkedPath.parent_path());
    fs::create_symlink(newPath, linkedPath);
    luciole::PinService managed(fixture.root / "managed-state/state.toml", linkedPath, {});
    managed.reload();
    pump.ready(managed);
    TEST_CHECK(managed.contains(b.location));
    replace(newPath, a.location.uri() + " Managed edit\n");
    pump.until([&] { return !managed.busy() && managed.contains(a.location) && !managed.contains(b.location); });
    auto dying = std::make_unique<luciole::PinService>(fixture.root / "dying/state.toml", newPath, fs::path{});
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
    Fixture fixture;
    testSharedDocument(fixture);
    testPlacesAndPins(fixture);
    testExistingNetworkLayout(fixture);
    std::println("Shared bookmarks and Places checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Places test: {}", error.what());
    return 1;
  }
}
