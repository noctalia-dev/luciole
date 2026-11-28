#include "event_loop/glib_poll_source.h"
#include "fs/drive_monitor.h"
#include "fs/gio_interop.h"
#include "fs/location.h"
#include "test_check.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gio/gnativevolumemonitor.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  struct UnmountReply {
    bool succeed = false;
    bool emitBusyProcess = false;
    GIOErrorEnum errorCode = G_IO_ERROR_FAILED;
    std::string errorMessage;
  };

  struct MockMount {
    GObject parent;
    char* name;
    char* uri;
    GVolume* volume;
  };
  struct MockMountClass {
    GObjectClass parent;
  };
  struct MockVolume {
    GObject parent;
    char* name;
    char* id;
    char* unixDevice;
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
  UnmountReply nextUnmountReply;

  void mountInterface(GMountIface* iface);
  void volumeInterface(GVolumeIface* iface);

  // GObject requires these generated C entry-point names.
  // NOLINTBEGIN(readability-identifier-naming)
  G_DEFINE_TYPE_WITH_CODE(MockMount, mock_mount, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_MOUNT, mountInterface))
  G_DEFINE_TYPE_WITH_CODE(MockVolume, mock_volume, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_VOLUME, volumeInterface))
  G_DEFINE_TYPE(MockMonitor, mock_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

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

  void mountInterface(GMountIface* iface) {
    iface->get_root = +[](GMount* m) { return g_file_new_for_uri(reinterpret_cast<MockMount*>(m)->uri); };
    iface->get_name = +[](GMount* m) { return g_strdup(reinterpret_cast<MockMount*>(m)->name); };
    iface->get_volume = +[](GMount* m) -> GVolume* {
      auto* volume = reinterpret_cast<MockMount*>(m)->volume;
      return volume ? G_VOLUME(g_object_ref(volume)) : nullptr;
    };
    iface->get_drive = +[](GMount*) -> GDrive* { return nullptr; };
    iface->can_unmount = +[](GMount*) -> gboolean { return TRUE; };
    iface->can_eject = +[](GMount*) -> gboolean { return FALSE; };
    iface->unmount_with_operation = +[](GMount* source, GMountUnmountFlags, GMountOperation* operation,
                                        GCancellable* cancellable, GAsyncReadyCallback callback, gpointer data) {
      auto* task = g_task_new(source, cancellable, callback, data);
      if (nextUnmountReply.emitBusyProcess && operation != nullptr) {
        GArray* processes = g_array_new(FALSE, FALSE, sizeof(GPid));
        const GPid pid = ::getpid();
        g_array_append_val(processes, pid);
        const char* choices[] = {"Unmount Anyway", "Cancel", nullptr};
        g_signal_emit_by_name(operation, "show-processes", "Volume is busy", processes, choices);
        g_array_unref(processes);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY, "Target is busy");
      } else if (nextUnmountReply.succeed) {
        g_task_return_boolean(task, TRUE);
      } else {
        g_task_return_new_error(
            task, G_IO_ERROR, nextUnmountReply.errorCode, "%s", nextUnmountReply.errorMessage.c_str()
        );
      }
      g_object_unref(task);
    };
    iface->unmount_with_operation_finish =
        +[](GMount*, GAsyncResult* result, GError** error) { return g_task_propagate_boolean(G_TASK(result), error); };
  }

  void volumeInterface(GVolumeIface* iface) {
    iface->get_name = +[](GVolume* v) { return g_strdup(reinterpret_cast<MockVolume*>(v)->name); };
    iface->get_identifier = +[](GVolume* source, const char* kind) -> char* {
      const auto* volume = reinterpret_cast<MockVolume*>(source);
      const std::string_view key(kind);
      if (key == G_VOLUME_IDENTIFIER_KIND_UUID) {
        return volume->id ? g_strdup(volume->id) : nullptr;
      }
      if (key == G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) {
        return volume->unixDevice ? g_strdup(volume->unixDevice) : nullptr;
      }
      return nullptr;
    };
    iface->get_drive = +[](GVolume*) -> GDrive* { return nullptr; };
    iface->get_mount = +[](GVolume* source) -> GMount* {
      auto* mount = reinterpret_cast<MockVolume*>(source)->mount;
      return mount ? G_MOUNT(g_object_ref(mount)) : nullptr;
    };
  }

  luciole::GioPtr<GMount> makeMount(const char* name, const char* uri, GVolume* volume = nullptr) {
    auto* mount = reinterpret_cast<MockMount*>(g_object_new(mock_mount_get_type(), nullptr));
    mount->name = g_strdup(name);
    mount->uri = g_strdup(uri);
    mount->volume = volume;
    return luciole::GioPtr<GMount>(G_MOUNT(mount));
  }

  luciole::GioPtr<GVolume> makeVolume(const char* name, const char* id, const char* unixDevice, const char* uri) {
    auto* volume = reinterpret_cast<MockVolume*>(g_object_new(mock_volume_get_type(), nullptr));
    volume->name = g_strdup(name);
    volume->id = g_strdup(id);
    volume->unixDevice = g_strdup(unixDevice);
    volume->mount = makeMount(name, uri, G_VOLUME(volume)).release();
    return luciole::GioPtr<GVolume>(G_VOLUME(volume));
  }

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

  std::string expectedTrustedUmount() {
    static constexpr std::array<const char*, 5> kTrustedPaths{
        "/usr/bin/umount", "/bin/umount", "/usr/sbin/umount", "/sbin/umount", "/run/wrappers/bin/umount",
    };
    for (const auto* path : kTrustedPaths) {
      struct stat info{};
      if (::stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0 && ::access(path, X_OK) == 0) {
        return path;
      }
    }
    return {};
  }

  struct MockUdisks {
    struct Entry {
      std::string objectPath;
      std::vector<std::string> mountPoints;
    };
    std::vector<Entry> objects;
    std::string unmountErrorName;
    std::string unmountErrorMessage;
    bool holdUnmount = false;
    int unmountCalls = 0;
    bool sawNoUserInteractionFalse = false;
    bool sawInteractiveFlag = false;
    std::string lastObjectPath;
    GDBusMethodInvocation* heldInvocation = nullptr;

    luciole::GioPtr<GDBusConnection> connection;
    guint ownerId = 0;
    std::vector<guint> registrations;
    bool nameAcquired = false;

    void start(const char* address) {
      GError* error = nullptr;
      connection.reset(g_dbus_connection_new_for_address_sync(
          address,
          static_cast<GDBusConnectionFlags>(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
          ),
          nullptr, nullptr, &error
      ));
      TEST_CHECK(connection && error == nullptr);
      ownerId = g_bus_own_name_on_connection(
          connection.get(), "org.freedesktop.UDisks2", G_BUS_NAME_OWNER_FLAGS_NONE,
          +[](GDBusConnection*, const char*, gpointer data) { static_cast<MockUdisks*>(data)->nameAcquired = true; },
          nullptr, this, nullptr
      );
      static constexpr auto kManagerXml = R"xml(
        <node>
          <interface name="org.freedesktop.DBus.ObjectManager">
            <method name="GetManagedObjects">
              <arg name="objects" type="a{oa{sa{sv}}}" direction="out"/>
            </method>
          </interface>
        </node>
      )xml";
      static constexpr auto kFsXml = R"xml(
        <node>
          <interface name="org.freedesktop.UDisks2.Filesystem">
            <method name="Unmount">
              <arg name="options" type="a{sv}" direction="in"/>
            </method>
          </interface>
        </node>
      )xml";
      GDBusNodeInfo* managerNode = g_dbus_node_info_new_for_xml(kManagerXml, &error);
      TEST_CHECK(managerNode && error == nullptr);
      GDBusNodeInfo* fsNode = g_dbus_node_info_new_for_xml(kFsXml, &error);
      TEST_CHECK(fsNode && error == nullptr);

      static const GDBusInterfaceVTable managerVtable = {
          +[](GDBusConnection*, const char*, const char*, const char*, const char* method, GVariant*,
              GDBusMethodInvocation* invocation, gpointer data) {
            TEST_CHECK(std::string_view(method) == "GetManagedObjects");
            auto* self = static_cast<MockUdisks*>(data);
            GVariantBuilder objectsBuilder{};
            g_variant_builder_init(&objectsBuilder, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
            for (const auto& entry : self->objects) {
              GVariantBuilder ifaces{};
              g_variant_builder_init(&ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
              GVariantBuilder props{};
              g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
              GVariantBuilder mps{};
              g_variant_builder_init(&mps, G_VARIANT_TYPE("aay"));
              for (const auto& mp : entry.mountPoints) {
                GVariantBuilder bytes{};
                g_variant_builder_init(&bytes, G_VARIANT_TYPE("ay"));
                for (const char ch : mp) {
                  g_variant_builder_add(&bytes, "y", static_cast<guchar>(ch));
                }
                g_variant_builder_add(&bytes, "y", static_cast<guchar>(0));
                g_variant_builder_add(&mps, "ay", &bytes);
              }
              g_variant_builder_add(&props, "{sv}", "MountPoints", g_variant_builder_end(&mps));
              g_variant_builder_add(&ifaces, "{sa{sv}}", "org.freedesktop.UDisks2.Filesystem", &props);
              g_variant_builder_add(&objectsBuilder, "{oa{sa{sv}}}", entry.objectPath.c_str(), &ifaces);
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{oa{sa{sv}}})", &objectsBuilder));
          },
          nullptr,
          nullptr,
          {}
      };
      static const GDBusInterfaceVTable fsVtable = {
          +[](GDBusConnection*, const char*, const char* objectPath, const char*, const char* method,
              GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
            TEST_CHECK(std::string_view(method) == "Unmount");
            auto* self = static_cast<MockUdisks*>(data);
            ++self->unmountCalls;
            self->lastObjectPath = objectPath;
            const auto flags = g_dbus_message_get_flags(g_dbus_method_invocation_get_message(invocation));
            self->sawInteractiveFlag = (flags & G_DBUS_MESSAGE_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION) != 0;
            GVariant* options = g_variant_get_child_value(parameters, 0);
            gboolean noInteraction = TRUE;
            self->sawNoUserInteractionFalse =
                g_variant_lookup(options, "auth.no_user_interaction", "b", &noInteraction) != FALSE
                && noInteraction == FALSE;
            g_variant_unref(options);
            if (self->holdUnmount) {
              self->heldInvocation = invocation;
              return;
            }
            if (self->unmountErrorName.empty()) {
              g_dbus_method_invocation_return_value(invocation, nullptr);
            } else {
              g_dbus_method_invocation_return_dbus_error(
                  invocation, self->unmountErrorName.c_str(), self->unmountErrorMessage.c_str()
              );
            }
          },
          nullptr,
          nullptr,
          {}
      };
      registrations.push_back(g_dbus_connection_register_object(
          connection.get(), "/org/freedesktop/UDisks2", managerNode->interfaces[0], &managerVtable, this, nullptr,
          &error
      ));
      TEST_CHECK(error == nullptr);
      for (const auto* path :
           {"/org/freedesktop/UDisks2/block_devices/sdb2", "/org/freedesktop/UDisks2/block_devices/sdb3",
            "/org/freedesktop/UDisks2/block_devices/sdb4"}) {
        registrations.push_back(g_dbus_connection_register_object(
            connection.get(), path, fsNode->interfaces[0], &fsVtable, this, nullptr, &error
        ));
        TEST_CHECK(error == nullptr);
      }
      g_dbus_node_info_unref(managerNode);
      g_dbus_node_info_unref(fsNode);
      waitUntil([&] { return nameAcquired; });
    }

    void stop() {
      if (heldInvocation != nullptr) {
        g_dbus_method_invocation_return_value(std::exchange(heldInvocation, nullptr), nullptr);
      }
      if (connection) {
        for (const auto id : registrations) {
          g_dbus_connection_unregister_object(connection.get(), id);
        }
        registrations.clear();
        if (ownerId != 0) {
          g_bus_unown_name(std::exchange(ownerId, 0));
        }
        (void)g_dbus_connection_close_sync(connection.get(), nullptr, nullptr);
        connection.reset();
      }
    }

    ~MockUdisks() { stop(); }
  };

  void writeScript(const fs::path& file, std::string_view content) {
    std::ofstream out(file);
    out << content;
    out.close();
    fs::permissions(file, fs::perms::owner_all);
  }

  luciole::DriveRemoveResult runRemove(luciole::DriveMonitor& drives, const std::string& key) {
    std::optional<luciole::DriveRemoveResult> outcome;
    drives.remove(key, [&](luciole::DriveRemoveResult result) { outcome = std::move(result); });
    waitUntil([&] { return outcome.has_value(); });
    return std::move(*outcome);
  }
} // namespace

int main() {
  const luciole::GioPtr<GTestDBus> bus(g_test_dbus_new(G_TEST_DBUS_NONE));
  g_test_dbus_up(bus.get());
  const char* address = g_test_dbus_get_bus_address(bus.get());
  TEST_CHECK(address != nullptr);
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", address, TRUE);
  g_io_extension_point_implement(
      G_NATIVE_VOLUME_MONITOR_EXTENSION_POINT_NAME, mock_monitor_get_type(), "luciole-unmount-test", 10000
  );
  g_setenv("GIO_USE_VOLUME_MONITOR", "luciole-unmount-test", TRUE);

  const luciole::GString tmpDir(g_dir_make_tmp("luciole-unmount-XXXXXX", nullptr));
  TEST_CHECK(tmpDir != nullptr);
  const fs::path root(tmpDir.get());
  const fs::path binDir = root / "bin";
  const fs::path btrfsMount = root / "CachyOS-Recovered";
  const fs::path multiMount = root / "Multi-Subvol";
  const fs::path fstabMount = root / "Fstab-Share";
  const fs::path blockMount = root / "Block-Disk";
  const fs::path trickyMount = root / "Target is Busy";
  fs::create_directories(binDir);
  fs::create_directories(btrfsMount);
  fs::create_directories(multiMount);
  fs::create_directories(fstabMount);
  fs::create_directories(blockMount);
  fs::create_directories(trickyMount);

  const fs::path pkexecLog = root / "pkexec.log";
  const char* inheritedPath = g_getenv("PATH");
  const std::string savedPath = inheritedPath ? inheritedPath : "";
  g_setenv("PATH", (binDir.native() + ":" + savedPath).c_str(), TRUE);

  // Place a shadowing umount on PATH to prove pkexec never receives a session-PATH wrapper.
  writeScript(binDir / "umount", "#!/bin/sh\necho 'shadowed umount invoked' >&2\nexit 99\n");

  auto setPkexec = [&](int exitCode, std::string_view stderrText = {}) {
    writeScript(
        binDir / "pkexec",
        std::format(
            "#!/bin/sh\nprintf '%s|%s\\n' \"$LC_ALL\" \"$*\" >> '{}'\nprintf '%s' '{}' >&2\nexit {}\n",
            pkexecLog.native(), stderrText, exitCode
        )
    );
  };
  setPkexec(0);

  const std::string trustedUmount = expectedTrustedUmount();
  const auto btrfsUri = luciole::Location::fromPath(btrfsMount.native()).uri();
  const auto multiUri = luciole::Location::fromPath(multiMount.native()).uri();
  const auto fstabUri = luciole::Location::fromPath(fstabMount.native()).uri();
  const auto blockUri = luciole::Location::fromPath(blockMount.native()).uri();
  const auto trickyUri = luciole::Location::fromPath(trickyMount.native()).uri();

  const auto standaloneBtrfs = makeMount("CachyOS-Recovered", btrfsUri.c_str());
  const auto standaloneMulti = makeMount("Multi-Subvol", multiUri.c_str());
  const auto standaloneFstab = makeMount("Fstab-Share", fstabUri.c_str());
  const auto standaloneTricky = makeMount("Target is Busy", trickyUri.c_str());
  const auto remoteMount = makeMount("Remote", "smb://nas.local/share/");
  const auto blockVolume = makeVolume("Block-Disk", "uuid-block", "/dev/sda2", blockUri.c_str());

  currentMounts = {
      standaloneBtrfs.get(), standaloneMulti.get(), standaloneFstab.get(), standaloneTricky.get(), remoteMount.get()
  };
  currentVolumes = {blockVolume.get()};

  {
    MockUdisks udisks;
    udisks.objects = {
        {.objectPath = "/org/freedesktop/UDisks2/block_devices/sdb2", .mountPoints = {btrfsMount.native()}},
        {.objectPath = "/org/freedesktop/UDisks2/block_devices/sdb3",
         .mountPoints = {multiMount.native(), (root / "Other-Subvol").native()}},
        {.objectPath = "/org/freedesktop/UDisks2/block_devices/sdb4", .mountPoints = {trickyMount.native()}},
    };
    udisks.start(address);

    luciole::DriveMonitor drives;
    drives.start();

    const std::string btrfsKey = "mount:" + btrfsUri;
    const std::string multiKey = "mount:" + multiUri;
    const std::string fstabKey = "mount:" + fstabUri;
    const std::string trickyKey = "mount:" + trickyUri;
    const std::string remoteKey = "mount:smb://nas.local/share/";
    const std::string blockKey = "volume:/dev/sda2";

    // 1. Single-mountpoint UDisks2 filesystem (e.g. btrfs with subvol=/@) escalates through UDisks2 D-Bus.
    nextUnmountReply = {
        .succeed = false,
        .errorCode = G_IO_ERROR_FAILED,
        .errorMessage = "umount: " + btrfsMount.native() + ": must be superuser to unmount."
    };
    {
      const auto result = runRemove(drives, btrfsKey);
      TEST_CHECK(result.error.empty() && !result.busy && !result.cancelled);
      TEST_CHECK(udisks.unmountCalls == 1);
      TEST_CHECK(udisks.lastObjectPath == "/org/freedesktop/UDisks2/block_devices/sdb2");
      TEST_CHECK(udisks.sawInteractiveFlag && udisks.sawNoUserInteractionFalse);
      TEST_CHECK(!fs::exists(pkexecLog));
    }

    // 2. Dismissing UDisks2's Polkit prompt cancels cleanly without falling back to pkexec.
    udisks.unmountErrorName = "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed";
    udisks.unmountErrorMessage = "Authentication dialog was dismissed";
    {
      const auto result = runRemove(drives, btrfsKey);
      TEST_CHECK(result.cancelled && result.error.empty() && !result.busy);
      TEST_CHECK(udisks.unmountCalls == 2);
      TEST_CHECK(!fs::exists(pkexecLog));
    }

    // 3. UDisks2 DeviceBusy maps to busy without falling back to pkexec.
    udisks.unmountErrorName = "org.freedesktop.UDisks2.Error.DeviceBusy";
    udisks.unmountErrorMessage = "Target is busy";
    {
      const auto result = runRemove(drives, btrfsKey);
      TEST_CHECK(result.busy && !result.cancelled);
      TEST_CHECK(udisks.unmountCalls == 3);
      TEST_CHECK(!fs::exists(pkexecLog));
    }

    // 4. UDisks2 NotAuthorized and vanished-object UnknownObject finish without retrying pkexec by stale path.
    udisks.unmountErrorName = "org.freedesktop.UDisks2.Error.NotAuthorized";
    udisks.unmountErrorMessage = "Not authorized to perform operation";
    {
      const auto result = runRemove(drives, btrfsKey);
      TEST_CHECK(!result.busy && !result.cancelled && result.error == "Not authorized to perform operation");
      TEST_CHECK(udisks.unmountCalls == 4);
      TEST_CHECK(!fs::exists(pkexecLog));
    }
    udisks.unmountErrorName = "org.freedesktop.DBus.Error.UnknownObject";
    udisks.unmountErrorMessage = "No such object";
    {
      const auto result = runRemove(drives, btrfsKey);
      TEST_CHECK(!result.busy && !result.cancelled && result.error == "No such object");
      TEST_CHECK(udisks.unmountCalls == 5);
      TEST_CHECK(!fs::exists(pkexecLog));
    }
    udisks.unmountErrorName.clear();
    udisks.unmountErrorMessage.clear();

    // 5. A mount named "Target is Busy" is classified by the error after the mount path, not the folder name.
    nextUnmountReply = {
        .succeed = false,
        .errorCode = G_IO_ERROR_FAILED,
        .errorMessage = "umount: " + trickyMount.native() + ": must be superuser to unmount."
    };
    {
      const auto result = runRemove(drives, trickyKey);
      TEST_CHECK(result.error.empty() && !result.busy && !result.cancelled);
      TEST_CHECK(udisks.unmountCalls == 6);
      TEST_CHECK(udisks.lastObjectPath == "/org/freedesktop/UDisks2/block_devices/sdb4");
    }

    // 6. Multi-mountpoint UDisks2 device skips UDisks2 and uses pkexec with a trusted absolute umount and LC_ALL=C.
    nextUnmountReply = {
        .succeed = false,
        .errorCode = G_IO_ERROR_FAILED,
        .errorMessage = "umount: " + multiMount.native() + ": must be superuser to unmount."
    };
    if (!trustedUmount.empty()) {
      const auto result = runRemove(drives, multiKey);
      TEST_CHECK(result.error.empty() && !result.busy && !result.cancelled);
      TEST_CHECK(udisks.unmountCalls == 6);
      std::string logged;
      std::getline(std::ifstream(pkexecLog), logged);
      TEST_CHECK(logged == "C|--disable-internal-agent " + trustedUmount + " -- " + multiMount.native());
      fs::remove(pkexecLog);

      // 7. Non-UDisks2 mount uses pkexec: dismissal cancels, helper exit 126 surfaces error, busy sets busy.
      nextUnmountReply = {
          .succeed = false,
          .errorCode = G_IO_ERROR_FAILED,
          .errorMessage = "umount: " + fstabMount.native() + ": must be superuser to unmount."
      };
      setPkexec(126, "Error executing command as another user: Request dismissed\n");
      {
        const auto dismissed = runRemove(drives, fstabKey);
        TEST_CHECK(dismissed.cancelled && dismissed.error.empty() && !dismissed.busy);
      }
      setPkexec(126, "umount: " + fstabMount.native() + ": failed to execute /sbin/umount.nfs\n");
      {
        const auto helperFailed = runRemove(drives, fstabKey);
        TEST_CHECK(
            !helperFailed.cancelled
            && !helperFailed.busy
            && helperFailed.error == "umount: " + fstabMount.native() + ": failed to execute /sbin/umount.nfs"
        );
      }
      setPkexec(32, "umount: " + fstabMount.native() + ": target is busy.\n");
      {
        const auto busy = runRemove(drives, fstabKey);
        TEST_CHECK(busy.busy && !busy.cancelled);
      }
      setPkexec(127, "Error executing command as another user: Not authorized\n");
      {
        const auto denied = runRemove(drives, fstabKey);
        TEST_CHECK(
            !denied.busy
            && !denied.cancelled
            && denied.error == "Error executing command as another user: Not authorized"
        );
      }
      fs::remove(pkexecLog);
    } else {
      // Without a root-owned system umount binary (e.g. an empty build sandbox), pkexec is never invoked.
      const auto result = runRemove(drives, multiKey);
      TEST_CHECK(!result.error.empty() && !fs::exists(pkexecLog));
    }

    // 8. GIO busy, FAILED_HANDLED, non-native mounts, and already-denied block volumes never escalate.
    nextUnmountReply = {.succeed = false, .emitBusyProcess = true, .errorMessage = {}};
    {
      const auto result = runRemove(drives, fstabKey);
      TEST_CHECK(result.busy && !result.blockers.empty() && !fs::exists(pkexecLog));
    }
    nextUnmountReply = {.succeed = false, .errorCode = G_IO_ERROR_FAILED_HANDLED, .errorMessage = "Dismissed"};
    {
      const auto result = runRemove(drives, fstabKey);
      TEST_CHECK(result.cancelled && result.error.empty() && !fs::exists(pkexecLog));
    }
    nextUnmountReply = {.succeed = false, .errorCode = G_IO_ERROR_PERMISSION_DENIED, .errorMessage = "Denied"};
    {
      const auto remoteResult = runRemove(drives, remoteKey);
      TEST_CHECK(!remoteResult.cancelled && remoteResult.error == "Denied" && !fs::exists(pkexecLog));
      const auto blockResult = runRemove(drives, blockKey);
      TEST_CHECK(!blockResult.cancelled && blockResult.error == "Denied" && !fs::exists(pkexecLog));
      TEST_CHECK(udisks.unmountCalls == 6);
    }

    // 9. Destroying DriveMonitor while an elevated unmount is waiting on Polkit drops the callback safely.
    udisks.holdUnmount = true;
    nextUnmountReply = {
        .succeed = false,
        .errorCode = G_IO_ERROR_FAILED,
        .errorMessage = "umount: " + btrfsMount.native() + ": must be superuser to unmount."
    };
    {
      auto transient = std::make_unique<luciole::DriveMonitor>();
      transient->start();
      bool called = false;
      auto sentinel = std::make_shared<int>(0);
      const std::weak_ptr<int> weakSentinel = sentinel;
      transient->remove(btrfsKey, [&called, held = std::move(sentinel)](const luciole::DriveRemoveResult&) {
        (void)held;
        called = true;
      });
      waitUntil([&] { return udisks.heldInvocation != nullptr; });
      transient.reset();
      g_dbus_method_invocation_return_value(std::exchange(udisks.heldInvocation, nullptr), nullptr);
      waitUntil([&] { return weakSentinel.expired(); });
      TEST_CHECK(!called);
    }

    udisks.stop();
  }

  // Close the singleton system-bus connection before shutting down GTestDBus.
  if (const luciole::GioPtr<GDBusConnection> systemBus(g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr));
      systemBus) {
    (void)g_dbus_connection_close_sync(systemBus.get(), nullptr, nullptr);
  }
  g_test_dbus_down(bus.get());

  currentVolumes.clear();
  currentMounts.clear();
  g_setenv("PATH", savedPath.c_str(), TRUE);
  fs::remove_all(root);
  std::println("Drive privileged unmount tests passed");
  return 0;
}
