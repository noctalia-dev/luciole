#include "core/log.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "platform/file_launcher.h"
#include "test_check.h"

#include <algorithm>
#include <print>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {
  struct VariantUnref {
    void operator()(GVariant* value) const { g_variant_unref(value); }
  };
  using Variant = std::unique_ptr<GVariant, VariantUnref>;
  luciole::ApplicationList query(
      luciole::FileLauncher& launcher, bool all, std::string search = {},
      std::vector<std::string> types = {"text/plain"}
  ) {
    std::optional<luciole::ApplicationList> result;
    launcher.applications(std::move(types), [&](auto value) { result = std::move(value); }, all, std::move(search));
    const auto deadline = g_get_monotonic_time() + 5000000;
    while (launcher.busy()) {
      (void)g_main_context_iteration(nullptr, false);
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(result.has_value());
    return std::move(*result);
  }

  luciole::ApplicationChoice testCatalog(luciole::FileLauncher& launcher, bool large) {
    TEST_CHECK(std::string_view(g_get_user_config_dir()) == g_getenv("XDG_CONFIG_HOME"));
    TEST_CHECK(std::string_view(g_get_user_data_dir()) == g_getenv("XDG_DATA_HOME"));
    const auto suggested = query(launcher, false);
    TEST_CHECK(
        !suggested.choices.empty()
        && suggested.choices.front().id == "fixture.desktop"
        && suggested.choices.front().suggested
        && suggested.choices.front().isDefault
    );
    TEST_CHECK(std::ranges::none_of(suggested.choices, [](const auto& app) { return app.id == "other.desktop"; }));
    TEST_CHECK(query(launcher, false, "NOCTALIA LAUNCH FIXTURE CAFÉ").choices.empty());
    const auto other = query(launcher, true, "NOCTALIA LAUNCH FIXTURE CAFE\u0301");
    TEST_CHECK(
        other.choices.size() == 1
        && other.choices.front().id == "other.desktop"
        && !other.choices.front().suggested
        && !other.choices.front().isDefault
    );
    const auto all = query(launcher, true);
    std::set<std::string> ids;
    for (const auto& app : all.choices) {
      TEST_CHECK(app.id != "hidden.desktop" && app.id != "no-files.desktop" && ids.insert(app.id).second);
    }
    TEST_CHECK(all.choices.front().isDefault);
    if (large) {
      TEST_CHECK(all.truncated && all.choices.size() == 128);
      const auto last = query(launcher, true, "lastneedle");
      TEST_CHECK(!last.truncated && last.choices.size() == 1 && last.choices.front().id == "bulk-149.desktop");
    }
    return other.choices.front();
  }

  void testDbus(luciole::FileLauncher& launcher, const std::vector<luciole::Location>& locations) {
    const luciole::GioPtr<GTestDBus> bus(g_test_dbus_new(G_TEST_DBUS_NONE));
    g_test_dbus_up(bus.get());
    luciole::GioPtr<GDBusConnection> connection(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr));
    TEST_CHECK(connection);
    const Variant name(g_dbus_connection_call_sync(
        connection.get(), "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.example.Fixture", 4U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        nullptr
    ));
    TEST_CHECK(name);
    const char* xml = "<node><interface name='org.freedesktop.Application'>"
                      "<method name='Open'><arg type='as' direction='in'/><arg type='a{sv}' direction='in'/></method>"
                      "</interface></node>";
    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(xml, nullptr);
    TEST_CHECK(node);
    struct Received {
      std::vector<std::string> uris;
      std::string token;
      int calls = 0;
    } received;
    static const GDBusInterfaceVTable handler{
        .method_call =
            [](GDBusConnection*, const char*, const char*, const char*, const char* method, GVariant* parameters,
               GDBusMethodInvocation* invocation, gpointer data) {
              auto& received = *static_cast<Received*>(data);
              TEST_CHECK(std::string_view(method) == "Open");
              const Variant uris(g_variant_get_child_value(parameters, 0));
              const Variant platform(g_variant_get_child_value(parameters, 1));
              const char* token = nullptr;
              TEST_CHECK(g_variant_lookup(platform.get(), "activation-token", "&s", &token));
              received.token = token;
              for (gsize i = 0; i < g_variant_n_children(uris.get()); ++i) {
                const char* uri = nullptr;
                g_variant_get_child(uris.get(), i, "&s", &uri);
                received.uris.emplace_back(uri);
              }
              ++received.calls;
              g_dbus_method_invocation_return_value(invocation, nullptr);
            },
        .get_property = nullptr,
        .set_property = nullptr,
        .padding = {}
    };
    const auto registration = g_dbus_connection_register_object(
        connection.get(), "/org/example/Fixture", node->interfaces[0], &handler, &received, nullptr, nullptr
    );
    TEST_CHECK(registration);
    g_dbus_node_info_unref(node);
    const auto choices = query(launcher, false);
    const auto app = std::ranges::find(choices.choices, "org.example.Fixture.desktop", &luciole::ApplicationChoice::id);
    TEST_CHECK(app != choices.choices.end());
    launcher.setActivationProvider([](const auto&, const auto& reply) { reply("dbus-fresh"); });
    launcher.openWith(locations, *app);
    const auto wait = [&](int count) {
      const auto deadline = g_get_monotonic_time() + 5000000;
      while (launcher.busy() || received.calls < count) {
        (void)g_main_context_iteration(nullptr, false);
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
    };
    wait(1);
    std::vector<std::string> expected;
    expected.reserve(locations.size());
    for (const auto& location : locations) {
      expected.push_back(location.uri());
    }
    TEST_CHECK(received.token == "dbus-fresh" && received.uris == expected);
    received.uris.clear();
    launcher.open(locations.front());
    wait(2);
    TEST_CHECK(received.token == "dbus-fresh" && received.uris == std::vector{locations.front().uri()});
    TEST_CHECK(g_dbus_connection_unregister_object(connection.get(), registration));
    TEST_CHECK(g_dbus_connection_close_sync(connection.get(), nullptr, nullptr));
    connection.reset();
    g_test_dbus_down(bus.get());
  }

  void testMixed(luciole::FileLauncher& launcher) {
    const auto common = query(launcher, false, "Private", {"image/png", "text/plain", "image/png"});
    TEST_CHECK(
        common.choices.size() == 1
        && common.choices.front().id == "common.desktop"
        && common.choices.front().suggested
        && !common.choices.front().isDefault
    );
    TEST_CHECK(query(launcher, false, "launch fixture Café", {"image/png", "text/plain"}).choices.empty());
    const auto other = query(launcher, true, "launch fixture Café", {"image/png", "text/plain"});
    TEST_CHECK(other.choices.size() == 1 && !other.choices.front().suggested && !other.choices.front().isDefault);
  }

  luciole::ApplicationChoice testNoDisplay(luciole::FileLauncher& launcher) {
    for (const bool all : {false, true}) {
      const auto apps = query(launcher, all, "Luciole NoDisplay fixture");
      TEST_CHECK(apps.choices.size() == 2);
      TEST_CHECK(apps.choices.front().id == "fixture.desktop" && apps.choices.front().isDefault);
      for (const auto& app : apps.choices) {
        TEST_CHECK(app.suggested && !g_app_info_should_show(app.app.get()));
      }
    }
    const auto searched = query(launcher, true, "LUCIOLE NODISPLAY FIXTURE DEFAULT");
    TEST_CHECK(searched.choices.size() == 1 && searched.choices.front().id == "fixture.desktop");
    const auto mixed = query(launcher, false, "Luciole NoDisplay fixture", {"text/plain", "image/png"});
    TEST_CHECK(mixed.choices.size() == 1 && mixed.choices.front().id == "common.desktop");
    TEST_CHECK(mixed.choices.front().suggested && !mixed.choices.front().isDefault);
    TEST_CHECK(query(launcher, true, "unrelated.desktop").choices.empty());
    return mixed.choices.front();
  }

  void testDefault(luciole::FileLauncher& launcher, const luciole::ApplicationChoice& app, bool failure) {
    const auto set = [&](const std::string& mime) {
      std::optional<std::string> error;
      launcher.setDefault(mime, app, [&](std::string value) { error = std::move(value); });
      const auto deadline = g_get_monotonic_time() + 5000000;
      while (launcher.busy()) {
        (void)g_main_context_iteration(nullptr, false);
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
      TEST_CHECK(error.has_value());
      return *error;
    };
    TEST_CHECK(!set("").empty());
    const auto error = set("text/plain");
    TEST_CHECK(error.empty() != failure);
    if (!failure) {
      const luciole::GioPtr<GAppInfo> current(g_app_info_get_default_for_type("text/plain", false));
      TEST_CHECK(current && std::string_view(g_app_info_get_id(current.get())) == app.id);
      const auto listed = query(launcher, false);
      TEST_CHECK(!listed.choices.empty() && listed.choices.front().id == app.id && listed.choices.front().isDefault);
    }
  }
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 3) {
      throw std::invalid_argument("expected mode and native fixture paths");
    }
    initLogLevelFromEnvironment();
    GlibPollSource glib;
    auto launcher = std::make_unique<luciole::FileLauncher>();
    const bool destroy = std::string_view(argv[1]) == "destroy";
    const bool expectError = std::string_view(argv[1]) == "error" || std::string_view(argv[1]) == "no-handler";
    const bool sequential = std::string_view(argv[1]) == "sequential";
    const std::string_view mode(argv[1]);
    const bool catalog = mode == "catalog", changeDefault = mode == "default", defaultError = mode == "default-error";
    const bool other = mode == "other";
    const bool with = mode == "with" || other;
    const bool batch = mode == "batch" || mode == "batch-single";
    const bool groupedOpen = mode == "grouped-open";
    const bool tokens = mode == "tokens", activationDestroy = mode == "activation-destroy";
    int callbacks = 0, errors = 0;
    launcher->setChangedCallback([&](const std::string& error) {
      ++callbacks;
      if (!error.empty()) {
        ++errors;
      }
    });
    const auto deadline = g_get_monotonic_time() + 5000000;
    if (mode == "remote") {
      launcher->open(luciole::Location::fromUri("sftp://fixture.invalid/private.txt"));
      while (launcher->busy()) {
        (void)g_main_context_iteration(nullptr, false);
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
      TEST_CHECK(callbacks == 1 && errors == 0);
      return 0;
    }
    if (mode.starts_with("terminal-")) {
      launcher->setTerminalCommand(
          mode == "terminal-setting"       ? "foot"
              : mode == "terminal-missing" ? "missing-luciole-terminal"
                                           : ""
      );
      const auto apps = query(*launcher, false);
      TEST_CHECK(!apps.choices.empty());
      for (int i = 2; i < argc; ++i) {
        const auto location = luciole::Location::fromPath(argv[i]);
        if (mode == "terminal-with") {
          launcher->openWith(location, apps.choices.front());
        } else {
          launcher->open(location);
        }
      }
      while (launcher->busy()) {
        (void)g_main_context_iteration(nullptr, false);
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
      TEST_CHECK(callbacks == argc - 2 && errors == (mode == "terminal-missing" ? argc - 2 : 0));
      return 0;
    }
    if (mode == "dbus") {
      std::vector<luciole::Location> locations;
      for (int i = 2; i < argc; ++i) {
        locations.push_back(luciole::Location::fromPath(argv[i]));
      }
      testDbus(*launcher, locations);
      TEST_CHECK(callbacks == 2 && errors == 0);
      return 0;
    }
    if (mode == "mixed-catalog") {
      testMixed(*launcher);
      return 0;
    }
    if (mode == "nodisplay") {
      const auto app = testNoDisplay(*launcher);
      std::vector<luciole::Location> locations;
      for (int i = 2; i < argc; ++i) {
        locations.push_back(luciole::Location::fromPath(argv[i]));
      }
      launcher->openWith(locations, app);
      const auto deadline = g_get_monotonic_time() + 5000000;
      while (launcher->busy()) {
        (void)g_main_context_iteration(nullptr, false);
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
      TEST_CHECK(callbacks == 1 && errors == 0);
      return 0;
    }
    if (batch || groupedOpen || tokens || activationDestroy) {
      const auto apps = query(*launcher, false);
      const auto app = std::ranges::find(apps.choices, "fixture.desktop", &luciole::ApplicationChoice::id);
      TEST_CHECK(app != apps.choices.end());
      std::vector<luciole::Location> locations;
      for (int i = 2; i < argc; ++i) {
        locations.push_back(luciole::Location::fromPath(argv[i]));
      }
      std::vector<luciole::FileLauncher::ActivationReply> replies;
      std::vector<std::string> appIds;
      if (tokens || activationDestroy || groupedOpen) {
        launcher->setActivationProvider([&](const std::string& id, auto reply) {
          appIds.push_back(id);
          if (groupedOpen) {
            reply({});
          } else {
            replies.push_back(std::move(reply));
          }
        });
      }
      if (groupedOpen) {
        launcher->open(locations);
      } else if (batch || activationDestroy) {
        launcher->openWith(locations, *app);
      } else {
        TEST_CHECK(locations.size() == 2);
        launcher->openWith(locations.front(), *app);
        launcher->open(locations.back());
      }
      TEST_CHECK(launcher->busy());
      if (activationDestroy) {
        TEST_CHECK(replies.size() == 1 && callbacks == 0);
        launcher.reset();
        replies.front()("cancelled-token");
        replies.front()("duplicate-token");
        TEST_CHECK(callbacks == 0);
      } else {
        if (tokens) {
          TEST_CHECK(
              (appIds == std::vector<std::string>{"fixture.desktop", ""}) && replies.size() == 2 && callbacks == 0
          );
          replies[1]("fresh-second");
          replies[0]("fresh-first");
          replies[0]("ignored-duplicate");
        }
        while (launcher->busy()) {
          (void)g_main_context_iteration(nullptr, false);
          TEST_CHECK(g_get_monotonic_time() < deadline);
          g_usleep(1000);
        }
        TEST_CHECK(callbacks == (tokens || groupedOpen ? 2 : 1) && errors == 0);
        if (groupedOpen) {
          TEST_CHECK((appIds == std::vector<std::string>{"fixture.desktop", "other.desktop"}));
        }
      }
      return 0;
    }
    std::optional<luciole::ApplicationChoice> choice;
    bool staleChoice = false;
    if (other || catalog || changeDefault || defaultError) {
      choice = testCatalog(*launcher, catalog);
      if (changeDefault || defaultError) {
        testDefault(*launcher, *choice, defaultError);
      }
      if (catalog || defaultError) {
        TEST_CHECK(callbacks == 0);
        std::println("application catalog/default checks passed");
        return 0;
      }
    }
    if (with && !other) {
      launcher->applications("text/plain", [&](const auto&) { staleChoice = true; });
      launcher->applications("text/plain", [&](auto apps) {
        for (auto& app : apps.choices) {
          if (app.id == "fixture.desktop") {
            choice = std::move(app);
          }
        }
      });
      while (launcher->busy()) {
        (void)g_main_context_iteration(nullptr, false);
        if (g_get_monotonic_time() >= deadline) {
          throw std::runtime_error("application lookup timed out");
        }
        g_usleep(1000);
      }
      if (staleChoice || !choice) {
        throw std::runtime_error("application choice missing or superseded query published");
      }
    }
    for (int index = 2; index < argc; ++index) {
      if (with) {
        launcher->openWith(luciole::Location::fromPath(argv[index]), *choice);
      } else {
        launcher->open(luciole::Location::fromPath(argv[index]));
      }
      while (sequential && launcher->busy()) {
        if (g_get_monotonic_time() >= deadline) {
          throw std::runtime_error("sequential launch timed out");
        }
        std::vector<pollfd> fds;
        const auto first = glib.addPollFds(fds);
        const int requested = glib.pollTimeoutMs();
        if (::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) < 0) {
          throw std::runtime_error("poll failed");
        }
        glib.dispatch(fds, first);
      }
    }
    if (destroy) {
      launcher.reset();
    }
    bool drained = false;
    guint drainTimer = 0;
    if (destroy) {
      drainTimer = g_timeout_add(
          100,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &drained
      );
    }
    while (destroy ? !drained : launcher->busy()) {
      if (g_get_monotonic_time() >= deadline) {
        if (drainTimer) {
          g_source_remove(drainTimer);
        }
        throw std::runtime_error("file launcher timed out");
      }
      std::vector<pollfd> fds;
      const auto first = glib.addPollFds(fds);
      const int requested = glib.pollTimeoutMs();
      if (::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) < 0) {
        throw std::runtime_error("poll failed");
      }
      glib.dispatch(fds, first);
    }
    if (callbacks != (destroy ? 0 : argc - 2) || errors != (expectError ? argc - 2 : 0)) {
      throw std::runtime_error("unexpected launch callback/error count");
    }
    if (other) {
      const luciole::GioPtr<GAppInfo> current(g_app_info_get_default_for_type("text/plain", false));
      TEST_CHECK(current && std::string_view(g_app_info_get_id(current.get())) == "fixture.desktop");
    }
    std::println("file launcher checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "file launcher test: {}", error.what());
    return 1;
  }
}
