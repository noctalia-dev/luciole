#include "browser/location_presentation.h"

#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>

std::vector<luciole::Breadcrumb> luciole::locationBreadcrumbs(const Location& location) {
  std::vector<Breadcrumb> result;
  const char* homePath = g_get_home_dir();
  const auto home =
      homePath && g_path_is_absolute(homePath) ? std::optional(Location::fromPath(homePath)) : std::nullopt;
  std::optional<Location> current = location;
  while (current) {
    auto parent = current->parent();
    const bool atHome = home && *current == *home;
    std::string label;
    if (atHome) {
      label = "Home";
    } else if (parent) {
      label = filenameDisplayText(current->basename().value_or(current->uri()));
    } else if (*current == Location::network()) {
      label = "Network";
    } else if (location.isTrash()) {
      label = "Trash";
    } else if (current->isNative()) {
      label = "File System";
    } else {
      const std::unique_ptr<GUri, decltype(&g_uri_unref)> parsed(
          g_uri_parse(current->uri().c_str(), G_URI_FLAGS_ENCODED, nullptr), &g_uri_unref
      );
      const char* host = parsed ? g_uri_get_host(parsed.get()) : nullptr;
      label = host != nullptr && *host != '\0' ? host : "Location";
    }
    result.push_back({.location = *current, .label = std::move(label)});
    if (atHome || (parent && *current == *parent)) {
      break;
    }
    current = std::move(parent);
  }
  std::ranges::reverse(result);
  return result;
}

std::string luciole::locationTitle(const Location& location) {
  if (location.parent()) {
    if (auto name = location.displayName()) {
      return *name;
    }
  }
  return locationBreadcrumbs(location).back().label;
}

std::string luciole::locationEditText(const Location& location) {
  // Virtual discovery folders have no native path; asking GVfs can start discovery just to build a tooltip.
  if (location.isNetworkDiscovery()) {
    return location.uri();
  }
  const auto path = location.nativePath();
  return path && g_utf8_validate(path->c_str(), static_cast<gssize>(path->size()), nullptr) ? *path : location.uri();
}

luciole::Location luciole::resolveLocationText(const std::string& text, const Location& base) {
  if (text.empty() || text.contains('\0')) {
    throw std::invalid_argument("Enter a path or URI without NUL bytes.");
  }
  std::string expanded = text;
  if (text == "~" || text.starts_with("~/")) {
    const char* home = std::getenv("HOME");
    if (!home || !g_path_is_absolute(home)) {
      throw std::invalid_argument("HOME must be an absolute path");
    }
    expanded = std::string(home) + text.substr(1);
  }
  if (g_path_is_absolute(expanded.c_str())) {
    return Location::fromPath(expanded);
  }
  if (const GString scheme(g_uri_parse_scheme(expanded.c_str())); scheme) {
    return Location::fromUri(expanded);
  }
  return base.resolve(expanded);
}
