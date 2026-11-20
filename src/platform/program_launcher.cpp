#include "platform/program_launcher.h"

#include "fs/filename.h"
#include "fs/gio_interop.h"
#include "fs/network_info.h"
#include "fs/network_types.h"
#include "fs/posix_fd.h"
#include "platform/launch_context.h"
#include "platform/network_program.h"
#include "platform/terminal_command.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gio/gdesktopappinfo.h>
#include <map>
#include <thread>
#include <unistd.h>
#include <utility>

struct luciole::ProgramData {
  Location location;
  Fd descriptor;
  struct stat stamp, pathStamp;
  GioPtr<GDesktopAppInfo> application;
  std::optional<Location> scripts;
  std::optional<struct stat> scriptsStamp;
  std::shared_ptr<NetworkProcess> remote = nullptr;
  std::uint64_t remoteId = 0;
  ~ProgramData() {
    if (remote) {
      remote->discard(remoteId);
    }
  }
};

namespace {
  using namespace luciole;
  std::map<std::uint64_t, PreparedProgram> workerPrograms;
  std::uint64_t nextProgram = std::uint64_t{1} << 62;
  NetworkMessage retainProgram(PreparedProgram value) {
    const auto id = ++nextProgram;
    NetworkMessage result{
        {"id", id},
        {"name", value.name},
        {"command", value.command},
        {"desktop", value.desktop},
        {"uri", value.data->location.uri()}
    };
    workerPrograms.emplace(id, std::move(value));
    return result;
  }
  PreparedProgram remoteProgram(const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process) {
    const struct stat empty{};
    auto data = std::make_shared<ProgramData>(
        Location::fromUri(value.at("uri").get<std::string>()), Fd{}, empty, empty, GioPtr<GDesktopAppInfo>{},
        std::nullopt, std::nullopt
    );
    data->remote = process;
    data->remoteId = value.at("id").get<std::uint64_t>();
    return {
        value.at("name").get<std::string>(), value.at("command").get<std::string>(), value.at("desktop").get<bool>(),
        std::move(data)
    };
  }
  void check(const CancelToken& token) {
    if (token.cancelled()) {
      throwFileError(FileErrorKind::Cancelled, "Launch cancelled.");
    }
  }
  struct stat pathStamp(const Location& location) {
    const auto path = location.nativePath();
    if (!path || location.isTrash()) {
      throwFileError(FileErrorKind::Unsupported, "Running programs requires a native file outside Trash.");
    }
    struct stat stamp{};
    if (::lstat(path->c_str(), &stamp) < 0) {
      throwErrno(errno);
    }
    return stamp;
  }
  bool unchanged(const struct stat& a, const struct stat& b) {
    return sameIdentity(a, b)
        && a.st_mode == b.st_mode
        && a.st_uid == b.st_uid
        && a.st_gid == b.st_gid
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
  }
  struct stat trustedDirectory(const Location& location) {
    const auto path = location.nativePath();
    if (!path) {
      throwFileError(FileErrorKind::Unsupported, "The Scripts folder must be native.");
    }
    const auto fd = checkedFd(::open(path->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto stamp = statFd(fd.get());
    if ((stamp.st_uid != geteuid() && stamp.st_uid != 0) || (stamp.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      throwFileError(
          FileErrorKind::Denied, "The Scripts folder must be owned by you or root and not writable by others."
      );
    }
    return stamp;
  }
  std::string desktopText(int descriptor, const struct stat& stamp) {
    if (stamp.st_size <= 0 || stamp.st_size > 256 * 1024) {
      throwFileError(FileErrorKind::InvalidArgument, "The launcher is empty or exceeds 256 KiB.");
    }
    std::string text(static_cast<std::size_t>(stamp.st_size), '\0');
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto count = ::pread(descriptor, text.data() + offset, text.size() - offset, static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        throwFileError(FileErrorKind::Changed, "The launcher changed while being read.");
      }
      offset += static_cast<std::size_t>(count);
    }
    if (text.contains('\0') || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
      throwFileError(FileErrorKind::InvalidArgument, "The launcher must contain valid UTF-8 text.");
    }
    return text;
  }
  void loadDesktop(PreparedProgram& prepared, ProgramData& data) {
    const auto text = desktopText(data.descriptor.get(), data.stamp);
    const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> key(g_key_file_new(), g_key_file_unref);
    GError* raw = nullptr;
    if (!g_key_file_load_from_data(key.get(), text.data(), text.size(), G_KEY_FILE_NONE, &raw)) {
      const GErrorPtr error(raw);
      throwFileError(FileErrorKind::InvalidArgument, error->message);
    }
    const luciole::GString type(g_key_file_get_string(key.get(), "Desktop Entry", "Type", nullptr));
    const luciole::GString command(g_key_file_get_string(key.get(), "Desktop Entry", "Exec", nullptr));
    if (!type
        || std::string_view(type.get()) != "Application"
        || !command
        || !*command
        || g_key_file_get_boolean(key.get(), "Desktop Entry", "Hidden", nullptr)) {
      throwFileError(FileErrorKind::Unsupported, "Only application launchers with an Exec command can be run.");
    }
    if (std::strlen(command.get()) > 8192) {
      throwFileError(FileErrorKind::InvalidArgument, "The launcher command exceeds 8 KiB.");
    }
    const auto expanded = desktopExecCommand(command.get(), data.location.uri());
    g_key_file_set_string(key.get(), "Desktop Entry", "Exec", expanded.c_str());
    // Launch the captured key file, never reread an untrusted pathname after confirmation.
    data.application.reset(g_desktop_app_info_new_from_keyfile(key.get()));
    if (!data.application) {
      throwFileError(FileErrorKind::InvalidArgument, "The application launcher or its executable is unavailable.");
    }
    prepared.name = g_app_info_get_display_name(G_APP_INFO(data.application.get()));
    prepared.command = command.get();
  }
  void validate(const ProgramData& data, const CancelToken& token) {
    check(token);
    if (data.scripts) {
      (void)trustedDirectory(*data.scripts->parent());
      const auto stamp = trustedDirectory(*data.scripts);
      if (!sameIdentity(stamp, *data.scriptsStamp)) {
        throwFileError(FileErrorKind::Changed, "The Scripts folder changed. Reopen the menu.");
      }
    }
    if (!unchanged(data.stamp, statFd(data.descriptor.get())) || !unchanged(data.pathStamp, pathStamp(data.location))) {
      throwFileError(FileErrorKind::Changed, "The program changed. Select it again before running it.");
    }
    const auto path = *data.location.nativePath();
    const auto current = checkedFd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!unchanged(data.stamp, statFd(current.get()))) {
      throwFileError(FileErrorKind::Changed, "The program target changed. Select it again before running it.");
    }
  }
} // namespace

luciole::PreparedProgram luciole::prepareProgram(const Location& location, const CancelToken& token) {
  if (!inNetworkWorker()) {
    const auto process = NetworkProcess::acquire(operationLane(location));
    auto request = filesystemRequest("program", location);
    request["method"] = "prepare";
    return remoteProgram(process->call(std::move(request), token), process);
  }

  check(token);
  const auto stamp = pathStamp(location);
  if (!S_ISREG(stamp.st_mode) && !S_ISLNK(stamp.st_mode)) {
    throwFileError(FileErrorKind::Unsupported, "Only regular programs and scripts can be run.");
  }
  const auto path = *location.nativePath();
  auto fd = checkedFd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  const auto target = statFd(fd.get());
  if (!S_ISREG(target.st_mode)) {
    throwFileError(FileErrorKind::Unsupported, "Only regular programs and scripts can be run.");
  }
  auto data = std::make_shared<ProgramData>(
      location, std::move(fd), target, stamp, GioPtr<GDesktopAppInfo>{}, std::nullopt, std::nullopt
  );
  const auto name = location.basename().value_or("");
  PreparedProgram result{filenameDisplayText(name), filenameDisplayText(path), name.ends_with(".desktop"), data};
  if (result.desktop) {
    loadDesktop(result, *data);
  } else {
    if (::access(path.c_str(), X_OK) != 0) {
      throwFileError(FileErrorKind::Denied, "Allow executing this file in Properties before running it.");
    }
    std::array<char, 4> header{};
    const auto count = ::pread(data->descriptor.get(), header.data(), header.size(), 0);
    if (count < 2
        || (std::memcmp(header.data(), "#!", 2) != 0
            && (count != 4 || std::memcmp(header.data(), "\177ELF", 4) != 0))) {
      throwFileError(
          FileErrorKind::Unsupported, "This file is not an ELF program or a script with an interpreter line."
      );
    }
  }
  validate(*data, token);
  return result;
}

void luciole::launchProgram(
    const PreparedProgram& program, const Location& workingDirectory, const std::vector<Location>& arguments,
    bool terminal, const std::string& activationToken, const CancelToken& token, const std::string& terminalCommand
) {
  if (program.data && program.data->remote) {
    auto request = filesystemRequest("program", workingDirectory);
    request.update(
        {{"method", "launch"},
         {"object", program.data->remoteId},
         {"arguments", arguments},
         {"terminal", terminal},
         {"activationToken", activationToken},
         {"terminalCommand", terminalCommand}}
    );
    (void)program.data->remote->call(std::move(request), token);
    return;
  }

  if (!program.data || arguments.size() > 4096) {
    throwFileError(FileErrorKind::InvalidArgument, "Invalid program or too many selected files.");
  }
  validate(*program.data, token);
  if (program.data->application) {
    if (terminal) {
      throwFileError(FileErrorKind::Unsupported, "Application launchers choose their own terminal behavior.");
    }
    const auto application = terminalApplication(
        G_APP_INFO(program.data->application.get()), terminalCommand, workingDirectory.nativePath().value_or("")
    );
    GList* uris = nullptr;
    for (const auto& item : arguments) {
      uris = g_list_append(uris, const_cast<char*>(item.uri().c_str())); // Borrowed for this synchronous GIO call.
    }
    const auto context = launchContext(activationToken);
    GError* raw = nullptr;
    const bool ok = g_app_info_launch_uris(application.get(), uris, context.get(), &raw);
    g_list_free(uris);
    const GErrorPtr error(raw);
    if (!ok) {
      throwFileError(FileErrorKind::Other, error ? error->message : "Could not launch the application.");
    }
    return;
  }
  const auto cwd = workingDirectory.nativePath();
  if (!cwd || workingDirectory.isTrash()) {
    throwFileError(FileErrorKind::Unsupported, "Running programs requires a native working folder.");
  }
  std::vector<std::string> argv;
  argv.reserve(arguments.size() + 4);
  if (terminal) {
    argv = terminalArguments(terminalCommand, true, *cwd);
  }
  argv.push_back(*program.data->location.nativePath());
  std::size_t bytes = 0;
  for (const auto& item : arguments) {
    const auto path = item.nativePath();
    if (!path || item.isTrash()) {
      throwFileError(FileErrorKind::Unsupported, "Scripts currently accept native files outside Trash.");
    }
    bytes += path->size() + 1;
    if (bytes > 1024 * 1024) {
      throwFileError(FileErrorKind::InvalidArgument, "The selected paths exceed the script argument limit.");
    }
    argv.push_back(*path);
  }
  const GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE));
  g_subprocess_launcher_set_cwd(launcher.get(), cwd->c_str());
  g_subprocess_launcher_unsetenv(launcher.get(), "XDG_ACTIVATION_TOKEN");
  g_subprocess_launcher_unsetenv(launcher.get(), "DESKTOP_STARTUP_ID");
  if (!activationToken.empty()) {
    g_subprocess_launcher_setenv(launcher.get(), "XDG_ACTIVATION_TOKEN", activationToken.c_str(), true);
  }
  std::vector<const char*> raw;
  raw.reserve(argv.size() + 1);
  for (const auto& argument : argv) {
    raw.push_back(argument.c_str());
  }
  raw.push_back(nullptr);
  check(token);
  GError* error = nullptr;
  const GioPtr<GSubprocess> process(g_subprocess_launcher_spawnv(launcher.get(), raw.data(), &error));
  if (!process) {
    const GErrorPtr owned(error);
    throwFileError(FileErrorKind::Other, owned ? owned->message : "Could not start the program.");
  }
}

luciole::Location luciole::scriptsDirectory() {
  const std::filesystem::path base(g_get_user_data_dir());
  if (!base.is_absolute()) {
    throwFileError(FileErrorKind::InvalidArgument, "The XDG data directory must be absolute.");
  }
  return Location::fromPath((base / "luciole/scripts").native());
}

luciole::IoLane luciole::scriptsLane() {
  // Invalid XDG paths are reported by the script job on its normal error path.
  return g_path_is_absolute(g_get_user_data_dir()) ? operationLane(scriptsDirectory()) : IoLane::Operation;
}

luciole::ScriptListing luciole::listScripts(const CancelToken& token) {
  if (!inNetworkWorker()) {
    try {
      const auto root = scriptsDirectory();
      const auto process = NetworkProcess::acquire(operationLane(root));
      auto request = filesystemRequest("program", root);
      request["method"] = "scripts";
      const auto value = process->call(std::move(request), token);
      ScriptListing result{
          .items = {},
          .error = value.at("error").get<std::optional<FileError>>(),
          .truncated = value.at("truncated").get<bool>()
      };
      for (const auto& item : value.at("items")) {
        result.items.push_back(remoteProgram(item, process));
      }
      return result;
    } catch (...) {
      return {.items = {}, .error = currentFileError(), .truncated = false};
    }
  }

  ScriptListing result;
  try {
    check(token);
    const auto root = scriptsDirectory();
    std::error_code error;
    if (!std::filesystem::exists(*root.nativePath(), error) && !error) {
      return result;
    }
    (void)trustedDirectory(*root.parent());
    const auto stamp = trustedDirectory(root);
    std::size_t inspected = 0;
    for (const auto& entry : std::filesystem::directory_iterator(*root.nativePath())) {
      check(token);
      if (++inspected > 1024 || result.items.size() >= 64) {
        result.truncated = true;
        break;
      }
      if (entry.path().filename().native().starts_with('.')) {
        continue;
      }
      try {
        auto program = prepareProgram(Location::fromPath(entry.path().native()), token);
        const auto& target = program.data->stamp;
        if (program.desktop
            || (target.st_uid != geteuid() && target.st_uid != 0)
            || (target.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
          continue;
        }
        auto data = std::make_shared<ProgramData>(
            program.data->location, checkedFd(::fcntl(program.data->descriptor.get(), F_DUPFD_CLOEXEC, 0)),
            program.data->stamp, program.data->pathStamp, GioPtr<GDesktopAppInfo>{}, root, stamp
        );
        program.data = std::move(data);
        result.items.push_back(std::move(program));
      } catch (const FileErrorException& failure) {
        if (failure.error().kind == FileErrorKind::Cancelled) {
          throw;
        }
        // Non-executable files and unsupported launchers do not become script actions.
      }
    }
    std::ranges::sort(result.items, {}, &PreparedProgram::name);
  } catch (...) {
    result.error = currentFileError();
  }
  return result;
}

luciole::Location luciole::ensureScriptsDirectory(const CancelToken& token) {
  if (!inNetworkWorker()) {
    const auto root = scriptsDirectory();
    auto request = filesystemRequest("program", root);
    request["method"] = "ensureScripts";
    return NetworkProcess::acquire(operationLane(root))->call(std::move(request), token).get<Location>();
  }

  check(token);
  const auto root = scriptsDirectory();
  const auto parent = *root.parent();
  std::filesystem::create_directories(*parent.parent()->nativePath());
  for (const auto& directory : {parent, root}) {
    if (::mkdir(directory.nativePath()->c_str(), 0700) < 0 && errno != EEXIST) {
      throwErrno(errno);
    }
    (void)trustedDirectory(directory);
  }
  check(token);
  return root;
}

void luciole::releaseWorkerPrograms(const NetworkMessage& request) {
  for (const auto& value : request.value("release", NetworkMessage::array())) {
    workerPrograms.erase(value.get<std::uint64_t>());
  }
}
luciole::NetworkMessage luciole::networkProgramRequest(const NetworkMessage& request) {
  const auto method = request.at("method").get<std::string>();
  const CancelToken token;
  const auto location = request.at("uri").get<Location>();
  const auto mount = openRequestMount(request, location, token);
  if (method == "terminal") {
    const auto& folder = location;
    const auto info = isolatedInfo(
        folder, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, G_FILE_QUERY_INFO_NONE, token
    );
    if (g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY
        || (g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)
            && !g_file_info_get_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))) {
      throwFileError(FileErrorKind::Denied, "The terminal location is not an accessible folder.");
    }
    const auto path = folder.nativePath();
    if (!path) {
      throwFileError(FileErrorKind::Unsupported, "Open Terminal Here requires a native folder.");
    }
    auto arguments = terminalArguments(request.at("command").get<std::string>(), false, *path);
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& value : arguments) {
      argv.push_back(value.c_str());
    }
    argv.push_back(nullptr);
    const GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE));
    g_subprocess_launcher_set_cwd(launcher.get(), path->c_str());
    GError* raw = nullptr;
    const GioPtr<GSubprocess> process(g_subprocess_launcher_spawnv(launcher.get(), argv.data(), &raw));
    const GErrorPtr error(raw);
    if (!process) {
      throw FileErrorException(
          error ? fileError(*error) : FileError{FileErrorKind::Unavailable, "Could not launch the terminal."}
      );
    }
    struct Wait {
      bool done = false, success = false;
    };
    auto wait = std::make_shared<Wait>();
    g_subprocess_wait_check_async(
        process.get(), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<std::shared_ptr<Wait>> held(static_cast<std::shared_ptr<Wait>*>(data));
          (*held)->success = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, nullptr);
          (*held)->done = true;
        },
        new std::shared_ptr<Wait>(wait)
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!wait->done && std::chrono::steady_clock::now() < deadline) {
      g_main_context_iteration(nullptr, FALSE);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (wait->done && !wait->success) {
      throwFileError(FileErrorKind::Other, "The terminal exited unsuccessfully.");
    }
    return nullptr;
  }
  if (method == "prepare") {
    return retainProgram(prepareProgram(location, token));
  }
  if (method == "scripts") {
    auto listing = listScripts(token);
    NetworkMessage items = NetworkMessage::array();
    for (auto& value : listing.items) {
      items.push_back(retainProgram(std::move(value)));
    }
    return {{"items", std::move(items)}, {"error", listing.error}, {"truncated", listing.truncated}};
  }
  if (method == "ensureScripts") {
    return ensureScriptsDirectory(token);
  }
  if (method == "launch") {
    const auto found = workerPrograms.find(request.at("object").get<std::uint64_t>());
    if (found == workerPrograms.end()) {
      throwFileError(FileErrorKind::Changed, "The prepared program is no longer available. Select it again.");
    }
    launchProgram(
        found->second, location, request.at("arguments").get<std::vector<Location>>(),
        request.at("terminal").get<bool>(), request.at("activationToken").get<std::string>(), token,
        request.at("terminalCommand").get<std::string>()
    );
    return nullptr;
  }
  throwFileError(FileErrorKind::InvalidArgument, "Unknown program request.");
}
