#include "fs/custom_icons.h"

#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"
#include "fs/system_mounts.h"

#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  // A .directory file holds a few short keys; anything larger is not one.
  constexpr std::size_t kMaxDirectoryFile = 64U * 1024U;

  std::string child(const std::string& folder, std::string_view name) {
    std::string path = folder;
    if (!path.ends_with('/')) {
      path.push_back('/');
    }
    path.append(name);
    return path;
  }

  // A themed name falls back to the plain folder icon when the theme lacks it.
  luciole::FileIconSource themedFolderIcon(std::string name) {
    return {.names = {std::move(name), "folder"}, .filePath = {}, .desktopFile = {}};
  }

  // Icon= is a themed name or an absolute path; relative paths are refused rather than resolved against the folder.
  std::optional<luciole::FileIconSource> iconValue(std::string value) {
    if (value.empty()) {
      return std::nullopt;
    }
    if (value.starts_with('/')) {
      return luciole::FileIconSource{.names = {"folder"}, .filePath = std::move(value), .desktopFile = {}};
    }
    if (value.contains('/')) {
      return std::nullopt;
    }
    return themedFolderIcon(std::move(value));
  }

  std::optional<luciole::FileIconSource> metadataIcon(const std::string& path) {
    const luciole::GioPtr<GFile> file(g_file_new_for_path(path.c_str()));
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        file.get(), "metadata::custom-icon,metadata::custom-icon-name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr,
        nullptr
    ));
    if (!info) {
      return std::nullopt;
    }
    if (const char* uri = g_file_info_get_attribute_string(info.get(), "metadata::custom-icon")) {
      const luciole::GioPtr<GFile> image(g_file_new_for_uri(uri));
      if (const luciole::GString local(g_file_get_path(image.get())); local) {
        return luciole::FileIconSource{.names = {"folder"}, .filePath = local.get(), .desktopFile = {}};
      }
    }
    if (const char* name = g_file_info_get_attribute_string(info.get(), "metadata::custom-icon-name")) {
      return iconValue(name);
    }
    return std::nullopt;
  }

  std::optional<luciole::FileIconSource> directoryFileIcon(const std::string& folder) {
    // O_NOFOLLOW: a .directory that is itself a link is not read.
    const int fd = ::open(child(folder, ".directory").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      return std::nullopt;
    }
    struct stat info{};
    std::string bytes;
    if (::fstat(fd, &info) == 0
        && S_ISREG(info.st_mode)
        && static_cast<std::size_t>(info.st_size) <= kMaxDirectoryFile) {
      bytes.resize(static_cast<std::size_t>(info.st_size));
      std::size_t count = 0;
      while (count < bytes.size()) {
        const auto read = ::read(fd, bytes.data() + count, bytes.size() - count);
        if (read <= 0) {
          break;
        }
        count += static_cast<std::size_t>(read);
      }
      bytes.resize(count);
    }
    ::close(fd);
    if (bytes.empty()) {
      return std::nullopt;
    }
    const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> file(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_data(file.get(), bytes.data(), bytes.size(), G_KEY_FILE_NONE, nullptr)) {
      return std::nullopt;
    }
    const luciole::GString icon(g_key_file_get_string(file.get(), "Desktop Entry", "Icon", nullptr));
    return icon ? iconValue(icon.get()) : std::nullopt;
  }

  luciole::FileIconSource fileTypeIcon(const char* contentType) {
    luciole::FileIconSource result;
    if (contentType != nullptr) {
      const luciole::GioPtr<GIcon> icon(g_content_type_get_icon(contentType));
      result = luciole::readIconSource(icon.get());
    }
    result.names.emplace_back("text-x-generic");
    result.names.emplace_back("application-octet-stream");
    return result;
  }
} // namespace

std::optional<luciole::FileIconSource> luciole::customFolderIcon(const std::string& path) {
  if (auto icon = metadataIcon(path)) {
    return icon;
  }
  return directoryFileIcon(path);
}

std::optional<luciole::FileIconSource>
luciole::linkTargetIcon(const std::string& path, std::span<const MountConfiguration> mounts) {
  struct statx target{};
  if (::statx(AT_FDCWD, path.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE, &target) != 0) {
    return std::nullopt;
  }
  if (S_ISDIR(target.stx_mode)) {
    // GVFS keeps metadata by path: an icon set on the link itself wins, then the one set on its target folder.
    if (auto icon = metadataIcon(path)) {
      return icon;
    }
    const std::unique_ptr<char, decltype(&std::free)> resolved(::realpath(path.c_str(), nullptr), &std::free);
    if (resolved) {
      if (auto icon = metadataIcon(resolved.get())) {
        return icon;
      }
      // Opening .directory inside a configured automount would activate it while listing a link to it.
      if (!mounts.empty() && automountFor(Location::fromPath(resolved.get()), mounts)) {
        return *localFolderIcon();
      }
    }
    if (auto icon = directoryFileIcon(path)) {
      return icon;
    }
    return *localFolderIcon();
  }
  // Following the link, GIO types the target by its own name and, when that is uncertain, its contents.
  const GioPtr<GFile> file(g_file_new_for_path(path.c_str()));
  const GioPtr<GFileInfo> info(
      g_file_query_info(file.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr)
  );
  return fileTypeIcon(info ? infoString(info.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE) : nullptr);
}
