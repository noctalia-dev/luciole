#include "fs/location.h"

#include "fs/filename.h"
#include "fs/gio_interop.h"

#include <stdexcept>
#include <utility>

namespace {
  bool trashUri(const std::string& uri) { return g_ascii_strncasecmp(uri.c_str(), "trash:", 6) == 0; }

  void rejectNul(const std::string& value) {
    if (value.contains('\0')) {
      throw std::invalid_argument("locations cannot contain a NUL byte");
    }
  }
} // namespace

luciole::GioPtr<GFile> luciole::gioFile(const Location& location) {
  const auto& uri = location.uri();
  if (trashUri(uri)) {
    // Keep GVfs's interned Trash mount alive across UI/worker handle churn.
    static const GioPtr<GFile> root(g_file_new_for_uri("trash:///"));
    if (uri == "trash:///") {
      return GioPtr<GFile>(G_FILE(g_object_ref(root.get())));
    }
  }
  return GioPtr<GFile>(g_file_new_for_uri(uri.c_str()));
}

luciole::Location luciole::locationFromFile(GFile* file) {
  const GString uri(g_file_get_uri(file));
  return Location::fromUri(uri.get());
}

luciole::Location::Location(std::string uri) : m_uri(std::move(uri)) {}

luciole::Location luciole::Location::fromUri(const std::string& uri) {
  rejectNul(uri);
  const GString scheme(g_uri_parse_scheme(uri.c_str()));
  if (!scheme || !g_uri_is_valid(uri.c_str(), G_URI_FLAGS_ENCODED, nullptr)) {
    throw std::invalid_argument("expected an absolute URI");
  }
  const auto file = gioFile(Location(uri));
  const GString canonical(g_file_get_uri(file.get()));
  return Location(canonical.get());
}

luciole::Location luciole::Location::fromPath(const std::string& absolutePath) {
  rejectNul(absolutePath);
  if (!g_path_is_absolute(absolutePath.c_str())) {
    throw std::invalid_argument("expected an absolute native path");
  }
  const GioPtr<GFile> file(g_file_new_for_path(absolutePath.c_str()));
  const GString uri(g_file_get_uri(file.get()));
  return Location(uri.get());
}

luciole::Location luciole::Location::fromCanonicalNativeUri(std::string uri) {
  rejectNul(uri);
  return Location(std::move(uri));
}

std::optional<std::string> luciole::Location::nativePath() const {
  if (isTrash()) {
    return std::nullopt;
  }
  const GString path(g_file_get_path(gioFile(*this).get()));
  return path ? std::optional<std::string>(path.get()) : std::nullopt;
}

luciole::Location luciole::Location::trash() { return Location("trash:///"); }
luciole::Location luciole::Location::network() { return Location("network:///"); }
bool luciole::Location::isNetworkDiscovery() const {
  return m_uri.starts_with("network:") || m_uri.starts_with("dns-sd:") || m_uri.starts_with("wsdd:");
}
bool luciole::Location::isTrash() const { return trashUri(m_uri); }
bool luciole::Location::isNative() const { return g_file_is_native(gioFile(*this).get()) != FALSE; }

std::optional<luciole::Location> luciole::Location::parent() const {
  const GioPtr<GFile> parent(g_file_get_parent(gioFile(*this).get()));
  return parent ? std::optional(locationFromFile(parent.get())) : std::nullopt;
}

luciole::Location luciole::Location::child(const std::string& name) const {
  rejectNul(name);
  const GioPtr<GFile> child(g_file_get_child(gioFile(*this).get(), name.c_str()));
  // GIO's URI of a child already is canonical; listings resolve every entry through here.
  const GString uri(g_file_get_uri(child.get()));
  return Location(uri.get());
}

luciole::Location luciole::Location::resolve(const std::string& relativePath) const {
  rejectNul(relativePath);
  const GioPtr<GFile> file(g_file_resolve_relative_path(gioFile(*this).get(), relativePath.c_str()));
  return locationFromFile(file.get());
}

std::optional<std::string> luciole::Location::basename() const {
  const GString name(g_file_get_basename(gioFile(*this).get()));
  return name ? std::optional<std::string>(name.get()) : std::nullopt;
}

std::optional<std::string> luciole::Location::displayName() const {
  const auto name = basename();
  return name ? std::optional(filenameDisplayText(*name)) : std::nullopt;
}

bool luciole::Location::isInside(const Location& ancestor) const {
  return g_file_has_prefix(gioFile(*this).get(), gioFile(ancestor).get()) != FALSE;
}
