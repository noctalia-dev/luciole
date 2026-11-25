#include "fs/gio_interop.h"
#include "fs/system_mounts.h"
#include "platform/program_launcher.h"
#include "platform/terminal_command.h"
#include "platform/terminal_launcher.h"
#include "test_check.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gio/gdesktopappinfo.h>
#include <nlohmann/json.hpp>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace luciole;
  using Json = nlohmann::json;
  void write(const fs::path& path, const std::string& data, bool executable = false) {
    std::ofstream(path) << data;
    TEST_CHECK(::chmod(path.c_str(), executable ? 0700 : 0600) == 0);
  }
  std::string hex(const std::string& value) {
    std::string result;
    for (const unsigned char byte : value) {
      result += "0123456789abcdef"[byte >> 4U];
      result += "0123456789abcdef"[byte & 15U];
    }
    return result;
  }
  std::vector<Json> records(const fs::path& log) {
    std::vector<Json> result;
    std::ifstream file(log);
    std::string line;
    while (std::getline(file, line)) {
      result.push_back(Json::parse(line));
    }
    return result;
  }
  Json waitRecord(const fs::path& log, std::size_t count) {
    const auto deadline = g_get_monotonic_time() + 5000000;
    while (true) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      auto result = records(log);
      if (result.size() >= count) {
        return result.at(count - 1);
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
  }
  template <class Work> void fails(FileErrorKind kind, Work work) {
    try {
      work();
      TEST_CHECK(false);
    } catch (const FileErrorException& error) {
      TEST_CHECK(error.error().kind == kind);
    }
  }
  struct Fixture {
    fs::path root, log;
    std::string python, recorder;
    Location cwd;
    std::size_t launches = 0;
    Fixture(fs::path path, std::string interpreter)
        : root(std::move(path)), log(root / "launches.jsonl"), python(std::move(interpreter)),
          cwd(Location::fromPath((root / "working").native())) {
      fs::create_directories(root / "working");
      fs::create_directories(root / "bin");
      recorder = "#!"
          + python
          + "\nimport json,os,sys\n"
            "with open(os.environ['NF_EXEC_TEST_LOG'],'a') as f:\n"
            " f.write(json.dumps({'argv':[os.fsencode(v).hex() for v in sys.argv],"
            "'cwd':os.fsencode(os.getcwd()).hex(),'token':os.getenv('XDG_ACTIVATION_TOKEN'),"
            "'startup':os.getenv('DESKTOP_STARTUP_ID')})+'\\n')\n";
    }
    Json
    run(const PreparedProgram& program, const std::vector<Location>& args = {}, bool terminal = false,
        const std::string& token = {}) {
      launchProgram(program, cwd, args, terminal, token, {});
      return waitRecord(log, ++launches);
    }
  };
  void testTerminalPolicy(Fixture& f) {
    const std::string originalPath = g_getenv("PATH");
    const auto bin = f.root / "bin";
    for (const auto* name : {"foot", "kitty", "wezterm", "ghostty", "alacritty", "xdg-terminal-exec"}) {
      write(bin / name, "#!" + f.python + "\n", true);
    }
    g_setenv("PATH", bin.c_str(), true);
    g_setenv("TERMINAL", "kitty", true);
    g_setenv("TERM", "not-a-program", true);
    TEST_CHECK(terminalArguments("foot", true) == (std::vector<std::string>{(bin / "foot").native(), "--"}));
    TEST_CHECK(terminalArguments("", true) == (std::vector<std::string>{(bin / "kitty").native(), "--"}));
    auto literal = terminalArguments("foot --title='space %U $(touch CANARY)'", true);
    TEST_CHECK(literal[1] == "--title=space %U $(touch CANARY)" && literal.back() == "--");
    TEST_CHECK(!fs::exists(f.root / "CANARY"));
    fails(FileErrorKind::Unavailable, [&] { terminalArguments("not-installed", true); });
    fails(FileErrorKind::InvalidArgument, [&] { terminalArguments("foot '", true); });
    fails(FileErrorKind::InvalidArgument, [&] { terminalArguments("foot\nkitty", true); });
    fails(FileErrorKind::InvalidArgument, [&] { terminalArguments(std::string(4097, 'x'), true); });
    g_unsetenv("TERMINAL");
    TEST_CHECK(terminalArguments("", true, "/a b").at(1) == "--dir=/a b");
    fs::remove(bin / "xdg-terminal-exec");
    TEST_CHECK(terminalArguments("", false) == (std::vector<std::string>{(bin / "foot").native()}));
    TEST_CHECK(
        terminalArguments("wezterm", true) == (std::vector<std::string>{(bin / "wezterm").native(), "start", "--"})
    );
    const std::string folder = "/quotes ' % $() ;\n日本語";
    const auto expect = [&](const std::string& command, bool execute, const std::vector<std::string>& suffix) {
      const auto args = terminalArguments(command, execute, folder);
      TEST_CHECK(std::vector<std::string>(args.begin() + 1, args.end()) == suffix);
    };
    expect("wezterm", false, {"start", "--cwd=" + folder});
    expect(
        "wezterm --config-file 'my config.lua' --config 'font_size=12'", true,
        {"--config-file", "my config.lua", "--config", "font_size=12", "start", "--cwd=" + folder, "--"}
    );
    expect("wezterm -n start --new-tab --cwd=/old --", true, {"-n", "start", "--new-tab", "--cwd=" + folder, "--"});
    expect("wezterm -e", true, {"-e", "--cwd=" + folder, "--"});
    expect("wezterm cli spawn", true, {"cli", "spawn", "--cwd=" + folder, "--"});
    expect("wezterm connect server", false, {"connect", "server"});
    expect(
        "ghostty --title='my title' -e", true,
        {"--title=my title", "--working-directory=" + folder, "--gtk-single-instance=false", "-e"}
    );
    expect("ghostty +new-window", false, {"+new-window", "--working-directory=" + folder});
    expect("alacritty --working-directory /old -e", true, {"--working-directory=" + folder, "-e"});
    expect("kitty --", true, {"--directory=" + folder, "--"});
    expect("foot", false, {"--working-directory=" + folder});
    for (const auto* name :
         {"footclient", "gnome-terminal", "kgx", "ptyxis", "mate-terminal", "xfce4-terminal", "konsole"}) {
      write(bin / name, "#!" + f.python + "\n", true);
      const auto* option = std::string(name) == "konsole" ? "--workdir=" : "--working-directory=";
      expect(name, false, {option + folder});
      fs::remove(bin / name);
    }
    fs::remove(bin / "ghostty");
    fs::remove(bin / "alacritty");
    fs::remove(bin / "foot");
    fs::remove(bin / "kitty");
    fs::remove(bin / "wezterm");
    fails(FileErrorKind::Unavailable, [&] { terminalArguments("", true); });
    g_setenv("PATH", originalPath.c_str(), true);

    // A terminal emulator fixture forwards arguments verbatim without a shell.
    write(
        bin / "foot",
        "#!"
            + f.python
            + "\nimport os,sys\na=sys.argv[1:]\n"
              "if a and a[0].startswith('--working-directory='):\n os.chdir(a.pop(0).split('=',1)[1])\n"
              "if not a:\n open("
            + (std::string("\"") + (f.root / "terminal-cwd").native() + "\"")
            + ",'w').write(os.getcwd())\n sys.exit(0)\nassert a.pop(0)=='--'\nos.execv(a[0],a)\n",
        true
    );
    TerminalLauncher terminal;
    terminal.setCommand("missing-luciole-terminal");
    TEST_CHECK(!terminal.available());
    terminal.setCommand("foot");
    TEST_CHECK(terminal.available());
    std::optional<std::string> completed;
    terminal.open(f.cwd, [&](const std::string& error) { completed = error; });
    const auto deadline = g_get_monotonic_time() + 5000000;
    while (!completed) {
      (void)g_main_context_iteration(nullptr, false);
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(completed->empty());
    std::ifstream directory(f.root / "terminal-cwd");
    std::string cwd;
    std::getline(directory, cwd);
    TEST_CHECK(cwd == *f.cwd.nativePath());
    const auto desktopPath = f.root / "terminal % ' launcher.desktop";
    const auto runner = f.root / "terminal-recorder.py";
    write(runner, f.recorder, true);
    write(
        desktopPath,
        "[Desktop Entry]\nType=Application\nName=Terminal fixture\nTerminal=true\nExec="
            + f.python
            + " "
            + runner.native()
            + " %c %k %U\n"
    );
    const GioPtr<GDesktopAppInfo> original(g_desktop_app_info_new_from_filename(desktopPath.c_str()));
    TEST_CHECK(original);
    const auto app = terminalApplication(G_APP_INFO(original.get()), "foot", *f.cwd.nativePath());
    TEST_CHECK(!g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(app.get()), "Terminal"));
    TEST_CHECK(g_desktop_app_info_get_boolean(original.get(), "Terminal"));
    const auto location = Location::fromPath((f.root / "raw-\xff ' % café.txt").native());
    GList* uris = g_list_append(nullptr, const_cast<char*>(location.uri().c_str()));
    TEST_CHECK(g_app_info_launch_uris(app.get(), uris, nullptr, nullptr));
    g_list_free(uris);
    const auto record = waitRecord(f.log, ++f.launches);
    TEST_CHECK(record["argv"][1] == hex("Terminal fixture"));
    TEST_CHECK(record["argv"][2] == hex(Location::fromPath(desktopPath.native()).uri()));
    TEST_CHECK(record["argv"][3] == hex(*location.nativePath()));
    TEST_CHECK(record["cwd"] == hex(*f.cwd.nativePath()));
    const auto explicitDirectory = f.root / "explicit ' % 日本語";
    fs::create_directory(explicitDirectory);
    write(
        desktopPath,
        "[Desktop Entry]\nType=Application\nName=Explicit directory\nTerminal=true\nExec="
            + f.python
            + " "
            + runner.native()
            + "\nPath="
            + explicitDirectory.native()
            + "\n"
    );
    const GioPtr<GDesktopAppInfo> explicitApp(g_desktop_app_info_new_from_filename(desktopPath.c_str()));
    const auto adapted = terminalApplication(G_APP_INFO(explicitApp.get()), "foot", *f.cwd.nativePath());
    TEST_CHECK(g_app_info_launch_uris(adapted.get(), nullptr, nullptr, nullptr));
    TEST_CHECK(waitRecord(f.log, ++f.launches)["cwd"] == hex(explicitDirectory.native()));
    // Restore the captured launcher fixture for the program-launching checks.
    write(
        desktopPath,
        "[Desktop Entry]\nType=Application\nName=Terminal fixture\nTerminal=true\nExec="
            + f.python
            + " "
            + runner.native()
            + " %c %k %U\n"
    );
    const auto nativeProgram = prepareProgram(Location::fromPath(runner.native()), {});
    launchProgram(nativeProgram, f.cwd, {location}, true, "terminal-fresh", {}, "foot");
    const auto nativeRecord = waitRecord(f.log, ++f.launches);
    TEST_CHECK(nativeRecord["argv"][1] == hex(*location.nativePath()) && nativeRecord["token"] == "terminal-fresh");
    const auto desktopProgram = prepareProgram(Location::fromPath(desktopPath.native()), {});
    launchProgram(desktopProgram, f.cwd, {location}, false, "desktop-fresh", {}, "foot");
    const auto desktopRecord = waitRecord(f.log, ++f.launches);
    TEST_CHECK(desktopRecord["argv"][2] == hex(Location::fromPath(desktopPath.native()).uri()));
    TEST_CHECK(desktopRecord["argv"][3] == hex(*location.nativePath()));
    TEST_CHECK(desktopRecord["cwd"] == hex(*f.cwd.nativePath()));
    fs::remove(bin / "foot");
  }
  void testPrograms(Fixture& f) {
    const auto file = f.root / "runner ; $(touch CANARY).py";
    write(file, f.recorder, true);
    const auto location = Location::fromPath(file.native());
    const auto program = prepareProgram(location, {});
    TEST_CHECK(!program.desktop && records(f.log).size() == f.launches);
    const auto raw = (f.root / (std::string("raw-") + static_cast<char>(0xff) + ".txt")).native();
    const auto strange = (f.root / "space café #?% ' $(touch CANARY).txt").native();
    const auto args = std::vector{Location::fromPath(raw), Location::fromPath(strange)};
    const auto result = f.run(program, args, false, "fresh-token");
    TEST_CHECK(result["argv"] == Json::array({hex(file.native()), hex(raw), hex(strange)}));
    TEST_CHECK(result["cwd"] == hex(*f.cwd.nativePath()) && result["token"] == "fresh-token");
    TEST_CHECK(result["startup"].is_null() && !fs::exists(f.root / "working/CANARY"));
    TEST_CHECK(f.run(program)["token"].is_null());
    const auto link = f.root / "program-link";
    fs::create_symlink(file, link);
    TEST_CHECK(f.run(prepareProgram(Location::fromPath(link.native()), {}))["argv"][0] == hex(link.native()));
    const auto helper = f.root / "bin/xdg-terminal-exec";
    write(helper, f.recorder, true);
    const auto terminal = f.run(program, args, true);
    TEST_CHECK(
        terminal["argv"]
        == Json::array(
            {hex(helper.native()), hex("--dir=" + *f.cwd.nativePath()), hex("--"), hex(file.native()), hex(raw),
             hex(strange)}
        )
    );
    const CancelToken cancelled;
    cancelled.cancel();
    fails(FileErrorKind::Cancelled, [&] { (void)prepareProgram(location, cancelled); });
    fails(FileErrorKind::Cancelled, [&] { launchProgram(program, f.cwd, {}, false, {}, cancelled); });
    fails(FileErrorKind::Unsupported, [&] { f.run(program, {Location::fromUri("sftp://example.invalid/file")}); });
    fails(FileErrorKind::InvalidArgument, [&] { f.run(program, std::vector<Location>(4097, location)); });
    write(file, f.recorder + "# changed length\n", true);
    fails(FileErrorKind::Changed, [&] { f.run(program); });
    const auto current = prepareProgram(location, {});
    fs::rename(file, f.root / "old-program");
    write(file, f.recorder, true);
    fails(FileErrorKind::Changed, [&] { f.run(current); });
    const auto linked = prepareProgram(Location::fromPath(link.native()), {});
    fs::remove(link);
    fs::create_symlink(f.root / "old-program", link);
    fails(FileErrorKind::Changed, [&] { f.run(linked); });
  }
  void testApplicationOpening(Fixture& f) {
    const auto binary = f.root / "native application ' % $(touch CANARY) café";
    fs::copy_file("/proc/self/exe", binary);
    TEST_CHECK(::chmod(binary.c_str(), 0700) == 0);
    const auto location = Location::fromPath(binary.native());
    auto prepared = prepareApplication(location, {});
    TEST_CHECK(prepared && !prepared->desktop && records(f.log).size() == f.launches);
    g_setenv("NF_EXEC_BINARY_FIXTURE", "1", true);
    const auto argument = Location::fromPath((f.root / "literal ' % 日本語.txt").native());
    const auto launched = f.run(*prepared, {argument}, false, "binary-open-token");
    TEST_CHECK(launched["argv"] == Json::array({hex(binary.native()), hex(*argument.nativePath())}));
    TEST_CHECK(launched["cwd"] == hex(*f.cwd.nativePath()) && launched["token"] == "binary-open-token");
    TEST_CHECK(launched["startup"].is_null() && !fs::exists(f.root / "working/CANARY"));
    const auto link = f.root / "binary-link";
    fs::create_symlink(binary.filename(), link);
    prepared = prepareApplication(Location::fromPath(link.native()), {});
    TEST_CHECK(prepared && f.run(*prepared)["argv"][0] == hex(link.native()));
    fs::remove(link);
    fs::create_symlink("missing-target", link);
    fails(FileErrorKind::Changed, [&] { f.run(*prepared); });
    TEST_CHECK(::chmod(binary.c_str(), 0600) == 0);
    TEST_CHECK(!prepareApplication(location, {}));
    TEST_CHECK(::chmod(binary.c_str(), 0700) == 0);
    prepared = prepareApplication(location, {});
    TEST_CHECK(prepared);
    fs::rename(binary, f.root / "original-binary");
    write(binary, f.recorder, true);
    fails(FileErrorKind::Changed, [&] { f.run(*prepared); });
    g_unsetenv("NF_EXEC_BINARY_FIXTURE");
    for (const auto* name : {"script.sh", "extensionless-script", "script.AppImage", "launcher.desktop"}) {
      const auto script = f.root / name;
      write(
          script,
          name == std::string_view("launcher.desktop")
              ? "[Desktop Entry]\nType=Application\nName=Fixture\nExec=/bin/false\n"
              : f.recorder,
          true
      );
      TEST_CHECK(!prepareApplication(Location::fromPath(script.native()), {}));
    }
    const auto document = f.root / "document.txt";
    write(document, "ordinary document\n", true);
    TEST_CHECK(!prepareApplication(Location::fromPath(document.native()), {}));
    const auto library = f.root / "library.so";
    fs::copy_file(f.root / "original-binary", library);
    TEST_CHECK(::chmod(library.c_str(), 0700) == 0);
    TEST_CHECK(!prepareApplication(Location::fromPath(library.native()), {}));
    const auto windows = f.root / "windows.exe";
    write(windows, "MZ executable fixture\n", true);
    TEST_CHECK(!prepareApplication(Location::fromPath(windows.native()), {}));
    TEST_CHECK(!prepareApplication(f.cwd, {}));
    TEST_CHECK(!prepareApplication(Location::fromUri("sftp://example.invalid/program"), {}));
    TEST_CHECK(!prepareApplication(Location::trash(), {}));
    const CancelToken cancelled;
    cancelled.cancel();
    fails(FileErrorKind::Cancelled, [&] { (void)prepareApplication(location, cancelled); });
    fails(FileErrorKind::Missing, [&] {
      (void)prepareApplication(Location::fromPath((f.root / "absent-program").native()), {});
    });
  }
  void testRefusals(Fixture& f) {
    const auto file = f.root / "not-executable.py";
    write(file, f.recorder);
    fails(FileErrorKind::Denied, [&] { (void)prepareProgram(Location::fromPath(file.native()), {}); });
    write(file, "plain text, even with the executable bit", true);
    fails(FileErrorKind::Unsupported, [&] { (void)prepareProgram(Location::fromPath(file.native()), {}); });
    write(file, "#!/luciole-execution-test-missing-interpreter\n", true);
    fails(FileErrorKind::Other, [&] { f.run(prepareProgram(Location::fromPath(file.native()), {})); });
    for (const auto& item :
         {Location::fromUri("sftp://example.invalid/file"), Location::trash(), f.cwd,
          Location::fromPath("/dev/null")}) {
      fails(FileErrorKind::Unsupported, [&] { (void)prepareProgram(item, {}); });
    }
    fails(FileErrorKind::Missing, [&] { (void)prepareProgram(Location::fromPath((f.root / "missing").native()), {}); });
  }
  void testStoppedMount(Fixture& f) {
    const auto path = fs::path(*f.cwd.nativePath()) / "program";
    write(path, f.recorder, true);
    const auto location = Location::fromPath(path.native());
    const auto prepared = prepareProgram(location, {});
    publishNativeMounts(
        {{.path = *f.cwd.nativePath(), .source = "nas:/export", .type = "nfs", .options = "", .automount = true}}
    );
    fails(FileErrorKind::NotMounted, [&] { (void)prepareProgram(location, {}); });
    fails(FileErrorKind::NotMounted, [&] { (void)prepareApplication(location, {}); });
    fails(FileErrorKind::NotMounted, [&] { f.run(prepared, {}); });
    publishNativeMounts({});
  }
  void testDesktop(Fixture& f) {
    const auto worker = f.root / "desktop-recorder.py";
    write(worker, f.recorder);
    const auto file = f.root / "launch café %.desktop";
    auto desktop = [&](const std::string& fields) {
      write(file, "[Desktop Entry]\nType=Application\nName=Literal <launcher>\n" + fields);
      return prepareProgram(Location::fromPath(file.native()), {});
    };
    const auto prefix = "Exec=" + f.python + " " + worker.native();
    auto program = desktop(prefix + " %U\nPath=" + *f.cwd.nativePath() + "\n");
    TEST_CHECK(program.desktop && program.name == "Literal <launcher>");
    const auto raw = Location::fromPath((f.root / (std::string("raw-") + static_cast<char>(0xff))).native());
    const auto remote = Location::fromUri("davs://example.invalid/files/caf%C3%A9%20%23.txt");
    auto record = f.run(program, {raw, remote}, false, "desktop-token");
    TEST_CHECK(record["argv"] == Json::array({hex(worker.native()), hex(*raw.nativePath()), hex(remote.uri())}));
    TEST_CHECK(record["token"] == "desktop-token" && record["startup"].is_null());
    program = desktop(prefix + " %F\nPath=" + *f.cwd.nativePath() + "\n");
    record = f.run(program, {raw});
    TEST_CHECK(record["argv"] == Json::array({hex(worker.native()), hex(*raw.nativePath())}));
    program = desktop(prefix + " %k %%k %c\n");
    record = f.run(program);
    TEST_CHECK(
        record["argv"]
        == Json::array(
            {hex(worker.native()), hex(Location::fromPath(file.native()).uri()), hex("%k"), hex(program.name)}
        )
    );
    fails(FileErrorKind::Unsupported, [&] { f.run(program, {}, true); });
    write(file, "[Desktop Entry]\nType=Application\nName=Changed\nExec=/luciole-test-missing\n");
    fails(FileErrorKind::Changed, [&] { f.run(program); });
    for (const auto& text :
         {"not a desktop file", "[Desktop Entry]\nType=Link\nURL=https://example.invalid\n",
          "[Desktop Entry]\nType=Application\nName=Absent\nDBusActivatable=true\n"}) {
      write(file, text);
      bool refused = false;
      try {
        (void)prepareProgram(Location::fromPath(file.native()), {});
      } catch (const FileErrorException&) {
        refused = true;
      }
      TEST_CHECK(refused);
    }
  }
  void testScripts(Fixture& f) {
    TEST_CHECK(listScripts({}).items.empty());
    const auto root = ensureScriptsDirectory({});
    TEST_CHECK(root == scriptsDirectory());
    const fs::path directory(*root.nativePath());
    write(directory / "Normal.py", f.recorder, true);
    write(directory / ".hidden.py", f.recorder, true);
    write(directory / "not-executable", f.recorder);
    write(directory / "unsafe.py", f.recorder, true);
    TEST_CHECK(::chmod((directory / "unsafe.py").c_str(), 0777) == 0);
    fs::create_directory(directory / "subfolder");
    write(f.root / "external.py", f.recorder, true);
    fs::create_symlink(f.root / "external.py", directory / "Linked.py");
    auto listing = listScripts({});
    TEST_CHECK(!listing.error && listing.items.size() == 2 && listing.items[0].name == "Linked.py");
    const auto args = std::vector{Location::fromPath((f.root / "some file.txt").native())};
    TEST_CHECK(f.run(listing.items[0], args)["argv"][1] == hex(*args[0].nativePath()));
    TEST_CHECK(::chmod(directory.c_str(), 0777) == 0);
    fails(FileErrorKind::Denied, [&] { f.run(listing.items[0], args); });
    TEST_CHECK(listScripts({}).error->kind == FileErrorKind::Denied);
    TEST_CHECK(::chmod(directory.c_str(), 0700) == 0);
    listing = listScripts({});
    fs::rename(directory, directory.parent_path() / "old-scripts");
    fs::create_directory(directory);
    fails(FileErrorKind::Changed, [&] { f.run(listing.items[0]); });
    for (int i = 0; i < 70; ++i) {
      write(directory / ("script-" + std::to_string(i)), f.recorder, true);
    }
    listing = listScripts({});
    TEST_CHECK(listing.items.size() == 64 && listing.truncated);
    const CancelToken cancelled;
    cancelled.cancel();
    TEST_CHECK(listScripts(cancelled).error->kind == FileErrorKind::Cancelled);
  }
} // namespace

int main(int argc, char** argv) {
  if (g_getenv("NF_EXEC_BINARY_FIXTURE")) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
      arguments.push_back(hex(argv[index]));
    }
    const auto value = [](const char* name) -> Json {
      const char* text = g_getenv(name);
      return text ? Json(text) : Json(nullptr);
    };
    std::ofstream(g_getenv("NF_EXEC_TEST_LOG"), std::ios::app)
        << Json{{"argv", arguments}, {"cwd", hex(fs::current_path().native())}, {"token", value("XDG_ACTIVATION_TOKEN")}, {"startup", value("DESKTOP_STARTUP_ID")}}
               .dump()
        << '\n';
    return 0;
  }
  char temporary[] = "/tmp/luciole-program-test-XXXXXX";
  TEST_CHECK(mkdtemp(temporary));
  const fs::path root(temporary);
  const luciole::GString python(g_find_program_in_path("python3"));
  TEST_CHECK(python);
  g_setenv("HOME", temporary, true);
  g_setenv("XDG_DATA_HOME", (root / "data").c_str(), true);
  g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), true);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/luciole-program-test-missing-bus", true);
  g_setenv("NF_EXEC_TEST_LOG", (root / "launches.jsonl").c_str(), true);
  g_setenv("XDG_ACTIVATION_TOKEN", "must-not-inherit", true);
  g_setenv("DESKTOP_STARTUP_ID", "must-not-inherit", true);
  const std::string search = (root / "bin").native() + ":" + g_getenv("PATH");
  g_setenv("PATH", search.c_str(), true);
  g_unsetenv("TERMINAL");
  g_unsetenv("GIO_LAUNCH_DESKTOP");
  g_unsetenv("DISPLAY");
  g_unsetenv("WAYLAND_DISPLAY");
  try {
    Fixture fixture(root, python.get());
    testTerminalPolicy(fixture);
    testPrograms(fixture);
    testApplicationOpening(fixture);
    testRefusals(fixture);
    testStoppedMount(fixture);
    testDesktop(fixture);
    testScripts(fixture);
    fs::remove_all(root);
    std::println("program launcher checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    fs::remove_all(root);
    return 1;
  }
}
