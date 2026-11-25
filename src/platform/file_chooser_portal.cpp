#include "platform/file_chooser_portal.h"

#include "core/log.h"

#include <exception>
#include <gio/gio.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
  constexpr const char* kBusName = "org.freedesktop.impl.portal.desktop.luciole";
  constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";
  // Matches the default D-Bus call timeout: the activating caller has given up by then.
  constexpr guint kActivationTimeoutSeconds = 25;
  constexpr const char* kIntrospection = R"(<node>
  <interface name="org.freedesktop.impl.portal.FileChooser">
    <method name="OpenFile">
      <arg type="o" name="handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFile">
      <arg type="o" name="handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFiles">
      <arg type="o" name="handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
      <arg type="a{sv}" name="results" direction="out"/>
    </method>
  </interface>
</node>)";
  constexpr const char* kRequestIntrospection = R"(<node>
  <interface name="org.freedesktop.impl.portal.Request">
    <method name="Close"/>
  </interface>
</node>)";
  constexpr guint kResponseSuccess = 0;
  constexpr guint kResponseCancelled = 1;
  constexpr guint kResponseOther = 2;

  const Logger kLog("file-chooser-dbus");

  // GTK mnemonics: a lone `_` marks the access key, `__` is a literal underscore.
  std::string stripMnemonics(std::string_view label) {
    std::string text;
    text.reserve(label.size());
    for (std::size_t i = 0; i < label.size(); ++i) {
      if (label[i] != '_') {
        text.push_back(label[i]);
      } else if (i + 1 < label.size() && label[i + 1] == '_') {
        text.push_back('_');
        ++i;
      }
    }
    return text;
  }

  // `value`: a (sa(us)) tuple.
  luciole::FileChooserFilter parseFilter(GVariant* value) {
    luciole::FileChooserFilter filter;
    const char* name = nullptr;
    GVariantIter* patterns = nullptr;
    g_variant_get(value, "(&sa(us))", &name, &patterns);
    filter.name = name;
    guint32 kind = 0;
    const char* pattern = nullptr;
    while (g_variant_iter_next(patterns, "(u&s)", &kind, &pattern)) {
      if (kind == 0) {
        filter.patterns.push_back({.kind = luciole::FilePattern::Kind::Glob, .pattern = pattern});
      } else if (kind == 1) {
        filter.patterns.push_back({.kind = luciole::FilePattern::Kind::MimeType, .pattern = pattern});
      }
    }
    g_variant_iter_free(patterns);
    return filter;
  }

  GVariant* filterVariant(const luciole::FileChooserFilter& filter) {
    GVariantBuilder patterns;
    g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)"));
    for (const auto& pattern : filter.patterns) {
      g_variant_builder_add(
          &patterns, "(us)", static_cast<guint32>(pattern.kind == luciole::FilePattern::Kind::MimeType ? 1 : 0),
          pattern.pattern.c_str()
      );
    }
    return g_variant_new("(s@a(us))", filter.name.c_str(), g_variant_builder_end(&patterns));
  }

  // std::string(const char*) would silently cut a bytestring at an embedded NUL.
  std::optional<std::string> bytestring(GVariant* value) {
    const char* text = g_variant_get_bytestring(value);
    const std::string_view view(text);
    const auto size = g_variant_get_size(value);
    return size == 0 || view.size() + 1 == size ? std::optional<std::string>(view) : std::nullopt;
  }
  GVariant* emptyResults() { return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0); }
} // namespace

luciole::FileChooserRequest luciole::parseFileChooserRequest(FileChooserMode mode, GVariant* parameters) {
  FileChooserRequest request;
  request.mode = mode;
  const char* handle = nullptr;
  const char* appId = nullptr;
  const char* parentWindow = nullptr;
  const char* title = nullptr;
  GVariant* options = nullptr;
  g_variant_get(parameters, "(&o&s&s&s@a{sv})", &handle, &appId, &parentWindow, &title, &options);
  // Request objects belong under the portal request tree; other paths would be exported on our connection.
  if (!std::string_view(handle).starts_with("/org/freedesktop/portal/desktop/request/")) {
    g_variant_unref(options);
    throw std::runtime_error("invalid request handle");
  }
  request.handle = handle;
  request.appId = appId;
  request.parentWindow = parentWindow;
  request.title = title;

  if (GVariant* label = g_variant_lookup_value(options, "accept_label", G_VARIANT_TYPE_STRING)) {
    request.acceptLabel = stripMnemonics(g_variant_get_string(label, nullptr));
    g_variant_unref(label);
  }
  gboolean flag = FALSE;
  if (g_variant_lookup(options, "modal", "b", &flag)) {
    request.modal = flag != FALSE;
  }
  if (g_variant_lookup(options, "multiple", "b", &flag)) {
    request.multiple = flag != FALSE;
  }
  if (g_variant_lookup(options, "directory", "b", &flag)) {
    request.directory = flag != FALSE;
  }

  if (GVariant* filters = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"))) {
    GVariantIter iter;
    g_variant_iter_init(&iter, filters);
    while (GVariant* filter = g_variant_iter_next_value(&iter)) {
      request.filters.push_back(parseFilter(filter));
      g_variant_unref(filter);
    }
    g_variant_unref(filters);
  }
  if (GVariant* filter = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"))) {
    request.currentFilter = parseFilter(filter);
    g_variant_unref(filter);
  }
  if (GVariant* choices = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"))) {
    GVariantIter iter;
    g_variant_iter_init(&iter, choices);
    const char* id = nullptr;
    const char* label = nullptr;
    GVariantIter* entries = nullptr;
    const char* initial = nullptr;
    while (g_variant_iter_next(&iter, "(&s&sa(ss)&s)", &id, &label, &entries, &initial)) {
      FileChooserChoice choice{.id = id, .label = label, .options = {}, .initial = initial};
      const char* optionId = nullptr;
      const char* optionLabel = nullptr;
      while (g_variant_iter_next(entries, "(&s&s)", &optionId, &optionLabel)) {
        choice.options.emplace_back(optionId, optionLabel);
      }
      g_variant_iter_free(entries);
      request.choices.push_back(std::move(choice));
    }
    g_variant_unref(choices);
  }
  // Hold the value while reading it: a borrowed bytestring from g_variant_lookup() does not outlive the lookup.
  if (GVariant* folder = g_variant_lookup_value(options, "current_folder", G_VARIANT_TYPE_BYTESTRING)) {
    const auto path = bytestring(folder);
    g_variant_unref(folder);
    try {
      if (!path) {
        throw std::runtime_error("embedded NUL");
      }
      request.currentFolder = Location::fromPath(*path);
    } catch (const std::exception& error) {
      kLog.debug("ignoring current_folder: {}", error.what());
    }
  }
  if (GVariant* name = g_variant_lookup_value(options, "current_name", G_VARIANT_TYPE_STRING)) {
    request.currentName = g_variant_get_string(name, nullptr);
    g_variant_unref(name);
  }
  // A file to save over: its folder and name replace current_folder and current_name.
  if (GVariant* file = g_variant_lookup_value(options, "current_file", G_VARIANT_TYPE_BYTESTRING)) {
    // An embedded NUL reads as empty, which has no slash and is ignored below.
    const std::string path = bytestring(file).value_or("");
    g_variant_unref(file);
    const auto slash = path.rfind('/');
    if (slash != std::string::npos && slash + 1 < path.size()) {
      try {
        request.currentFolder = Location::fromPath(slash == 0 ? "/" : path.substr(0, slash));
        request.currentName = path.substr(slash + 1);
      } catch (const std::exception& error) {
        kLog.debug("ignoring current_file: {}", error.what());
      }
    }
  }
  bool validFiles = true;
  if (GVariant* files = g_variant_lookup_value(options, "files", G_VARIANT_TYPE_BYTESTRING_ARRAY)) {
    GVariantIter iter;
    g_variant_iter_init(&iter, files);
    while (GVariant* file = g_variant_iter_next_value(&iter)) {
      auto name = bytestring(file);
      validFiles = validFiles && name;
      request.files.push_back(std::move(name).value_or(""));
      g_variant_unref(file);
    }
    g_variant_unref(files);
  }
  g_variant_unref(options);
  if (!validFiles) {
    throw std::runtime_error("files contains an embedded NUL");
  }
  // An empty list would let the dialog accept without returning a single file.
  if (mode == FileChooserMode::SaveFiles && request.files.empty()) {
    throw std::runtime_error("SaveFiles needs at least one file");
  }
  return request;
}

GVariant* luciole::fileChooserResults(const FileChooserResult& result) {
  GVariantBuilder results;
  g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
  GVariantBuilder uris;
  g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
  for (const auto& uri : result.uris) {
    g_variant_builder_add(&uris, "s", uri.c_str());
  }
  g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
  if (result.currentFilter) {
    g_variant_builder_add(&results, "{sv}", "current_filter", filterVariant(*result.currentFilter));
  }
  if (!result.choices.empty()) {
    GVariantBuilder choices;
    g_variant_builder_init(&choices, G_VARIANT_TYPE("a(ss)"));
    for (const auto& [id, value] : result.choices) {
      g_variant_builder_add(&choices, "(ss)", id.c_str(), value.c_str());
    }
    g_variant_builder_add(&results, "{sv}", "choices", g_variant_builder_end(&choices));
  }
  return g_variant_builder_end(&results);
}

luciole::FileChooserCall::FileChooserCall(
    GDBusMethodInvocation* invocation, GDBusConnection* connection, std::string sender
)
    // Takes over the invocation reference GDBus gives the method handler; finish() returns it.
    : m_invocation(invocation), m_connection(G_DBUS_CONNECTION(g_object_ref(connection))), m_sender(std::move(sender)) {
}

luciole::FileChooserCall::~FileChooserCall() {
  fail();
  g_object_unref(m_connection);
}

bool luciole::FileChooserCall::exportRequest(const std::string& handle, GDBusNodeInfo* node) {
  static const GDBusInterfaceVTable vtable = {
      .method_call = closeCall, .get_property = nullptr, .set_property = nullptr, .padding = {}
  };
  GError* error = nullptr;
  m_registration = g_dbus_connection_register_object(
      m_connection, handle.c_str(), node->interfaces[0], &vtable, this, nullptr, &error
  );
  if (m_registration == 0) {
    kLog.warn("cannot export request {}: {}", handle, error->message);
    g_error_free(error);
    return false;
  }
  return true;
}

void luciole::FileChooserCall::complete(const FileChooserResult& result) {
  if (!finished()) {
    finish(kResponseSuccess, fileChooserResults(result));
  }
}

void luciole::FileChooserCall::cancel() {
  if (!finished()) {
    finish(kResponseCancelled, emptyResults());
  }
}

void luciole::FileChooserCall::fail() {
  if (!finished()) {
    finish(kResponseOther, emptyResults());
  }
}

bool luciole::FileChooserCall::finished() const { return m_invocation == nullptr; }

void luciole::FileChooserCall::setCloseCallback(std::function<void()> callback) {
  m_closeCallback = std::move(callback);
}

void luciole::FileChooserCall::finish(unsigned response, GVariant* results) {
  if (m_registration != 0) {
    g_dbus_connection_unregister_object(m_connection, m_registration);
    m_registration = 0;
  }
  // Returning a value consumes the invocation reference this call held.
  g_dbus_method_invocation_return_value(
      std::exchange(m_invocation, nullptr), g_variant_new("(u@a{sv})", static_cast<guint32>(response), results)
  );
}

void luciole::FileChooserCall::closeCall(
    GDBusConnection* /*connection*/, const char* sender, const char* /*path*/, const char* /*interface*/,
    const char* /*method*/, GVariant* /*parameters*/, GDBusMethodInvocation* invocation, void* data
) {
  auto* self = static_cast<FileChooserCall*>(data);
  if (sender == nullptr || self->m_sender != sender) {
    g_dbus_method_invocation_return_error_literal(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "Only the requester can close this request"
    );
    return;
  }
  // The callback may drop the last reference to this call.
  auto callback = std::move(self->m_closeCallback);
  self->fail();
  g_dbus_method_invocation_return_value(invocation, nullptr);
  if (callback) {
    try {
      callback();
    } catch (const std::exception& error) {
      kLog.warn("closing the chooser failed: {}", error.what());
    }
  }
}

luciole::FileChooserService::FileChooserService(Handler handler)
    : m_handler(std::move(handler)), m_node(g_dbus_node_info_new_for_xml(kIntrospection, nullptr)),
      m_requestNode(g_dbus_node_info_new_for_xml(kRequestIntrospection, nullptr)), m_awaiting(true) {
  m_owner = g_bus_own_name(
      G_BUS_TYPE_SESSION, kBusName, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, busAcquired, nullptr, nameLost, this, nullptr
  );
  m_timeout = g_timeout_add_seconds(
      kActivationTimeoutSeconds,
      [](gpointer data) -> gboolean {
        auto* self = static_cast<FileChooserService*>(data);
        self->m_timeout = 0;
        self->m_awaiting = false;
        return G_SOURCE_REMOVE;
      },
      this
  );
}

luciole::FileChooserService::~FileChooserService() {
  GDBusConnection* connection = m_connection != nullptr ? G_DBUS_CONNECTION(g_object_ref(m_connection)) : nullptr;
  release();
  if (connection != nullptr) {
    g_dbus_connection_flush_sync(connection, nullptr, nullptr);
    g_object_unref(connection);
  }
  g_dbus_node_info_unref(m_requestNode);
  g_dbus_node_info_unref(m_node);
}

void luciole::FileChooserService::release() {
  stopAwaiting();
  if (m_registration != 0) {
    g_dbus_connection_unregister_object(m_connection, m_registration);
    m_registration = 0;
  }
  if (m_owner != 0) {
    g_bus_unown_name(m_owner);
    m_owner = 0;
  }
  if (m_connection != nullptr) {
    g_object_unref(m_connection);
    m_connection = nullptr;
  }
}

void luciole::FileChooserService::stopAwaiting() {
  m_awaiting = false;
  if (m_timeout != 0) {
    g_source_remove(m_timeout);
    m_timeout = 0;
  }
}

void luciole::FileChooserService::busAcquired(GDBusConnection* connection, const char* /*name*/, void* data) {
  auto* self = static_cast<FileChooserService*>(data);
  static const GDBusInterfaceVTable vtable = {
      .method_call = methodCall, .get_property = nullptr, .set_property = nullptr, .padding = {}
  };
  GError* error = nullptr;
  self->m_registration = g_dbus_connection_register_object(
      connection, kObjectPath, self->m_node->interfaces[0], &vtable, self, nullptr, &error
  );
  if (self->m_registration == 0) {
    kLog.warn("cannot export {}: {}", kObjectPath, error->message);
    g_error_free(error);
    return;
  }
  self->m_connection = G_DBUS_CONNECTION(g_object_ref(connection));
}

void luciole::FileChooserService::nameLost(GDBusConnection* connection, const char* /*name*/, void* data) {
  auto* self = static_cast<FileChooserService*>(data);
  if (connection == nullptr) {
    kLog.debug("no session bus; FileChooser requests are unavailable");
  }
  // The bus started this process for a call that another owner now receives.
  self->stopAwaiting();
}

void luciole::FileChooserService::methodCall(
    GDBusConnection* connection, const char* sender, const char* /*path*/, const char* /*interface*/,
    const char* method, GVariant* parameters, GDBusMethodInvocation* invocation, void* data
) {
  auto* self = static_cast<FileChooserService*>(data);
  const std::string_view name(method);
  const auto mode = name == "SaveFile" ? FileChooserMode::Save
      : name == "SaveFiles"            ? FileChooserMode::SaveFiles
                                       : FileChooserMode::Open;
  // Exceptions must not unwind through GLib's C frames. Once the call exists, it owns the reply.
  FileChooserRequest request;
  std::shared_ptr<FileChooserCall> call;
  try {
    request = parseFileChooserRequest(mode, parameters);
    call =
        std::shared_ptr<FileChooserCall>(new FileChooserCall(invocation, connection, sender != nullptr ? sender : ""));
  } catch (const std::exception& error) {
    kLog.warn("{} failed: {}", name, error.what());
    g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, error.what());
    return;
  }
  if (!call->exportRequest(request.handle, self->m_requestNode)) {
    call->fail();
    return;
  }
  self->stopAwaiting();
  try {
    self->m_handler(std::move(request), call);
  } catch (const std::exception& error) {
    kLog.warn("{} failed: {}", name, error.what());
    call->fail();
  }
}
