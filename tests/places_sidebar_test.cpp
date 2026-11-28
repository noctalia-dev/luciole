#include "browser/places.h"
#include "event_loop/glib_poll_source.h"
#include "fs/drive_monitor.h"
#include "fs/gio_interop.h"
#include "fs/system_mounts.h"
#include "test_check.h"
#include "ui/browser/places_sidebar.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gio/gnativevolumemonitor.h>
#include <print>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  class MetricsRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float size, const TextFont&, float width, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * size * 0.5F;
      return {
          .width = width > 0 ? std::min(width, natural) : natural, .bottom = size, .capHeight = size, .lineCount = 1
      };
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.bottom = size, .capHeight = size}; }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1; }
  };

  struct MockDrive {
    GObject parent;
    gboolean removable;
    gboolean canStop;
    gboolean mediaRemovable;
    gboolean canEject;
  };
  struct MockDriveClass {
    GObjectClass parent;
  };
  struct MockMount {
    GObject parent;
    char* name;
    char* uri;
    GVolume* volume;
    gboolean canEject;
  };
  struct MockMountClass {
    GObjectClass parent;
  };
  struct MockVolume {
    GObject parent;
    char* name;
    char* id;
    char* unixDevice;
    char* nfsMount;
    char* volumeClass;
    char* activationUri;
    GDrive* drive;
    GMount* mount;
  };
  struct MockVolumeClass {
    GObjectClass parent;
  };
  struct MockMonitor {
    GNativeVolumeMonitor parent;
  };
  struct MockMonitorClass {
    GNativeVolumeMonitorClass parent;
  };

  GVolumeMonitor* activeMonitor = nullptr;
  std::vector<GVolume*> currentVolumes;
  std::vector<GMount*> currentMounts;

  void driveInterface(GDriveIface* iface);
  void mountInterface(GMountIface* iface);
  void volumeInterface(GVolumeIface* iface);

  // GObject requires these generated C entry-point names.
  // NOLINTBEGIN(readability-identifier-naming)
  G_DEFINE_TYPE_WITH_CODE(MockDrive, mock_drive, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_DRIVE, driveInterface))
  G_DEFINE_TYPE_WITH_CODE(MockMount, mock_mount, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_MOUNT, mountInterface))
  G_DEFINE_TYPE_WITH_CODE(MockVolume, mock_volume, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_VOLUME, volumeInterface))
  G_DEFINE_TYPE(MockMonitor, mock_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

  void mock_drive_init(MockDrive*) {}
  void mock_drive_class_init(MockDriveClass*) {}

  void mock_mount_init(MockMount*) {}
  void mock_mount_class_init(MockMountClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
      auto* mount = reinterpret_cast<MockMount*>(object);
      g_free(mount->name);
      g_free(mount->uri);
      G_OBJECT_CLASS(mock_mount_parent_class)->finalize(object);
    };
  }

  void mock_volume_init(MockVolume*) {}
  void mock_volume_class_init(MockVolumeClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
      auto* volume = reinterpret_cast<MockVolume*>(object);
      g_free(volume->name);
      g_free(volume->id);
      g_free(volume->unixDevice);
      g_free(volume->nfsMount);
      g_free(volume->volumeClass);
      g_free(volume->activationUri);
      g_clear_object(&volume->drive);
      g_clear_object(&volume->mount);
      G_OBJECT_CLASS(mock_volume_parent_class)->finalize(object);
    };
  }

  void mock_monitor_init(MockMonitor* monitor) { activeMonitor = G_VOLUME_MONITOR(monitor); }
  void mock_monitor_class_init(MockMonitorClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
      if (activeMonitor == G_VOLUME_MONITOR(object)) {
        activeMonitor = nullptr;
      }
      G_OBJECT_CLASS(mock_monitor_parent_class)->finalize(object);
    };
    auto* monitor = G_VOLUME_MONITOR_CLASS(klass);
    monitor->is_supported = +[] -> gboolean { return TRUE; };
    monitor->get_volumes = +[](GVolumeMonitor*) -> GList* {
      GList* result = nullptr;
      for (auto* volume : currentVolumes) {
        result = g_list_append(result, g_object_ref(volume));
      }
      return result;
    };
    monitor->get_mounts = +[](GVolumeMonitor*) -> GList* {
      GList* result = nullptr;
      for (auto* mount : currentMounts) {
        result = g_list_append(result, g_object_ref(mount));
      }
      return result;
    };
    monitor->get_connected_drives = +[](GVolumeMonitor*) -> GList* { return nullptr; };
  }
  // NOLINTEND(readability-identifier-naming)

  void driveInterface(GDriveIface* iface) {
    iface->is_removable = +[](GDrive* d) -> gboolean { return reinterpret_cast<MockDrive*>(d)->removable; };
    iface->can_stop = +[](GDrive* d) -> gboolean { return reinterpret_cast<MockDrive*>(d)->canStop; };
    iface->is_media_removable = +[](GDrive* d) -> gboolean { return reinterpret_cast<MockDrive*>(d)->mediaRemovable; };
    iface->can_eject = +[](GDrive* d) -> gboolean { return reinterpret_cast<MockDrive*>(d)->canEject; };
  }

  void mountInterface(GMountIface* iface) {
    iface->get_root = +[](GMount* m) { return g_file_new_for_uri(reinterpret_cast<MockMount*>(m)->uri); };
    iface->get_name = +[](GMount* m) { return g_strdup(reinterpret_cast<MockMount*>(m)->name); };
    iface->get_volume = +[](GMount* m) -> GVolume* {
      auto* volume = reinterpret_cast<MockMount*>(m)->volume;
      return volume ? G_VOLUME(g_object_ref(volume)) : nullptr;
    };
    iface->get_drive = +[](GMount*) -> GDrive* { return nullptr; };
    iface->can_unmount = +[](GMount*) -> gboolean { return TRUE; };
    iface->can_eject = +[](GMount* m) -> gboolean { return reinterpret_cast<MockMount*>(m)->canEject; };
  }

  void volumeInterface(GVolumeIface* iface) {
    iface->get_name = +[](GVolume* v) { return g_strdup(reinterpret_cast<MockVolume*>(v)->name); };
    iface->get_identifier = +[](GVolume* source, const char* kind) -> char* {
      const auto* volume = reinterpret_cast<MockVolume*>(source);
      const std::string_view key(kind);
      if (key == G_VOLUME_IDENTIFIER_KIND_CLASS) {
        return volume->volumeClass ? g_strdup(volume->volumeClass) : nullptr;
      }
      if (key == G_VOLUME_IDENTIFIER_KIND_UUID) {
        return volume->id ? g_strdup(volume->id) : nullptr;
      }
      if (key == G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) {
        return volume->unixDevice ? g_strdup(volume->unixDevice) : nullptr;
      }
      if (key == G_VOLUME_IDENTIFIER_KIND_NFS_MOUNT) {
        return volume->nfsMount ? g_strdup(volume->nfsMount) : nullptr;
      }
      return nullptr;
    };
    iface->get_drive = +[](GVolume* source) -> GDrive* {
      auto* drive = reinterpret_cast<MockVolume*>(source)->drive;
      return drive ? G_DRIVE(g_object_ref(drive)) : nullptr;
    };
    iface->get_mount = +[](GVolume* source) -> GMount* {
      auto* mount = reinterpret_cast<MockVolume*>(source)->mount;
      return mount ? G_MOUNT(g_object_ref(mount)) : nullptr;
    };
    iface->get_activation_root = +[](GVolume* source) -> GFile* {
      const auto* uri = reinterpret_cast<MockVolume*>(source)->activationUri;
      return uri ? g_file_new_for_uri(uri) : nullptr;
    };
  }

  struct DriveSpec {
    bool removable = false;
    bool canStop = false;
    bool mediaRemovable = false;
    bool canEject = false;
  };

  luciole::GioPtr<GDrive> makeDrive(DriveSpec spec = {}) {
    auto* drive = reinterpret_cast<MockDrive*>(g_object_new(mock_drive_get_type(), nullptr));
    drive->removable = spec.removable ? TRUE : FALSE;
    drive->canStop = spec.canStop ? TRUE : FALSE;
    drive->mediaRemovable = spec.mediaRemovable ? TRUE : FALSE;
    drive->canEject = spec.canEject ? TRUE : FALSE;
    return luciole::GioPtr<GDrive>(G_DRIVE(drive));
  }

  luciole::GioPtr<GMount>
  makeMount(const char* name, const char* uri, GVolume* volume = nullptr, bool canEject = false) {
    auto* mount = reinterpret_cast<MockMount*>(g_object_new(mock_mount_get_type(), nullptr));
    mount->name = g_strdup(name);
    mount->uri = g_strdup(uri);
    mount->volume = volume;
    mount->canEject = canEject ? TRUE : FALSE;
    return luciole::GioPtr<GMount>(G_MOUNT(mount));
  }

  struct VolumeSpec {
    const char* name = nullptr;
    const char* id = nullptr;
    const char* unixDevice = nullptr;
    const char* nfsMount = nullptr;
    const char* volumeClass = nullptr;
    GDrive* drive = nullptr;
    const char* mountUri = nullptr;
    bool mountCanEject = false;
    const char* activationUri = nullptr;
  };

  luciole::GioPtr<GVolume> makeVolume(VolumeSpec spec) {
    auto* volume = reinterpret_cast<MockVolume*>(g_object_new(mock_volume_get_type(), nullptr));
    volume->name = g_strdup(spec.name);
    volume->id = spec.id ? g_strdup(spec.id) : nullptr;
    volume->unixDevice = spec.unixDevice ? g_strdup(spec.unixDevice) : nullptr;
    volume->nfsMount = spec.nfsMount ? g_strdup(spec.nfsMount) : nullptr;
    volume->volumeClass = spec.volumeClass ? g_strdup(spec.volumeClass) : nullptr;
    volume->activationUri = spec.activationUri ? g_strdup(spec.activationUri) : nullptr;
    if (spec.drive != nullptr) {
      volume->drive = G_DRIVE(g_object_ref(spec.drive));
    }
    if (spec.mountUri != nullptr) {
      volume->mount = makeMount(spec.name, spec.mountUri, G_VOLUME(volume), spec.mountCanEject).release();
    }
    return luciole::GioPtr<GVolume>(G_VOLUME(volume));
  }

  void setVolumeMount(GVolume* volume, const char* uri) {
    auto* mock = reinterpret_cast<MockVolume*>(volume);
    g_clear_object(&mock->mount);
    if (uri != nullptr) {
      mock->mount = makeMount(mock->name, uri, volume).release();
    }
  }

  void collectLabels(const Node& node, std::vector<std::string>& out) {
    if (!node.visible()) {
      return;
    }
    if (const auto* label = dynamic_cast<const Label*>(&node); label && !label->text().empty()) {
      out.push_back(label->text());
    }
    for (const auto& child : node.children()) {
      collectLabels(*child, out);
    }
  }

  std::vector<std::string> sidebarLabels(luciole::PlacesSidebar& sidebar) {
    MetricsRenderer renderer;
    sidebar.measure(renderer, {.maxWidth = 220.0F, .maxHeight = 800.0F});
    sidebar.arrange(renderer, {0.0F, 0.0F, 220.0F, 800.0F});
    std::vector<std::string> labels;
    collectLabels(sidebar, labels);
    return labels;
  }

  struct Fixture {
    fs::path root;
    Fixture() {
      const luciole::GString path(g_dir_make_tmp("luciole-sidebar-XXXXXX", nullptr));
      TEST_CHECK(path != nullptr);
      root = path.get();
      fs::create_directories(root / "home");
      fs::create_directories(root / "config");
      fs::create_directories(root / "mnt-local");
      fs::create_directories(root / "mnt-loop");
      fs::create_directories(root / "mnt-iso");
      fs::create_directories(root / "mnt-sd");
      g_setenv("HOME", (root / "home").c_str(), TRUE);
      g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), TRUE);
      std::ofstream(root / "config/user-dirs.dirs")
          << "XDG_DESKTOP_DIR=\"$HOME\"\nXDG_DOCUMENTS_DIR=\"$HOME\"\nXDG_DOWNLOAD_DIR=\"$HOME\"\n"
             "XDG_MUSIC_DIR=\"$HOME\"\nXDG_PICTURES_DIR=\"$HOME\"\nXDG_VIDEOS_DIR=\"$HOME\"\n"
             "XDG_PUBLICSHARE_DIR=\"$HOME\"\nXDG_TEMPLATES_DIR=\"$HOME\"\n";
      g_reload_user_special_dirs_cache();
    }
    ~Fixture() {
      currentVolumes.clear();
      currentMounts.clear();
      fs::remove_all(root);
    }
  };

  void waitUntil(const std::function<bool()>& done) {
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto start = source.addPollFds(fds);
      const int timeout = source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
      source.dispatch(fds, start);
    }
  }

  void waitPins(const luciole::PinService& pins) {
    waitUntil([&] { return !pins.busy(); });
  }

  void testClassifyDrive() {
    using luciole::DriveKind;
    const auto check = [](std::string_view cls, std::string_view scheme, std::string_view fs, std::string_view dev,
                          bool removable, DriveKind expected) {
      TEST_CHECK(luciole::classifyDrive({cls, scheme, fs, dev, false, removable}) == expected);
    };
    // Fstab SSHFS/WebDAV volumes may report class "device" with no physical GDrive.
    check("device", {}, "fuse.sshfs", {}, false, DriveKind::Network);
    check("device", {}, "fuse.davfs2", {}, false, DriveKind::Network);
    // Phone/camera FUSE mounts classify as removable rather than remote.
    check("device", {}, "fuse.ifuse", {}, false, DriveKind::Removable);
    // Mount-only loop ISO images stay local even when GLib marks iso9660 ejectable.
    check({}, {}, "iso9660", "/dev/loop0", true, DriveKind::Local);
    check({}, "archive", {}, {}, false, DriveKind::Local);
    check({}, "mtp", {}, {}, false, DriveKind::Removable);
    check({}, "smb", {}, {}, true, DriveKind::Network);

    // Unmounted GVfs fstab SSHFS volumes expose no path or unix-device, only their display label.
    const std::vector<luciole::ConfiguredFilesystem> configured{
        {.path = "/mnt/ssh", .source = "user@host:/srv", .type = "fuse.sshfs", .label = "Work SSH"},
        {.path = "/mnt/dup-local", .source = "/dev/sdb1", .type = "ext4", .label = "Shared"},
        {.path = "/mnt/dup-remote", .source = "user@host:/shared", .type = "fuse.sshfs", .label = "Shared"},
    };
    const auto* byLabel = luciole::configuredFilesystemFor({}, {}, "Work SSH", configured);
    TEST_CHECK(byLabel != nullptr && byLabel->type == "fuse.sshfs");
    TEST_CHECK(
        luciole::classifyDrive({"device", {}, byLabel->type, byLabel->source, false, false}) == DriveKind::Network
    );
    // Conflicting fstab labels with different types must not guess.
    TEST_CHECK(luciole::configuredFilesystemFor({}, {}, "Shared", configured) == nullptr);
    // Non-empty device source must not fall back to label matching.
    TEST_CHECK(luciole::configuredFilesystemFor({}, "/dev/nvme0n1p2", "Work SSH", configured) == nullptr);
  }

  void testSectionsHotplugAndTree(const Fixture& fixture) {
    luciole::PinService pins(fixture.root / "state.toml", fixture.root / "bookmarks", {});
    pins.reload();
    waitPins(pins);

    const auto localUri = luciole::Location::fromPath((fixture.root / "mnt-local").native()).uri();
    const auto loopUri = luciole::Location::fromPath((fixture.root / "mnt-loop").native()).uri();
    const auto isoUri = luciole::Location::fromPath((fixture.root / "mnt-iso").native()).uri();
    const auto sdUri = luciole::Location::fromPath((fixture.root / "mnt-sd").native()).uri();
    const auto fixedDrive = makeDrive();
    const auto hotplugDrive = makeDrive();
    const auto mediaDrive = makeDrive({.mediaRemovable = true});

    const auto localDisk =
        makeVolume({.name = "Games", .id = "uuid-games", .volumeClass = "device", .drive = fixedDrive.get()});
    const auto loopIso = makeVolume({.name = "Arch ISO", .id = "uuid-iso", .volumeClass = "loop"});
    const auto loopDevMount = makeVolume(
        {.name = "Debian ISO",
         .id = "uuid-deb",
         .unixDevice = "/dev/loop1",
         .mountUri = loopUri.c_str(),
         .mountCanEject = true}
    );
    const auto archiveMount = makeMount("Backup.zip", "archive://file%253A%252F%252Fbackup.zip/");
    const auto usbDrive = makeVolume(
        {.name = "USB Drive",
         .id = "uuid-usb",
         .volumeClass = "device",
         .drive = hotplugDrive.get(),
         .mountUri = localUri.c_str()}
    );
    const auto sdCard =
        makeVolume({.name = "SD Card", .id = "uuid-sd", .volumeClass = "device", .drive = mediaDrive.get()});
    // GVfs MTP phone volumes often expose no GDrive, only an mtp:// root.
    const auto mtpPhone =
        makeVolume({.name = "Phone", .id = "uuid-phone", .volumeClass = "device", .mountUri = "mtp://usb-001/"});
    const auto networkUnmounted = makeVolume({.name = "TV", .id = "uuid-tv", .volumeClass = "network"});
    const auto networkNfs = makeVolume({.name = "Archive", .nfsMount = "nas.local:/export/archive"});
    const auto networkSmb = makeMount("Filmes", "smb://nas.local/filmes/");
    const auto networkActivation =
        makeVolume({.name = "Cloud", .id = "uuid-cloud", .activationUri = "davs://cloud.example/remote/"});

    // Fixed local drives and loop mounts only: Removable stays hidden; Remote still shows Network first.
    currentVolumes = {localDisk.get(), loopIso.get(), loopDevMount.get()};
    currentMounts = {archiveMount.get()};
    luciole::DriveMonitor drives;
    int driveChanges = 0;
    drives.setChangedCallback([&] { ++driveChanges; });
    drives.start();
    TEST_CHECK(driveChanges == 1);
    TEST_CHECK(std::ranges::all_of(drives.drives(), [](const luciole::Drive& d) {
      return d.kind == luciole::DriveKind::Local;
    }));

    luciole::PlacesSidebar sidebar(pins, drives);
    TEST_CHECK(
        sidebarLabels(sidebar)
        == std::vector<std::string>(
            {"Places", "Home", "Trash", "Drives", "File System", "Games", "Arch ISO", "Debian ISO", "Backup.zip",
             "Remote", "Network"}
        )
    );
    // Recent follows Home once shown, and is a row, not a pin.
    sidebar.setShowRecent(true);
    TEST_CHECK(
        sidebarLabels(sidebar)
        == std::vector<std::string>(
            {"Places", "Home", "Recent", "Trash", "Drives", "File System", "Games", "Arch ISO", "Debian ISO",
             "Backup.zip", "Remote", "Network"}
        )
    );
    TEST_CHECK(!pins.contains(luciole::Location::recent()));
    // Without a Home pin, Recent leads Places.
    const luciole::Bookmark homePin{
        .label = "Home", .location = luciole::Location::fromPath(g_get_home_dir()), .iconName = {}
    };
    const luciole::Bookmark otherPin{.label = "Other", .location = luciole::Location::fromPath("/srv"), .iconName = {}};
    TEST_CHECK(luciole::pinBeforeRecent({otherPin, homePin}) == 1);
    TEST_CHECK(!luciole::pinBeforeRecent({otherPin}));
    sidebar.setShowRecent(false);
    // With no network shares, hiding Network also omits the empty Remote heading.
    sidebar.setShowNetwork(false);
    TEST_CHECK(
        sidebarLabels(sidebar)
        == std::vector<std::string>(
            {"Places", "Home", "Trash", "Drives", "File System", "Games", "Arch ISO", "Debian ISO", "Backup.zip"}
        )
    );
    sidebar.setShowNetwork(true);

    // No-op drive-changed does not trigger a rebuild; changing drive capability moves the row into Removable.
    currentVolumes.push_back(usbDrive.get());
    g_signal_emit_by_name(activeMonitor, "volume-added", usbDrive.get());
    TEST_CHECK(driveChanges == 2);
    TEST_CHECK(drives.find("volume:uuid-usb")->kind == luciole::DriveKind::Local);

    g_signal_emit_by_name(activeMonitor, "drive-changed", hotplugDrive.get());
    TEST_CHECK(driveChanges == 2);

    reinterpret_cast<MockDrive*>(hotplugDrive.get())->canStop = TRUE;
    g_signal_emit_by_name(activeMonitor, "drive-changed", hotplugDrive.get());
    TEST_CHECK(driveChanges == 3);
    TEST_CHECK(drives.find("volume:uuid-usb")->kind == luciole::DriveKind::Removable);
    TEST_CHECK(drives.find("volume:uuid-usb")->removal == luciole::DriveRemoval::PowerOff);

    // Add media-removable SD card, MTP phone, and network shares to the same live monitor.
    currentVolumes = {localDisk.get(), loopIso.get(),          usbDrive.get(),   sdCard.get(),
                      mtpPhone.get(),  networkUnmounted.get(), networkNfs.get(), networkActivation.get()};
    currentMounts = {archiveMount.get(), networkSmb.get()};
    g_signal_emit_by_name(activeMonitor, "volume-added", sdCard.get());
    sidebar.invalidate();

    const std::vector<std::string> expectedDriveOrder{"File System", "Games",   "Arch ISO", "Backup.zip",
                                                      "USB Drive",   "SD Card", "Phone",    "TV",
                                                      "Archive",     "Cloud",   "Filmes"};
    const auto collectDriveOrder = [&] {
      std::vector<std::string> order;
      for (const auto& drive : drives.drives()) {
        order.push_back(drive.label);
      }
      return order;
    };
    TEST_CHECK(collectDriveOrder() == expectedDriveOrder);
    TEST_CHECK(drives.find("volume:uuid-usb")->glyph == "device-usb");
    TEST_CHECK(drives.find("volume:uuid-sd")->kind == luciole::DriveKind::Removable);
    TEST_CHECK(drives.find("volume:uuid-phone")->kind == luciole::DriveKind::Removable);
    TEST_CHECK(drives.find("volume:nas.local:/export/archive")->kind == luciole::DriveKind::Network);
    TEST_CHECK(drives.find("mount:smb://nas.local/filmes/")->glyph == "server");

    const std::vector<std::string> expectedSidebarLabels{
        "Places",    "Home",    "Trash", "Drives", "File System", "Games", "Arch ISO", "Backup.zip", "Removable",
        "USB Drive", "SD Card", "Phone", "Remote", "Network",     "TV",    "Archive",  "Cloud",      "Filmes"
    };
    TEST_CHECK(sidebarLabels(sidebar) == expectedSidebarLabels);

    // Mounting and unmounting entries keeps each row in its original slot even if GIO reorders its volume list.
    setVolumeMount(loopIso.get(), isoUri.c_str());
    setVolumeMount(sdCard.get(), sdUri.c_str());
    setVolumeMount(networkNfs.get(), "nfs://nas.local/export/archive/");
    std::ranges::reverse(currentVolumes);
    for (auto* volume : {loopIso.get(), sdCard.get(), networkNfs.get()}) {
      const luciole::GioPtr<GMount> mount(g_volume_get_mount(volume));
      g_signal_emit_by_name(activeMonitor, "mount-added", mount.get());
    }
    sidebar.invalidate();
    TEST_CHECK(drives.find("volume:uuid-iso")->root.has_value());
    TEST_CHECK(drives.find("volume:uuid-sd")->root.has_value());
    TEST_CHECK(drives.find("volume:nas.local:/export/archive")->root.has_value());
    TEST_CHECK(collectDriveOrder() == expectedDriveOrder);
    TEST_CHECK(sidebarLabels(sidebar) == expectedSidebarLabels);

    // Tree mode reflects the newly mounted entries in the same section order.
    sidebar.setTreeEnabled(true);
    TEST_CHECK(
        sidebarLabels(sidebar)
        == std::vector<std::string>(
            {"Folders", "Home", "Trash", "File System", "Arch ISO", "Backup.zip", "USB Drive", "SD Card", "Phone",
             "Network", "Archive", "Filmes"}
        )
    );
    sidebar.setTreeEnabled(false);

    std::vector<luciole::GioPtr<GMount>> removedMounts;
    for (auto* volume : {loopIso.get(), sdCard.get(), networkNfs.get()}) {
      removedMounts.emplace_back(g_volume_get_mount(volume));
      setVolumeMount(volume, nullptr);
    }
    std::ranges::reverse(currentVolumes);
    for (const auto& mount : removedMounts) {
      g_signal_emit_by_name(activeMonitor, "mount-removed", mount.get());
    }
    sidebar.invalidate();
    TEST_CHECK(!drives.find("volume:uuid-iso")->root.has_value());
    TEST_CHECK(!drives.find("volume:uuid-sd")->root.has_value());
    TEST_CHECK(!drives.find("volume:nas.local:/export/archive")->root.has_value());
    TEST_CHECK(collectDriveOrder() == expectedDriveOrder);
    TEST_CHECK(sidebarLabels(sidebar) == expectedSidebarLabels);

    // Show Drives hides Drives, Removable, and remote shares; Remote still shows Network while Show Network is on.
    sidebar.setShowDrives(false);
    TEST_CHECK(sidebarLabels(sidebar) == std::vector<std::string>({"Places", "Home", "Trash", "Remote", "Network"}));
    sidebar.setShowNetwork(false);
    TEST_CHECK(sidebarLabels(sidebar) == std::vector<std::string>({"Places", "Home", "Trash"}));

    // Show Network hides only Network; Drives, Removable, and remote shares stay visible under Show Drives.
    sidebar.setShowDrives(true);
    TEST_CHECK(
        sidebarLabels(sidebar)
        == std::vector<std::string>(
            {"Places", "Home", "Trash", "Drives", "File System", "Games", "Arch ISO", "Backup.zip", "Removable",
             "USB Drive", "SD Card", "Phone", "Remote", "TV", "Archive", "Cloud", "Filmes"}
        )
    );

    // Tree mode lists mounted drives and configured system automounts under Folders in the same group order.
    sidebar.setShowNetwork(true);
    sidebar.setTreeEnabled(true);
    TEST_CHECK(
        sidebarLabels(sidebar)
        == std::vector<std::string>(
            {"Folders", "Home", "Trash", "File System", "Backup.zip", "USB Drive", "Phone", "Network", "Filmes"}
        )
    );
    sidebar.setShowDrives(false);
    TEST_CHECK(sidebarLabels(sidebar) == std::vector<std::string>({"Folders", "Home", "Trash", "Network"}));
    sidebar.setShowNetwork(false);
    TEST_CHECK(sidebarLabels(sidebar) == std::vector<std::string>({"Folders", "Home", "Trash"}));
    sidebar.setShowDrives(true);
    TEST_CHECK(
        sidebarLabels(sidebar)
        == std::vector<std::string>(
            {"Folders", "Home", "Trash", "File System", "Backup.zip", "USB Drive", "Phone", "Filmes"}
        )
    );

    // Same-name fallback volumes interleaved around another volume stay distinct and keep order and cached usage across
    // reversed refreshes.
    const auto dupUnmounted = makeVolume({.name = "Backup", .volumeClass = "device", .drive = fixedDrive.get()});
    const auto middleVolume =
        makeVolume({.name = "Media", .id = "uuid-media", .volumeClass = "device", .drive = fixedDrive.get()});
    auto dupMounted =
        makeVolume({.name = "Backup", .volumeClass = "device", .drive = fixedDrive.get(), .mountUri = isoUri.c_str()});
    currentVolumes = {dupUnmounted.get(), middleVolume.get(), dupMounted.get()};
    currentMounts.clear();
    g_signal_emit_by_name(activeMonitor, "volume-added", dupUnmounted.get());
    const auto collectLocalOrder = [&] {
      std::vector<std::string> order;
      for (const auto& drive : drives.drives()) {
        if (drive.kind == luciole::DriveKind::Local) {
          order.push_back(drive.label);
        }
      }
      return order;
    };
    const std::vector<std::string> expectedLocalOrder{"File System", "Backup", "Media", "Backup"};
    TEST_CHECK(collectLocalOrder() == expectedLocalOrder);
    TEST_CHECK(drives.drives()[1].key != drives.drives()[3].key);
    TEST_CHECK(!drives.drives()[1].root.has_value() && drives.drives()[3].root.has_value());
    const std::string dupMountedKey = drives.drives()[3].key;
    drives.refreshUsage();
    waitUntil([&] {
      const auto* rootDrive = drives.find("root");
      const auto* dupDrive = drives.find(dupMountedKey);
      return rootDrive && rootDrive->usage.has_value() && dupDrive && dupDrive->usage.has_value();
    });
    const auto savedUsage = drives.find(dupMountedKey)->usage;
    const auto orderBeforeReverse = collectDriveOrder();
    const int changesBeforeNoOp = driveChanges;
    std::ranges::reverse(currentVolumes);
    g_signal_emit_by_name(activeMonitor, "drive-changed", fixedDrive.get());
    TEST_CHECK(driveChanges == changesBeforeNoOp);
    TEST_CHECK(collectDriveOrder() == orderBeforeReverse);
    TEST_CHECK(collectLocalOrder() == expectedLocalOrder);
    TEST_CHECK(!drives.drives()[1].root.has_value() && drives.drives()[3].root.has_value());
    TEST_CHECK(drives.find(dupMountedKey)->usage == savedUsage);

    // Replacing a removed identifier-less volume allocates a fresh fallback key rather than reusing the old one.
    currentVolumes = {dupUnmounted.get(), middleVolume.get()};
    g_signal_emit_by_name(activeMonitor, "volume-removed", dupMounted.get());
    dupMounted.reset();
    TEST_CHECK(drives.find(dupMountedKey) == nullptr);
    const auto dupReplacement =
        makeVolume({.name = "Backup", .volumeClass = "device", .drive = fixedDrive.get(), .mountUri = sdUri.c_str()});
    currentVolumes = {dupUnmounted.get(), middleVolume.get(), dupReplacement.get()};
    g_signal_emit_by_name(activeMonitor, "volume-added", dupReplacement.get());
    TEST_CHECK(drives.find(dupMountedKey) == nullptr);
    TEST_CHECK(drives.drives()[3].key != dupMountedKey && drives.drives()[3].key != drives.drives()[1].key);

    // Volumes sharing a UUID (two loop-mounted copies of one ISO) stay distinct rows keyed by their block device.
    const auto isoCopyA =
        makeVolume({.name = "Copy", .id = "uuid-same", .unixDevice = "/dev/loop2", .volumeClass = "loop"});
    const auto isoCopyB =
        makeVolume({.name = "Copy", .id = "uuid-same", .unixDevice = "/dev/loop3", .volumeClass = "loop"});
    currentVolumes = {isoCopyA.get(), isoCopyB.get()};
    g_signal_emit_by_name(activeMonitor, "volume-added", isoCopyA.get());
    TEST_CHECK(drives.find("volume:/dev/loop2") != nullptr && drives.find("volume:/dev/loop3") != nullptr);
  }
} // namespace

int main() {
  g_io_extension_point_implement(
      G_NATIVE_VOLUME_MONITOR_EXTENSION_POINT_NAME, mock_monitor_get_type(), "luciole-sidebar-test", 10000
  );
  g_setenv("GIO_USE_VOLUME_MONITOR", "luciole-sidebar-test", TRUE);
  try {
    testClassifyDrive();
    const Fixture fixture;
    testSectionsHotplugAndTree(fixture);
    std::println("PlacesSidebar sections tests passed");
  } catch (const std::exception& error) {
    std::println(stderr, "PlacesSidebar sections test failed: {}", error.what());
    return 1;
  }
  return 0;
}
