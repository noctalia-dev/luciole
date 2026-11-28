#include "thumbnails/installed_thumbnailers.h"

#include "fs/gio_ptr.h"
#include "imaging/png_decoder.h"
#include "render/core/image_resize.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_store.h"

#include <algorithm>
#include <filesystem>
#include <optional>

namespace {
  namespace fs = std::filesystem;
  using luciole::GString;
  constexpr auto kGroup = "Thumbnailer Entry";
  constexpr auto kResult = "thumbnail.png";
  constexpr auto kOutput = "/output/thumbnail.png";
  constexpr std::size_t kCommandLimit = 64U * 1024U;
  constexpr std::size_t kOutputLimit = 8U * 1024U * 1024U;

  std::vector<std::string> parseCommand(GKeyFile* entry) {
    const GString tryExec(g_key_file_get_string(entry, kGroup, "TryExec", nullptr));
    if (tryExec && !GString(g_find_program_in_path(tryExec.get()))) {
      return {};
    }
    const GString exec(g_key_file_get_string(entry, kGroup, "Exec", nullptr));
    char** args = nullptr;
    int count = 0;
    if (!exec || !g_shell_parse_argv(exec.get(), &count, &args, nullptr) || count == 0) {
      return {};
    }
    const std::unique_ptr<char*, decltype(&g_strfreev)> words(args, g_strfreev);
    const GString program(g_find_program_in_path(args[0]));
    if (!program) {
      return {};
    }
    std::vector<std::string> command(args, args + count);
    std::error_code error;
    command.front() = fs::absolute(program.get(), error).native();
    if (error) {
      return {};
    }
    return command;
  }

  std::string inputPath(std::string_view name) {
    const auto dot = name.rfind('.');
    const auto extension = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
    if (extension.empty() || extension.size() > 32 || !std::ranges::all_of(extension, [](char ch) {
          return g_ascii_isalnum(ch);
        })) {
      return "/input/source";
    }
    return "/input/source." + std::string(extension);
  }

  // Parse quoting before substitution. A filename can never introduce a new argument.
  std::optional<std::vector<std::string>>
  expandCommand(const std::vector<std::string>& command, const std::string& input, const std::string& mime, int side) {
    std::vector<std::string> args{command.front()};
    const GString uri(g_filename_to_uri(input.c_str(), nullptr, nullptr));
    const auto size = std::to_string(side);
    bool hasInput = false, hasOutput = false;
    for (std::size_t index = 1; index < command.size(); ++index) {
      std::string arg;
      const auto& word = command[index];
      for (std::size_t pos = 0; pos < word.size(); ++pos) {
        if (word[pos] != '%') {
          arg += word[pos];
          continue;
        }
        if (++pos == word.size()) {
          return std::nullopt;
        }
        switch (word[pos]) {
        case 'i':
          arg += input;
          hasInput = true;
          break;
        case 'u':
          arg += uri.get();
          hasInput = true;
          break;
        case 'o':
          arg += kOutput;
          hasOutput = true;
          break;
        case 's':
          arg += size;
          break;
        case 'm':
          arg += mime;
          break;
        case '%':
          arg += '%';
          break;
        default:
          return std::nullopt;
        }
      }
      args.push_back(std::move(arg));
    }
    return hasInput && hasOutput ? std::optional(std::move(args)) : std::nullopt;
  }

  constexpr std::string_view kRuntimePaths[]{"/usr",
                                             "/bin",
                                             "/sbin",
                                             "/lib",
                                             "/lib32",
                                             "/lib64",
                                             "/nix/store",
                                             "/run/current-system/sw",
                                             "/etc/ld.so.cache",
                                             "/etc/fonts"};

  std::vector<std::string> sandboxCommand(
      const std::string& sandbox, const std::vector<std::string>& command, const std::string& input,
      const fs::path& output
  ) {
    std::vector<std::string> args{
        sandbox,
        "--unshare-all",
        "--die-with-parent",
        "--clearenv",
        "--dev",
        "/dev",
        "--proc",
        "/proc",
        "--tmpfs",
        "/tmp",
        "--setenv",
        "HOME",
        "/tmp",
        "--setenv",
        "PATH",
        g_getenv("PATH") ? g_getenv("PATH") : "/usr/bin:/bin"
    };
    for (const auto path : kRuntimePaths) {
      args.insert(args.end(), {"--ro-bind-try", std::string(path), std::string(path)});
    }
    const auto& program = command.front();
    const bool inRuntime = std::ranges::any_of(kRuntimePaths, [&](auto path) {
      return std::string_view(program).starts_with(path) && program.size() > path.size() && program[path.size()] == '/';
    });
    if (!inRuntime) {
      args.insert(args.end(), {"--ro-bind", program, program});
    }
    args.insert(args.end(), {"--ro-bind", "/proc/self/fd/3", input, "--bind", output.native(), "/output", "--"});
    args.insert(args.end(), command.begin(), command.end());
    return args;
  }
} // namespace

luciole::InstalledThumbnailers luciole::InstalledThumbnailers::discover() {
  InstalledThumbnailers result;
  if (const GString sandbox(g_find_program_in_path("bwrap")); sandbox) {
    result.m_sandbox = sandbox.get();
  }
  std::vector<fs::path> roots{g_get_user_data_dir()};
  for (const auto* dir = g_get_system_data_dirs(); *dir; ++dir) {
    roots.emplace_back(*dir);
  }
  for (const auto& root : roots) {
    std::vector<fs::path> entries;
    std::error_code error;
    for (fs::directory_iterator iter(root / "thumbnailers", error), end; !error && iter != end; iter.increment(error)) {
      if (iter->path().extension() == ".thumbnailer") {
        entries.push_back(iter->path());
      }
    }
    std::ranges::sort(entries);
    for (const auto& path : entries) {
      const auto resolved = fs::canonical(path, error);
      if (error) {
        continue;
      }
      const auto bytes = readThumbnailFile(resolved, kCommandLimit);
      const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> entry(g_key_file_new(), g_key_file_unref);
      if (!bytes
          || !g_key_file_load_from_data(
              entry.get(), reinterpret_cast<const char*>(bytes->data()), bytes->size(), G_KEY_FILE_NONE, nullptr
          )) {
        continue;
      }
      const std::unique_ptr<char*, decltype(&g_strfreev)> types(
          g_key_file_get_string_list(entry.get(), kGroup, "MimeType", nullptr, nullptr), g_strfreev
      );
      if (types) {
        const auto command = parseCommand(entry.get());
        if (!command.empty()) {
          for (auto* type = types.get(); *type; ++type) {
            result.m_commands.try_emplace(*type, command);
          }
        }
      }
    }
  }
  return result;
}

std::expected<LoadedImageFile, std::string> luciole::InstalledThumbnailers::render(
    int sourceFd, const std::string& mime, std::string_view name, int side, GCancellable* cancel
) const {
  auto found = m_commands.find(mime);
  if (found == m_commands.end() && !name.empty()) {
    const GString guessed(g_content_type_guess(std::string(name).c_str(), nullptr, 0, nullptr));
    if (guessed) {
      found = m_commands.find(guessed.get());
    }
  }
  if (found == m_commands.end()) {
    return std::unexpected("no installed thumbnailer for this MIME type");
  }
  if (m_sandbox.empty()) {
    return std::unexpected("installed thumbnailers require Bubblewrap");
  }
  const auto input = inputPath(name);
  const auto command = expandCommand(found->second, input, found->first, side);
  if (!command) {
    return std::unexpected("invalid thumbnailer command");
  }
  const GString temporary(g_dir_make_tmp("luciole-thumbnailer-XXXXXX", nullptr));
  if (!temporary) {
    return std::unexpected("cannot prepare installed thumbnailer");
  }
  const fs::path directory(temporary.get());
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{directory};
  if (auto result =
          runProviderProcess(sandboxCommand(m_sandbox, *command, input, directory), sourceFd, cancel, kOutputLimit, 1);
      !result) {
    return std::unexpected(result.error());
  }
  const auto bytes = readThumbnailFile(directory / kResult, kOutputLimit);
  if (!bytes) {
    return std::unexpected("cannot read thumbnailer output");
  }
  auto decoded = decodePng(*bytes);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  LoadedImageFile image{.rgba = std::move(decoded->pixels), .width = decoded->width, .height = decoded->height};
  const auto size = fitImageWithin(image.width, image.height, side, side);
  if (size.width != image.width || size.height != image.height) {
    image.rgba =
        resizeToRgba(image.rgba.data(), image.width, image.height, ResizeSourceLayout::Rgba, size.width, size.height);
    image.width = size.width;
    image.height = size.height;
    if (image.rgba.empty()) {
      return std::unexpected("cannot resize thumbnailer output");
    }
  }
  return image;
}
