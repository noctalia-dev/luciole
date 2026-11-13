#include "fs/gio_interop.h"
#include "platform/extraction_launcher.h"
#include "test_check.h"

#include <filesystem>
#include <print>
#include <stdexcept>

namespace {
  void until(const std::function<bool()>& done) {
    const auto deadline = g_get_monotonic_time() + 8000000;
    while (!done()) {
      (void)g_main_context_iteration(nullptr, false);
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
  }
  void run() {
    using luciole::ExtractionLauncher;
    using luciole::ExtractionOffer;
    namespace fs = std::filesystem;
    const luciole::GioPtr<GTestDBus> bus(g_test_dbus_new(G_TEST_DBUS_NONE));
    g_test_dbus_up(bus.get());
    luciole::GioPtr<GDBusConnection> connection(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr));
    TEST_CHECK(connection);
    const ExtractionOffer offer{g_dbus_connection_get_unique_name(connection.get()), "/source"};
    const luciole::GString tmp(g_dir_make_tmp("luciole-extraction-test-XXXXXX", nullptr));
    TEST_CHECK(tmp);
    const fs::path root(tmp.get());
    struct Cleanup {
      fs::path path;
      ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    fs::create_directory(root / "space café");
    const auto destination = luciole::Location::fromPath((root / "space café").native());
    const char* xml = "<node><interface name='org.kde.ark.DndExtract'><method name='extractSelectedFilesTo'>"
                      "<arg type='s' direction='in'/></method></interface></node>";
    auto* node = g_dbus_node_info_new_for_xml(xml, nullptr);
    TEST_CHECK(node);
    struct Received {
      std::string path;
      unsigned count = 0;
      bool hold = false, reject = false;
      luciole::GioPtr<GDBusMethodInvocation> pending;
    } received;
    static const GDBusInterfaceVTable handler{
        .method_call =
            [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* args,
               GDBusMethodInvocation* invocation, gpointer data) {
              auto& received = *static_cast<Received*>(data);
              const char* path = nullptr;
              g_variant_get(args, "(&s)", &path);
              received.path = path;
              ++received.count;
              if (received.hold) {
                received.pending.reset(static_cast<GDBusMethodInvocation*>(g_object_ref(invocation)));
              } else if (received.reject) {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.example.ExtractionFailed", "Source refused."
                );
              } else {
                // Like the actual source protocol, acknowledge before any extraction effects.
                g_dbus_method_invocation_return_value(invocation, nullptr);
              }
            },
        .get_property = nullptr,
        .set_property = nullptr,
        .padding = {}
    };
    const auto registration = g_dbus_connection_register_object(
        connection.get(), offer.path.c_str(), node->interfaces[0], &handler, &received, nullptr, nullptr
    );
    g_dbus_node_info_unref(node);
    TEST_CHECK(registration);
    ExtractionLauncher launcher;
    const auto request = [&](ExtractionOffer source, luciole::Location target) {
      bool done = false;
      std::string error;
      TEST_CHECK(launcher.request(std::move(source), std::move(target), [&](auto message) {
        error = std::move(message);
        done = true;
      }));
      TEST_CHECK(launcher.busy() && !done);
      TEST_CHECK(!launcher.request(offer, destination, [](const auto&) {}));
      until([&] { return done; });
      TEST_CHECK(!launcher.busy());
      return error;
    };
    TEST_CHECK(
        request(offer, destination).empty() && received.path == *destination.nativePath() && received.count == 1
    );
    TEST_CHECK(fs::is_empty(root / "space café"));
    for (const auto& invalid :
         {ExtractionOffer{"org.example.Name", "/source"}, ExtractionOffer{":1.25", "bad"},
          ExtractionOffer{std::string(4097, 'a'), "/source"}, ExtractionOffer{std::string("a\0b", 3), "/source"}}) {
      TEST_CHECK(!ExtractionLauncher::valid(invalid) && !request(invalid, destination).empty());
    }
    TEST_CHECK(!request(offer, luciole::Location::fromPath((root / "missing").native())).empty());
    TEST_CHECK(!request(offer, luciole::Location::fromUri("sftp://example.invalid/folder")).empty());
    TEST_CHECK(received.count == 1);
    auto absent = offer;
    absent.path = "/absent";
    TEST_CHECK(!request(absent, destination).empty());
    absent = offer;
    absent.service = ":999999.999999";
    TEST_CHECK(!request(absent, destination).empty());
    received.reject = true;
    TEST_CHECK(!request(offer, destination).empty());
    received.reject = false;
    received.hold = true;
    bool timedOut = false;
    TEST_CHECK(launcher.request(offer, destination, [&](const auto& error) { timedOut = !error.empty(); }));
    until([&] { return static_cast<bool>(received.pending); });
    until([&] { return !launcher.busy(); });
    TEST_CHECK(timedOut);
    g_dbus_method_invocation_return_value(received.pending.get(), nullptr);
    received.pending.reset();
    bool cancelled = false;
    TEST_CHECK(launcher.request(offer, destination, [&](const auto& error) { cancelled = !error.empty(); }));
    until([&] { return static_cast<bool>(received.pending); });
    launcher.cancel();
    until([&] { return !launcher.busy(); });
    TEST_CHECK(cancelled);
    g_dbus_method_invocation_return_value(received.pending.get(), nullptr);
    received.pending.reset();
    received.hold = false;
    bool retired = false;
    {
      ExtractionLauncher shortLived;
      TEST_CHECK(shortLived.request(offer, destination, [&](const auto& error) { retired = !error.empty(); }));
    }
    until([&] { return retired; });
    TEST_CHECK(g_dbus_connection_unregister_object(connection.get(), registration));
    TEST_CHECK(g_dbus_connection_close_sync(connection.get(), nullptr, nullptr));
    connection.reset();
    g_test_dbus_down(bus.get());
  }
} // namespace
int main() {
  try {
    run();
    std::println("extraction launcher checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "extraction launcher: {}", error.what());
    return 1;
  }
}
