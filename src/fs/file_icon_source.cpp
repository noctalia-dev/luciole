#include "fs/file_icon_source.h"

#include "fs/gio_interop.h"

#include <functional>

namespace {
  void readIcon(GIcon* icon, luciole::FileIconSource& result) {
    if (icon == nullptr) {
      return;
    }
    if (G_IS_EMBLEMED_ICON(icon)) {
      readIcon(g_emblemed_icon_get_icon(G_EMBLEMED_ICON(icon)), result);
    } else if (G_IS_THEMED_ICON(icon)) {
      const auto* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
      for (std::size_t i = 0; names != nullptr && names[i] != nullptr; ++i) {
        result.names.emplace_back(names[i]);
      }
    } else if (G_IS_FILE_ICON(icon)) {
      const luciole::GString path(g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon))));
      if (path) {
        result.filePath = path.get();
      }
    }
  }
} // namespace

luciole::FileIconSource luciole::readIconSource(GIcon* icon) {
  FileIconSource result;
  readIcon(icon, result);
  return result;
}

namespace {
  // `location` yields the entry's location; it is only resolved for desktop entries.
  luciole::FileIconSource
  fileIcon(GIcon* icon, luciole::FileType type, const char* name, const std::function<luciole::Location()>& location) {
    auto result = luciole::readIconSource(icon);
    result.names.emplace_back(type == luciole::FileType::Directory ? "folder" : "text-x-generic");
    result.names.emplace_back("application-octet-stream");
    if (type == luciole::FileType::Regular && name != nullptr && std::string_view(name).ends_with(".desktop")) {
      if (const auto path = location().nativePath()) {
        result.desktopFile = *path;
      }
    }
    return result;
  }

  bool desktopEntry(luciole::FileType type, const char* name) {
    return type == luciole::FileType::Regular && name != nullptr && std::string_view(name).ends_with(".desktop");
  }
} // namespace

luciole::FileIconSource luciole::readFileIcon(GFileInfo* info, const Location& location) {
  return fileIcon(
      g_file_info_get_icon(info), fileType(g_file_info_get_file_type(info)), g_file_info_get_name(info),
      [&] { return location; }
  );
}

const luciole::FileIconSource& luciole::emptyFileIcon() {
  static const FileIconSource empty;
  return empty;
}

luciole::SharedFileIcon luciole::FileIconPool::get(GFileInfo* info, const Location& folder) {
  const auto location = [&] { return folder.child(g_file_info_get_name(info)); };
  GIcon* icon = g_file_info_get_icon(info);
  const auto type = fileType(g_file_info_get_file_type(info));
  if (desktopEntry(type, g_file_info_get_name(info)) || icon == nullptr || !G_IS_THEMED_ICON(icon)) {
    return std::make_shared<const FileIconSource>(fileIcon(icon, type, g_file_info_get_name(info), location));
  }
  m_key.assign(type == FileType::Directory ? "ID" : "IF");
  const auto* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
  for (std::size_t i = 0; names != nullptr && names[i] != nullptr; ++i) {
    m_key.push_back('\n');
    m_key.append(names[i]);
  }
  if (const auto found = m_icons.find(m_key); found != m_icons.end()) {
    return found->second;
  }
  auto shared = std::make_shared<const FileIconSource>(fileIcon(icon, type, g_file_info_get_name(info), location));
  m_icons.emplace(m_key, shared);
  return shared;
}

void luciole::FileIconPool::setLocalFolder(const char* path) {
  // The path-based names GIO's local backend gives the home and XDG user folders, in its precedence order.
  struct Special {
    const char* path;
    const char* icon;
    bool fallbacks;
  };
  const Special specials[] = {
      {g_get_home_dir(), "user-home", false},
      {g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP), "user-desktop", false},
      {g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS), "folder-documents", true},
      {g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD), "folder-download", true},
      {g_get_user_special_dir(G_USER_DIRECTORY_MUSIC), "folder-music", true},
      {g_get_user_special_dir(G_USER_DIRECTORY_PICTURES), "folder-pictures", true},
      {g_get_user_special_dir(G_USER_DIRECTORY_PUBLIC_SHARE), "folder-publicshare", true},
      {g_get_user_special_dir(G_USER_DIRECTORY_TEMPLATES), "folder-templates", true},
      {g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS), "folder-videos", true},
  };
  m_special.clear();
  if (path == nullptr) {
    return;
  }
  for (const auto& special : specials) {
    if (special.path == nullptr) {
      continue;
    }
    const GString name(g_path_get_basename(special.path));
    const GString child(g_build_filename(path, name.get(), nullptr));
    if (g_strcmp0(child.get(), special.path) == 0) {
      m_special.try_emplace(name.get(), SpecialIcon{special.icon, special.fallbacks});
    }
  }
}

luciole::SharedFileIcon
luciole::FileIconPool::getLocal(const char* name, FileType type, const char* contentType, const Location& folder) {
  const SpecialIcon* special = nullptr;
  if (!m_special.empty() && name != nullptr) {
    if (const auto found = m_special.find(name); found != m_special.end()) {
      special = &found->second;
    }
  }
  // GIO attaches an icon only to an entry whose content type it could determine.
  if (contentType == nullptr) {
    special = nullptr;
  }
  const auto build = [&] {
    GioPtr<GIcon> icon;
    if (special != nullptr) {
      icon.reset(
          special->fallbacks ? g_themed_icon_new_with_default_fallbacks(special->name)
                             : g_themed_icon_new(special->name)
      );
    } else if (contentType != nullptr) {
      icon.reset(g_content_type_get_icon(contentType));
    }
    return std::make_shared<const FileIconSource>(fileIcon(icon.get(), type, name, [&] { return folder.child(name); }));
  };
  if (desktopEntry(type, name)) {
    return build();
  }
  m_key.assign(type == luciole::FileType::Directory ? "LD" : "LF");
  if (special != nullptr) {
    m_key.append("S").append(special->name);
  } else if (contentType != nullptr) {
    m_key.append("T").append(contentType);
  }
  if (const auto found = m_icons.find(m_key); found != m_icons.end()) {
    return found->second;
  }
  auto shared = build();
  m_icons.emplace(m_key, shared);
  return shared;
}
