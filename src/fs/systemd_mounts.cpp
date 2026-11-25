#include "fs/systemd_mounts.h"

#include "fs/gio_interop.h"

#include <map>
#include <set>

namespace {
  constexpr auto kManager = "/org/freedesktop/systemd1";

  using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;

  Variant call(
      GDBusConnection* connection, const char* path, const char* interface, const char* method, GVariant* parameters,
      const GVariantType* resultType, gint64 deadline, const luciole::CancelToken& token, GError** error = nullptr
  ) {
    const Variant arguments(parameters ? g_variant_ref_sink(parameters) : nullptr, g_variant_unref);
    const auto remaining = (deadline - g_get_monotonic_time()) / 1000;
    if (remaining <= 0 || token.cancelled()) {
      return {nullptr, g_variant_unref};
    }
    return {
        g_dbus_connection_call_sync(
            connection, "org.freedesktop.systemd1", path, interface, method, arguments.get(), resultType,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, static_cast<int>(remaining), luciole::gioCancellable(token), error
        ),
        g_variant_unref
    };
  }

  std::string property(GVariant* dictionary, const char* name) {
    const char* value = nullptr;
    return g_variant_lookup(dictionary, name, "&s", &value) ? value : "";
  }
} // namespace

std::optional<std::vector<luciole::MountConfiguration>>
luciole::readSystemdMounts(GDBusConnection* connection, const CancelToken& token, bool* subscribed) {
  std::vector<MountConfiguration> result;
  if (!connection) {
    return std::nullopt;
  }
  const auto deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
  if (subscribed && !*subscribed) {
    // Discovery-only helpers never need systemd's stream of unit changes.
    GError* raw = nullptr;
    const auto reply = call(
        connection, kManager, "org.freedesktop.systemd1.Manager", "Subscribe", nullptr, G_VARIANT_TYPE_UNIT, deadline,
        token, &raw
    );
    const GErrorPtr error(raw);
    const GString name(error ? g_dbus_error_get_remote_error(error.get()) : nullptr);
    *subscribed = reply || (name && std::string_view(name.get()) == "org.freedesktop.systemd1.AlreadySubscribed");
  }
  const char* patterns[] = {"*.mount", "*.automount", nullptr};
  const char* states[] = {nullptr};
  const auto units = call(
      connection, kManager, "org.freedesktop.systemd1.Manager", "ListUnitsByPatterns",
      g_variant_new("(^as^as)", states, patterns), G_VARIANT_TYPE("(a(ssssssouso))"), deadline, token
  );
  if (!units) {
    return std::nullopt;
  }
  const Variant list(g_variant_get_child_value(units.get(), 0), g_variant_unref);
  struct UnitPair {
    std::string mount, automount;
  };
  std::map<std::string, UnitPair> pairs;
  const auto count = g_variant_n_children(list.get());
  if (count > 4096) {
    return std::nullopt;
  }
  for (gsize i = 0; i < count; ++i) {
    const Variant unit(g_variant_get_child_value(list.get(), i), g_variant_unref);
    const char* name = nullptr;
    const char* path = nullptr;
    g_variant_get_child(unit.get(), 0, "&s", &name);
    g_variant_get_child(unit.get(), 6, "&o", &path);
    const std::string_view id(name);
    const auto suffix = id.rfind('.');
    if (suffix != std::string_view::npos) {
      auto& pair = pairs[std::string(id.substr(0, suffix))];
      (id.substr(suffix) == ".automount" ? pair.automount : pair.mount) = path;
    }
  }
  std::set<std::string> inspected;
  for (const auto& [name, pair] : pairs) {
    if (pair.mount.empty() || pair.automount.empty()) {
      continue;
    }
    const auto properties = call(
        connection, pair.mount.c_str(), "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.freedesktop.systemd1.Mount"), G_VARIANT_TYPE("(a{sv})"), deadline, token
    );
    if (!properties) {
      return std::nullopt;
    }
    const Variant dictionary(g_variant_get_child_value(properties.get(), 0), g_variant_unref);
    const auto target = property(dictionary.get(), "Where");
    if (target.empty() || target.front() != '/') {
      continue;
    }
    inspected.insert(target);
    const auto type = property(dictionary.get(), "Type");
    if (!networkMountType(type)) {
      continue;
    }
    NativeMountFiles fragments;
    for (const auto& [path, fragment] :
         {std::pair{&pair.mount, &fragments.mount}, std::pair{&pair.automount, &fragments.automount}}) {
      const auto unit = call(
          connection, path->c_str(), "org.freedesktop.DBus.Properties", "Get",
          g_variant_new("(ss)", "org.freedesktop.systemd1.Unit", "FragmentPath"), G_VARIANT_TYPE("(v)"), deadline, token
      );
      if (!unit) {
        return std::nullopt;
      }
      GVariant* value = nullptr;
      g_variant_get(unit.get(), "(v)", &value);
      const Variant held(value, g_variant_unref);
      if (!g_variant_is_of_type(held.get(), G_VARIANT_TYPE_STRING)) {
        return std::nullopt;
      }
      *fragment = g_variant_get_string(held.get(), nullptr);
    }
    result.push_back(
        {.path = target,
         .source = property(dictionary.get(), "What"),
         .type = type,
         .options = property(dictionary.get(), "Options"),
         .automount = true,
         .unitFiles = std::move(fragments)}
    );
  }
  for (const auto& previous : knownNativeMounts()) {
    if (previous.unitFiles
        && !inspected.contains(previous.path)
        && !previous.unitFiles->mount.empty()
        && !previous.unitFiles->automount.empty()
        && g_file_test(previous.unitFiles->mount.c_str(), G_FILE_TEST_EXISTS)
        && g_file_test(previous.unitFiles->automount.c_str(), G_FILE_TEST_EXISTS)) {
      result.push_back(previous);
    }
  }
  return result;
}
