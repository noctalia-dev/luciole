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
  std::size_t row(const luciole::PinService& service, const luciole::Location& location) {
    const auto found = std::ranges::find(service.bookmarks(), location, &luciole::Bookmark::location);
    TEST_CHECK(found != service.bookmarks().end());
    return static_cast<std::size_t>(found - service.bookmarks().begin());
  }
  void testSeparators(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "separators/state.toml";
    const auto shared = fixture.root / "separators/bookmarks";
    const auto a = fixture.pin("sep-a");
    const auto b = fixture.pin("sep-b");
    const auto c = fixture.pin("sep-c");
    write(shared, a.location.uri() + " sep-a\n" + b.location.uri() + " sep-b\n" + c.location.uri() + " sep-c\n");
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    const auto count = service.bookmarks().size();
    TEST_CHECK(service.separators().empty());

    // A separator sits in front of a row; never before the first, past the last or twice in one gap.
    TEST_CHECK(service.addSeparator(row(service, b.location)));
    // Shown before the write ends, since nothing else would redraw Places for a separator-only change.
    TEST_CHECK(service.busy() && (service.separators() == std::vector{row(service, b.location)}));
    pump.ready(service);
    TEST_CHECK((service.separators() == std::vector{row(service, b.location)}));
    TEST_CHECK(
        !service.addSeparator(0) && !service.addSeparator(count) && !service.addSeparator(row(service, b.location))
    );
    TEST_CHECK(read(path).contains("place_separators") && !read(shared).contains("separator"));
    luciole::PinService restarted(path, shared, {});
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK((restarted.separators() == service.separators()));

    // Moving a pin keeps the separator's row, so the pin crosses it.
    const auto before = service.separators();
    TEST_CHECK(service.move(c.location, row(service, a.location)));
    pump.ready(service);
    TEST_CHECK(service.separators() == before && row(service, c.location) < service.separators().front());
    // Removing a pin keeps the separator in front of the same row.
    const auto ahead = service.bookmarks()[service.separators().front()].location;
    TEST_CHECK(service.remove(c.location));
    pump.ready(service);
    TEST_CHECK(service.bookmarks()[service.separators().front()].location == ahead);

    // Moving and removing a separator.
    const auto at = service.separators().front();
    TEST_CHECK(service.moveSeparatorTo(at, at - 1));
    pump.ready(service);
    TEST_CHECK((service.separators() == std::vector{at - 1}));
    TEST_CHECK(service.moveSeparatorTo(at - 1, at));
    pump.ready(service);
    TEST_CHECK((service.separators() == std::vector{at}));
    TEST_CHECK(!service.moveSeparatorTo(at, 0) && !service.removeSeparator(at + 1));
    TEST_CHECK(service.removeSeparator(at));
    pump.ready(service);
    TEST_CHECK(service.separators().empty() && !read(path).contains("place_separators"));

    // Another instance changes only the separators. state.toml isn't watched, so this one still has the old ones:
    // its next write reports the conflict and adopts the other instance's separators instead of overwriting them.
    luciole::PinService other(path, shared, {});
    other.reload();
    pump.ready(other);
    TEST_CHECK(other.addSeparator(row(other, b.location)));
    pump.ready(other);
    const auto theirs = read(path);
    TEST_CHECK(service.separators().empty() && service.addSeparator(row(service, a.location)));
    pump.ready(service);
    TEST_CHECK(!service.error().empty() && read(path) == theirs);
    TEST_CHECK((service.separators() == other.separators()));

    // A write that can't read state.toml keeps showing the separators on record, not the unsaved ones.
    const auto kept = service.separators();
    write(path, "schema_version = 999\n");
    TEST_CHECK(service.addSeparator(row(service, a.location)));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!service.error().empty() && service.separators() == kept && read(path) == "schema_version = 999\n");
  }
  // With Network hidden, a gap on screen spans two rows: separators are placed, moved and drawn per shown gap.
  void testSeparatorGaps(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "gaps/state.toml";
    const auto shared = fixture.root / "gaps/bookmarks";
    const auto a = fixture.pin("gap-a");
    write(shared, a.location.uri() + " gap-a\n");
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    const auto network = row(service, luciole::Location::network());
    const auto after = row(service, a.location);
    TEST_CHECK(network > 1 && after == network + 1);

    const luciole::SeparatorGaps shown(service, true);
    TEST_CHECK(shown.gap(0) == std::nullopt && shown.gap(network) == network && shown.free(network));
    const luciole::SeparatorGaps hidden(service, false);
    // The gap under the row above Network is the one above the next shown row; Network's own gap is gone.
    TEST_CHECK(hidden.nextShown(network - 1) == after && hidden.previousShown(after) == network - 1);
    TEST_CHECK(!hidden.free(network) && hidden.free(after));

    // A separator saved in front of the hidden row is drawn above the next shown one, which then takes no other.
    TEST_CHECK(service.addSeparator(network));
    pump.ready(service);
    const luciole::SeparatorGaps drawn(service, false);
    TEST_CHECK(drawn.gap(network) == after && drawn.separatorAbove(after) == network && !drawn.free(after));
    TEST_CHECK(luciole::SeparatorGaps(service, true).free(after));
  }
  // Pins dropped next to a separator land on the side of its line the drop line showed, and the separators stay with
  // the rows that don't move.
  void testSeparatorDrops(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "drops/state.toml";
    const auto shared = fixture.root / "drops/bookmarks";
    const auto a = fixture.pin("drop-a");
    const auto b = fixture.pin("drop-b");
    const auto c = fixture.pin("drop-c");
    const auto d = fixture.pin("drop-d");
    const auto e = fixture.pin("drop-e");
    write(
        shared,
        a.location.uri()
            + " drop-a\n"
            + b.location.uri()
            + " drop-b\n"
            + c.location.uri()
            + " drop-c\n"
            + d.location.uri()
            + " drop-d\n"
    );
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    const auto first = row(service, a.location);
    // The pins from `a` on, with "|" for a separator, to compare whole layouts.
    const auto layout = [&] {
      std::string text;
      for (std::size_t i = first; i < service.bookmarks().size(); ++i) {
        if (std::ranges::contains(service.separators(), i)) {
          text += "| ";
        }
        text += service.bookmarks()[i].label + " ";
      }
      return text;
    };
    const auto pin = [&](const luciole::Bookmark& place) { return service.bookmarks()[row(service, place.location)]; };
    TEST_CHECK(service.addSeparator(row(service, c.location)));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-a drop-b | drop-c drop-d ");

    // Dropped below the line in front of c: b goes under it, in front of c.
    TEST_CHECK(service.add({pin(b)}, row(service, c.location), true));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-a | drop-b drop-c drop-d ");
    // Dropped above the line it now sits under: b goes back over it.
    TEST_CHECK(service.add({pin(b)}, row(service, b.location), false));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-a drop-b | drop-c drop-d ");
    // The row right under a separator, dragged elsewhere, leaves the separator in front of the row after it.
    TEST_CHECK(service.add({pin(c)}, row(service, a.location)));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-c drop-a drop-b | drop-d ");
    // A new pin dropped below or above a line.
    TEST_CHECK(service.add({e}, row(service, d.location), true));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-c drop-a drop-b | drop-e drop-d ");
    TEST_CHECK(service.add({pin(e)}, row(service, e.location), false));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-c drop-a drop-b drop-e | drop-d ");
    // Moving the only row of a group away empties it: its separator merges with the next one.
    TEST_CHECK(service.addSeparator(row(service, a.location)));
    pump.ready(service);
    TEST_CHECK(service.addSeparator(row(service, b.location)));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-c | drop-a | drop-b drop-e | drop-d ");
    TEST_CHECK(service.add({pin(a)}, row(service, d.location), true));
    pump.ready(service);
    TEST_CHECK(layout() == "drop-c | drop-b drop-e | drop-a drop-d ");
  }
  void testSeparatorsFollowExternalChanges(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "external/state.toml";
    const auto shared = fixture.root / "external/bookmarks";
    const auto a = fixture.pin("ext-a");
    const auto b = fixture.pin("ext-b");
    const auto c = fixture.pin("ext-c");
    const auto d = fixture.pin("ext-d");
    const auto line = [](const luciole::Bookmark& pin) { return pin.location.uri() + " " + pin.label + "\n"; };
    write(shared, line(a) + line(b) + line(c) + line(d));
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    TEST_CHECK(service.addSeparator(row(service, c.location)));
    pump.ready(service);
    const auto inFront = [&](const luciole::PinService& pins, const luciole::Bookmark& pin) {
      return pins.separators() == std::vector{row(pins, pin.location)};
    };
    const auto restartedInFront = [&](const luciole::Bookmark& pin) {
      luciole::PinService restarted(path, shared, {});
      restarted.reload();
      pump.ready(restarted);
      return inFront(restarted, pin);
    };

    // Another application unpins a place above the separator: the separator stays in front of c, now and after a
    // restart, instead of keeping its row number and landing in front of d.
    replace(shared, line(b) + line(c) + line(d));
    service.reload();
    pump.ready(service);
    TEST_CHECK(inFront(service, c) && restartedInFront(c));
    // ... unpins the place right below it: it moves to the next one still listed.
    replace(shared, line(b) + line(d));
    service.reload();
    pump.ready(service);
    TEST_CHECK(inFront(service, d) && restartedInFront(d));
    // ... and reorders them: it stays in front of d wherever d goes.
    replace(shared, line(d) + line(b));
    service.reload();
    pump.ready(service);
    TEST_CHECK(inFront(service, d) && restartedInFront(d));
    // A later write from this instance stores the moved separator.
    TEST_CHECK(service.addSeparator(row(service, b.location)));
    pump.ready(service);
    TEST_CHECK(service.error().empty() && service.separators().size() == 2);
  }
  void testSeparatorHeadings(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "headings/state.toml";
    const auto shared = fixture.root / "headings/bookmarks";
    const auto a = fixture.pin("head-a");
    const auto b = fixture.pin("head-b");
    const auto c = fixture.pin("head-c");
    write(shared, a.location.uri() + " head-a\n" + b.location.uri() + " head-b\n" + c.location.uri() + " head-c\n");
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    const auto first = row(service, a.location);
    const auto second = row(service, c.location);
    TEST_CHECK(service.addSeparator(first));
    pump.ready(service);
    TEST_CHECK(service.addSeparator(second));
    pump.ready(service);

    // A heading belongs to one separator; a bad one or a missing separator is refused.
    TEST_CHECK(service.separatorHeading(first).empty() && !service.setSeparatorHeading(first + 1, "Nowhere"));
    TEST_CHECK(!service.setSeparatorHeading(first, "Bad\nline"));
    TEST_CHECK(!service.setSeparatorHeading(first, "   ") && !service.setSeparatorHeading(first, "\u00a0\u3000"));
    TEST_CHECK(service.setSeparatorHeading(first, "Projects"));
    pump.ready(service);
    TEST_CHECK(service.separatorHeading(first) == "Projects" && service.separatorHeading(second).empty());
    TEST_CHECK(read(path).contains("Projects") && !read(shared).contains("Projects"));
    luciole::PinService restarted(path, shared, {});
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK(restarted.separatorHeading(first) == "Projects");

    // Moving the separator takes its heading along.
    TEST_CHECK(service.moveSeparatorTo(first, first + 1));
    pump.ready(service);
    TEST_CHECK(service.separatorHeading(first + 1) == "Projects" && service.separatorHeading(second).empty());
    // Removing a pin in front of it keeps the heading with its separator.
    TEST_CHECK(service.remove(a.location));
    pump.ready(service);
    TEST_CHECK(service.separatorHeading(first) == "Projects");
    // An empty heading turns it back into a plain line, and the key leaves the file.
    TEST_CHECK(service.setSeparatorHeading(first, {}));
    pump.ready(service);
    TEST_CHECK(service.separatorHeading(first).empty() && !read(path).contains("place_separator_headings"));

    // Another instance gives the separator a heading; this one, still without it, reports the conflict on its next
    // write and takes the heading instead of dropping it.
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK(restarted.setSeparatorHeading(first, "Theirs"));
    pump.ready(restarted);
    const auto theirs = read(path);
    TEST_CHECK(service.separatorHeading(first).empty() && service.removeSeparator(first + 1));
    pump.ready(service);
    TEST_CHECK(!service.error().empty() && read(path) == theirs && service.separatorHeading(first) == "Theirs");
  }
  void testMergedHeadings(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "merged-headings/state.toml";
    const auto shared = fixture.root / "merged-headings/bookmarks";
    const auto x = fixture.pin("merge-x");
    const auto y = fixture.pin("merge-y");
    const auto z = fixture.pin("merge-z");
    write(shared, x.location.uri() + " merge-x\n" + y.location.uri() + " merge-y\n" + z.location.uri() + " merge-z\n");
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    // "Solo" heads a group of one pin, x; "Rest" heads y and z.
    for (const auto& [pin, heading] : {std::pair{x, "Solo"}, std::pair{y, "Rest"}}) {
      TEST_CHECK(service.addSeparator(row(service, pin.location)));
      pump.ready(service);
      TEST_CHECK(service.setSeparatorHeading(row(service, pin.location), heading));
      pump.ready(service);
    }
    // Unpinning x empties its group: the two separators become one, in front of y, and it heads y's group.
    TEST_CHECK(service.remove(x.location));
    pump.ready(service);
    TEST_CHECK((service.separators() == std::vector{row(service, y.location)}));
    TEST_CHECK(service.separatorHeading(row(service, y.location)) == "Rest");
  }
  // A pin dropped next to a headed separator leaves each heading on its line.
  void testHeadingsFollowDrops(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "drop-headings/state.toml";
    const auto shared = fixture.root / "drop-headings/bookmarks";
    const auto p = fixture.pin("hd-p");
    const auto q = fixture.pin("hd-q");
    const auto r = fixture.pin("hd-r");
    write(shared, p.location.uri() + " hd-p\n" + q.location.uri() + " hd-q\n" + r.location.uri() + " hd-r\n");
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    for (const auto& [pin, heading] : {std::pair{q, "Top"}, std::pair{r, "Low"}}) {
      TEST_CHECK(service.addSeparator(row(service, pin.location)));
      pump.ready(service);
      TEST_CHECK(service.setSeparatorHeading(row(service, pin.location), heading));
      pump.ready(service);
    }
    const auto pin = [&](const luciole::Bookmark& place) { return service.bookmarks()[row(service, place.location)]; };
    // p dropped below the "Top" line: "Top" now sits in front of p, "Low" still in front of r.
    TEST_CHECK(service.add({pin(p)}, row(service, q.location), true));
    pump.ready(service);
    TEST_CHECK(service.separatorHeading(row(service, p.location)) == "Top");
    TEST_CHECK(service.separatorHeading(row(service, r.location)) == "Low");
    // p and q dropped below the "Low" line empty the "Top" group: the lines merge, and "Low" heads the rows below.
    TEST_CHECK(service.add({pin(p), pin(q)}, row(service, r.location), true));
    pump.ready(service);
    TEST_CHECK((service.separators() == std::vector{row(service, p.location)}));
    TEST_CHECK(service.separatorHeading(row(service, p.location)) == "Low");
  }
  void testHeadingsFollowExternalChanges(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "external-headings/state.toml";
    const auto shared = fixture.root / "external-headings/bookmarks";
    const auto a = fixture.pin("exth-a");
    const auto b = fixture.pin("exth-b");
    const auto c = fixture.pin("exth-c");
    const auto line = [](const luciole::Bookmark& pin) { return pin.location.uri() + " " + pin.label + "\n"; };
    write(shared, line(a) + line(b) + line(c));
    luciole::PinService service(path, shared, {});
    service.reload();
    pump.ready(service);
    TEST_CHECK(service.addSeparator(row(service, c.location)));
    pump.ready(service);
    TEST_CHECK(service.setSeparatorHeading(row(service, c.location), "Last"));
    pump.ready(service);
    // Another application unpins a place above it: the heading stays with its separator, in front of c.
    replace(shared, line(b) + line(c));
    service.reload();
    pump.ready(service);
    TEST_CHECK(service.separatorHeading(row(service, c.location)) == "Last");
    luciole::PinService restarted(path, shared, {});
    restarted.reload();
    pump.ready(restarted);
    TEST_CHECK((restarted.separators() == std::vector{row(restarted, c.location)}));
    TEST_CHECK(restarted.separatorHeading(row(restarted, c.location)) == "Last");
  }
} // namespace

int main() {
  try {
    Fixture fixture;
    testSharedDocument(fixture);
    testPlacesAndPins(fixture);
    testExistingNetworkLayout(fixture);
    testSeparators(fixture);
    testSeparatorGaps(fixture);
    testSeparatorDrops(fixture);
    testSeparatorsFollowExternalChanges(fixture);
    testSeparatorHeadings(fixture);
    testMergedHeadings(fixture);
    testHeadingsFollowDrops(fixture);
    testHeadingsFollowExternalChanges(fixture);
    std::println("Shared bookmarks and Places checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Places test: {}", error.what());
    return 1;
  }
}
