#include "fs/gio_ptr.h"
#include "platform/file_chooser_portal.h"
#include "test_check.h"

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>

namespace {
  constexpr const char* kBusName = "org.freedesktop.impl.portal.desktop.luciole";
  constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";
  constexpr const char* kInterface = "org.freedesktop.impl.portal.FileChooser";

  void until(const std::function<bool()>& done) {
    const auto deadline = g_get_monotonic_time() + 8000000;
    while (!done()) {
      (void)g_main_context_iteration(nullptr, false);
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
  }

  // Lets queued messages and idle dispatches settle.
  void settle() {
    const auto end = g_get_monotonic_time() + 100000;
    while (g_get_monotonic_time() < end) {
      (void)g_main_context_iteration(nullptr, false);
      g_usleep(1000);
    }
  }

  struct Reply {
    bool done = false;
    GVariant* value = nullptr;
    GError* error = nullptr;
    ~Reply() {
      if (value) {
        g_variant_unref(value);
      }
      if (error) {
        g_error_free(error);
      }
    }
  };

  void call(
      GDBusConnection* connection, const char* path, const char* interface, const char* method, GVariant* parameters,
      Reply& reply
  ) {
    g_dbus_connection_call(
        connection, kBusName, path, interface, method, parameters, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, G_MAXINT,
        nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          auto& reply = *static_cast<Reply*>(data);
          reply.value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &reply.error);
          reply.done = true;
        },
        &reply
    );
  }

  GVariant* openFileParameters(const char* handle, GVariant* options) {
    return g_variant_new("(osss@a{sv})", handle, "org.example.App", "wayland:abc", "Pick text", options);
  }

  GVariant* emptyOptions() { return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0); }

  void openFile(GDBusConnection* connection, const char* handle, GVariant* options, Reply& reply) {
    call(connection, kObjectPath, kInterface, "OpenFile", openFileParameters(handle, options), reply);
  }

  void closeRequest(GDBusConnection* connection, const char* handle, Reply& reply) {
    call(connection, handle, "org.freedesktop.impl.portal.Request", "Close", nullptr, reply);
  }

  // The (u a{sv}) reply's response code; `results` receives a new reference.
  guint32 response(const Reply& reply, GVariant** results) {
    TEST_CHECK(reply.done && reply.value && !reply.error);
    guint32 code = 0;
    g_variant_get(reply.value, "(u@a{sv})", &code, results);
    return code;
  }

  luciole::GioPtr<GDBusConnection> connect(GTestDBus* bus) {
    luciole::GioPtr<GDBusConnection> connection(g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(bus),
        static_cast<GDBusConnectionFlags>(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
        ),
        nullptr, nullptr, nullptr
    ));
    TEST_CHECK(connection);
    return connection;
  }

  bool nameOwned(GDBusConnection* connection) {
    GVariant* reply = g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", kBusName), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr
    );
    TEST_CHECK(reply);
    gboolean owned = FALSE;
    g_variant_get(reply, "(b)", &owned);
    g_variant_unref(reply);
    return owned != FALSE;
  }

  void checkMnemonics() {
    const auto label = [](const char* text) {
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&options, "{sv}", "accept_label", g_variant_new_string(text));
      GVariant* parameters = g_variant_ref_sink(
          openFileParameters("/org/freedesktop/portal/desktop/request/1_1/label", g_variant_builder_end(&options))
      );
      auto request = luciole::parseFileChooserRequest(luciole::FileChooserMode::Open, parameters);
      g_variant_unref(parameters);
      return request.acceptLabel;
    };
    TEST_CHECK(label("_Open") == "Open");
    TEST_CHECK(label("Save__as_x") == "Save_asx");
    TEST_CHECK(label("Plain") == "Plain");
    GVariant* foreign = g_variant_ref_sink(openFileParameters("/org/freedesktop/portal/desktop", emptyOptions()));
    bool rejected = false;
    try {
      luciole::parseFileChooserRequest(luciole::FileChooserMode::Open, foreign);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    g_variant_unref(foreign);
    TEST_CHECK(rejected);
  }

  // SaveFiles must name at least one file, and a name cut short at an embedded NUL must not reach the dialog.
  void checkSaveFilesNames() {
    const auto rejected = [](std::initializer_list<std::string_view> names) {
      GVariantBuilder files;
      g_variant_builder_init(&files, G_VARIANT_TYPE("aay"));
      for (const auto name : names) {
        g_variant_builder_add_value(
            &files, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, name.data(), name.size(), 1)
        );
      }
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&options, "{sv}", "files", g_variant_builder_end(&files));
      GVariant* parameters = g_variant_ref_sink(
          openFileParameters("/org/freedesktop/portal/desktop/request/1_1/files", g_variant_builder_end(&options))
      );
      bool thrown = false;
      try {
        luciole::parseFileChooserRequest(luciole::FileChooserMode::SaveFiles, parameters);
      } catch (const std::runtime_error&) {
        thrown = true;
      }
      g_variant_unref(parameters);
      return thrown;
    };
    using namespace std::string_view_literals;
    TEST_CHECK(rejected({}));
    TEST_CHECK(rejected({"a\0b\0"sv}));
    TEST_CHECK(!rejected({"a.txt\0"sv}));
  }

  void run() {
    using luciole::FileChooserCall;
    using luciole::FileChooserRequest;
    using luciole::FilePattern;
    checkMnemonics();
    checkSaveFilesNames();

    const luciole::GioPtr<GTestDBus> bus(g_test_dbus_new(G_TEST_DBUS_NONE));
    g_test_dbus_up(bus.get());
    auto client = connect(bus.get());
    auto stranger = connect(bus.get());

    struct State {
      bool keep = true;
      unsigned requests = 0;
      std::optional<FileChooserRequest> request;
      std::shared_ptr<FileChooserCall> call;
    } state;
    {
      luciole::FileChooserService service([&state](FileChooserRequest request, std::shared_ptr<FileChooserCall> call) {
        ++state.requests;
        state.request = std::move(request);
        if (state.keep) {
          state.call = std::move(call);
        }
      });
      TEST_CHECK(service.awaitingRequest());
      until([&] { return nameOwned(client.get()); });

      // OpenFile parsing and completion.
      const std::string folder = "/tmp/caf\xe9 folder";
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&options, "{sv}", "accept_label", g_variant_new_string("_Open"));
      g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(TRUE));
      g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_string("no"));
      g_variant_builder_add(
          &options, "{sv}", "filters",
          g_variant_new_parsed(
              "[('Text', [(uint32 0, '*.txt'), (uint32 7, 'odd'), (uint32 1, 'text/plain')]), "
              "('Images', [(uint32 1, 'image/*')])]"
          )
      );
      g_variant_builder_add(
          &options, "{sv}", "current_filter", g_variant_new_parsed("('Images', [(uint32 1, 'image/*')])")
      );
      g_variant_builder_add(
          &options, "{sv}", "choices",
          g_variant_new_parsed(
              "[('encoding', 'Encoding', [('utf8', 'UTF-8'), ('latin1', 'Latin-1')], 'latin1'), "
              "('readonly', 'Read only', @a(ss) [], 'true')]"
          )
      );
      g_variant_builder_add(&options, "{sv}", "current_folder", g_variant_new_bytestring(folder.c_str()));
      const char* first = "/org/freedesktop/portal/desktop/request/1_1/first";
      Reply opened;
      openFile(client.get(), first, g_variant_builder_end(&options), opened);
      until([&] { return state.requests == 1; });
      TEST_CHECK(!service.awaitingRequest());
      TEST_CHECK(state.call && !state.call->finished() && !opened.done);
      const auto& request = *state.request;
      TEST_CHECK(request.handle == first);
      TEST_CHECK(request.appId == "org.example.App");
      TEST_CHECK(request.parentWindow == "wayland:abc");
      TEST_CHECK(request.title == "Pick text");
      TEST_CHECK(request.acceptLabel == "Open");
      TEST_CHECK(request.modal && request.multiple && !request.directory);
      const luciole::FileChooserFilter text{
          .name = "Text",
          .patterns = {
              {.kind = FilePattern::Kind::Glob, .pattern = "*.txt"},
              {.kind = FilePattern::Kind::MimeType, .pattern = "text/plain"}
          }
      };
      const luciole::FileChooserFilter images{
          .name = "Images", .patterns = {{.kind = FilePattern::Kind::MimeType, .pattern = "image/*"}}
      };
      TEST_CHECK(request.filters.size() == 2 && request.filters[0] == text && request.filters[1] == images);
      TEST_CHECK(request.currentFilter == images);
      TEST_CHECK(request.choices.size() == 2);
      TEST_CHECK(request.choices[0].id == "encoding" && request.choices[0].label == "Encoding");
      TEST_CHECK(request.choices[0].initial == "latin1" && request.choices[0].options.size() == 2);
      TEST_CHECK(request.choices[0].options[1].first == "latin1" && request.choices[0].options[1].second == "Latin-1");
      TEST_CHECK(request.choices[1].id == "readonly" && request.choices[1].options.empty());
      TEST_CHECK(request.choices[1].initial == "true");
      TEST_CHECK(request.currentFolder == luciole::Location::fromPath(folder));

      state.call->complete(
          {.uris = {"file:///tmp/a%20b"}, .currentFilter = std::nullopt, .choices = {{"encoding", "utf8"}}}
      );
      TEST_CHECK(state.call->finished());
      state.call->cancel();
      until([&] { return opened.done; });
      GVariant* results = nullptr;
      TEST_CHECK(response(opened, &results) == 0);
      GVariant* uris = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
      TEST_CHECK(uris && g_variant_n_children(uris) == 1);
      const char* uri = nullptr;
      g_variant_get_child(uris, 0, "&s", &uri);
      TEST_CHECK(std::string(uri) == "file:///tmp/a%20b");
      g_variant_unref(uris);
      GVariant* choices = g_variant_lookup_value(results, "choices", G_VARIANT_TYPE("a(ss)"));
      TEST_CHECK(choices && g_variant_n_children(choices) == 1);
      const char* choiceId = nullptr;
      const char* choiceValue = nullptr;
      g_variant_get_child(choices, 0, "(&s&s)", &choiceId, &choiceValue);
      TEST_CHECK(std::string(choiceId) == "encoding" && std::string(choiceValue) == "utf8");
      g_variant_unref(choices);
      TEST_CHECK(g_variant_n_children(results) == 2);
      TEST_CHECK(!g_variant_lookup_value(results, "writable", nullptr));
      TEST_CHECK(!g_variant_lookup_value(results, "current_filter", nullptr));
      g_variant_unref(results);
      // The finished call no longer exports its Request object.
      Reply late;
      closeRequest(client.get(), first, late);
      until([&] { return late.done; });
      TEST_CHECK(late.error);
      state.call.reset();

      // Close from another connection is refused, from the caller it answers 2.
      const char* second = "/org/freedesktop/portal/desktop/request/1_1/second";
      Reply pending;
      openFile(client.get(), second, emptyOptions(), pending);
      until([&] { return state.requests == 2; });
      unsigned closed = 0;
      state.call->setCloseCallback([&] {
        ++closed;
        state.call.reset();
      });
      Reply denied;
      closeRequest(stranger.get(), second, denied);
      until([&] { return denied.done; });
      TEST_CHECK(g_error_matches(denied.error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED));
      // A duplicate handle cannot export its Request object and fails at once.
      Reply duplicate;
      openFile(client.get(), second, emptyOptions(), duplicate);
      until([&] { return duplicate.done; });
      results = nullptr;
      TEST_CHECK(response(duplicate, &results) == 2 && g_variant_n_children(results) == 0);
      g_variant_unref(results);
      settle();
      TEST_CHECK(state.call && !state.call->finished() && !pending.done && closed == 0);
      Reply close;
      closeRequest(client.get(), second, close);
      until([&] { return close.done && pending.done; });
      TEST_CHECK(close.value && !close.error);
      results = nullptr;
      TEST_CHECK(response(pending, &results) == 2 && g_variant_n_children(results) == 0);
      g_variant_unref(results);
      settle();
      TEST_CHECK(closed == 1 && !state.call);

      // A call dropped unanswered replies 2.
      state.keep = false;
      Reply dropped;
      openFile(client.get(), "/org/freedesktop/portal/desktop/request/1_1/third", emptyOptions(), dropped);
      until([&] { return dropped.done; });
      results = nullptr;
      TEST_CHECK(response(dropped, &results) == 2 && g_variant_n_children(results) == 0);
      g_variant_unref(results);

      // SaveFile: current_file supplies the folder and the name, over current_name.
      GVariantBuilder saveOptions;
      g_variant_builder_init(&saveOptions, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&saveOptions, "{sv}", "current_name", g_variant_new_string("ignored.txt"));
      g_variant_builder_add(&saveOptions, "{sv}", "current_file", g_variant_new_bytestring("/tmp/docs/r\xe9port.odt"));
      Reply save;
      call(
          client.get(), kObjectPath, kInterface, "SaveFile",
          openFileParameters("/org/freedesktop/portal/desktop/request/1_1/fourth", g_variant_builder_end(&saveOptions)),
          save
      );
      until([&] { return save.done; });
      TEST_CHECK(state.requests == 4 && state.request->mode == luciole::FileChooserMode::Save);
      TEST_CHECK(state.request->currentName == "r\xe9port.odt");
      TEST_CHECK(state.request->currentFolder == luciole::Location::fromPath("/tmp/docs"));

      // SaveFiles keeps the requested names in order, as raw bytes.
      GVariantBuilder filesOptions;
      g_variant_builder_init(&filesOptions, G_VARIANT_TYPE_VARDICT);
      const char* names[] = {"b.txt", "a\xff.png", nullptr};
      g_variant_builder_add(&filesOptions, "{sv}", "files", g_variant_new_bytestring_array(names, -1));
      Reply saveFiles;
      call(
          client.get(), kObjectPath, kInterface, "SaveFiles",
          openFileParameters("/org/freedesktop/portal/desktop/request/1_1/fifth", g_variant_builder_end(&filesOptions)),
          saveFiles
      );
      until([&] { return saveFiles.done; });
      TEST_CHECK(state.requests == 5 && state.request->mode == luciole::FileChooserMode::SaveFiles);
      TEST_CHECK((state.request->files == std::vector<std::string>{"b.txt", "a\xff.png"}));
    }
    settle();
    TEST_CHECK(g_dbus_connection_close_sync(stranger.get(), nullptr, nullptr));
    TEST_CHECK(g_dbus_connection_close_sync(client.get(), nullptr, nullptr));
    stranger.reset();
    client.reset();
    g_test_dbus_down(bus.get());
  }
} // namespace

int main() {
  try {
    run();
    std::println("file chooser portal checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "file chooser portal: {}", error.what());
    return 1;
  }
}
