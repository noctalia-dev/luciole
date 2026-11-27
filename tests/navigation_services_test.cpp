#include "browser/browser_controller.h"
#include "browser/path_completion.h"
#include "event_loop/glib_poll_source.h"
#include "fs/file_metadata.h"
#include "fs/gio_interop.h"
#include "fs/location_mount.h"
#include "platform/terminal_launcher.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  struct Fixture {
    fs::path path;
    Fixture() {
      const luciole::GString temp(g_dir_make_tmp("luciole-navigation-XXXXXX", nullptr));
      TEST_CHECK(temp != nullptr);
      path = temp.get();
      fs::create_directory(path / "Alpha");
      fs::create_directory(path / "A space <literal> café");
      fs::create_directory(path / ".hidden");
      fs::create_directory(path / ("raw-" + std::string(1, static_cast<char>(0xff))));
      fs::create_directory_symlink("Alpha", path / "Alias");
      std::ofstream(path / "A file") << "not a folder";
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  void mountInteractionChecks() {
    luciole::MountInteraction interaction;
    unsigned notifications = 0, replies = 0;
    GMountOperationResult result = G_MOUNT_OPERATION_UNHANDLED;
    interaction.setChangedCallback([&] { ++notifications; });
    struct Reply {
      unsigned* count;
      GMountOperationResult* result;
    } reply{&replies, &result};
    g_signal_connect(
        interaction.operation(), "reply", G_CALLBACK(+[](GMountOperation*, GMountOperationResult value, gpointer data) {
          const auto& reply = *static_cast<Reply*>(data);
          ++*reply.count;
          *reply.result = value;
        }),
        &reply
    );
    const auto flags = static_cast<GAskPasswordFlags>(
        G_ASK_PASSWORD_NEED_USERNAME
        | G_ASK_PASSWORD_NEED_PASSWORD
        | G_ASK_PASSWORD_NEED_DOMAIN
        | G_ASK_PASSWORD_SAVING_SUPPORTED
        | G_ASK_PASSWORD_ANONYMOUS_SUPPORTED
    );
    g_signal_emit_by_name(interaction.operation(), "ask-password", "Login <literal>", "user", "WORKGROUP", flags);
    TEST_CHECK(notifications == 1 && interaction.prompt() && interaction.prompt()->message == "Login <literal>");
    const auto first = interaction.prompt()->id;
    TEST_CHECK(!interaction.answer(first + 1, {"user", "domain", "password", false}) && replies == 0);
    TEST_CHECK(!interaction.answer(first, {"", "domain", "password", false}) && replies == 0);
    TEST_CHECK(!interaction.answer(first, {"user", "domain", std::string("bad\0value", 9), false}));
    TEST_CHECK(interaction.answer(first, {"user", "WORKGROUP", "fixture password", false}));
    TEST_CHECK(replies == 1 && result == G_MOUNT_OPERATION_HANDLED && !interaction.prompt());
    TEST_CHECK(std::string_view(g_mount_operation_get_password(interaction.operation())) == "fixture password");
    TEST_CHECK(g_mount_operation_get_password_save(interaction.operation()) == G_PASSWORD_SAVE_FOR_SESSION);
    g_signal_emit_by_name(interaction.operation(), "ask-password", "Try again", "user", "WORKGROUP", flags);
    TEST_CHECK(interaction.prompt() && interaction.prompt()->id != first);
    TEST_CHECK(!interaction.answer(first, {"user", "", "old reply", false}));
    TEST_CHECK(interaction.answer(interaction.prompt()->id, {"", "", "", true}));
    TEST_CHECK(g_mount_operation_get_anonymous(interaction.operation()) && replies == 2);
    TEST_CHECK(std::string_view(g_mount_operation_get_password(interaction.operation())).empty());
    const char* choices[] = {"Accept", "Reject", nullptr};
    g_signal_emit_by_name(interaction.operation(), "ask-question", "Confirm", choices);
    const auto question = interaction.prompt()->id;
    TEST_CHECK(!interaction.choose(question, 2) && interaction.prompt());
    TEST_CHECK(!interaction.answer(question, {"user", "", "", false}));
    TEST_CHECK(interaction.choose(question, 1) && g_mount_operation_get_choice(interaction.operation()) == 1);
    g_signal_emit_by_name(interaction.operation(), "ask-password", "Cancel", "", "", flags);
    interaction.cancel();
    interaction.cancel();
    TEST_CHECK(replies == 4 && result == G_MOUNT_OPERATION_ABORTED && !interaction.prompt());
    g_signal_emit_by_name(interaction.operation(), "ask-password", "Aborted", "", "", flags);
    g_signal_emit_by_name(interaction.operation(), "aborted");
    TEST_CHECK(!interaction.prompt());
    g_signal_emit_by_name(
        interaction.operation(), "ask-password", "Password only", "preset-user", "preset-domain",
        G_ASK_PASSWORD_NEED_PASSWORD
    );
    TEST_CHECK(interaction.answer(interaction.prompt()->id, {"", "", "secret", false}));
    TEST_CHECK(std::string_view(g_mount_operation_get_username(interaction.operation())) == "preset-user");
    TEST_CHECK(std::string_view(g_mount_operation_get_domain(interaction.operation())) == "preset-domain");
    const auto unsupported = static_cast<GAskPasswordFlags>(1U << 20);
    g_signal_emit_by_name(interaction.operation(), "ask-password", "Unsupported", "", "", unsupported);
    TEST_CHECK(!interaction.problem().empty() && replies == 6 && !interaction.prompt());
    auto destroyed = std::make_unique<luciole::MountInteraction>();
    const luciole::GioPtr<GMountOperation> held(G_MOUNT_OPERATION(g_object_ref(destroyed->operation())));
    destroyed->setChangedCallback([&] { destroyed.reset(); });
    g_signal_emit_by_name(held.get(), "ask-password", "Destroy", "", "", flags);
    TEST_CHECK(!destroyed);
  }
  void mountLifetimeChecks() {
    Fixture fixture;
    Pump pump;
    bool obsolete = false, done = false;
    luciole::LocationMount mount;
    const auto native = luciole::Location::fromPath(fixture.path.native());
    mount.start(native, [&](const auto&) { obsolete = true; });
    mount.cancel();
    mount.start(native, [&](const auto& error) {
      TEST_CHECK(!error);
      done = true;
    });
    pump.until([&] { return done; });
    TEST_CHECK(!obsolete);
    auto destroyed = std::make_unique<luciole::LocationMount>();
    destroyed->start(native, [&](const auto&) { obsolete = true; });
    destroyed.reset();
    done = false;
    mount.start(native, [&](const auto& error) {
      TEST_CHECK(!error);
      done = true;
    });
    pump.until([&] { return done; });
    TEST_CHECK(!obsolete);
    GError* error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED, "Not mounted");
    TEST_CHECK(luciole::fileError(*error).kind == luciole::FileErrorKind::NotMounted);
    g_error_free(error);
    const luciole::FileError unsupported{luciole::FileErrorKind::Unsupported, "Provider-specific error"};
    TEST_CHECK(luciole::directoryOpenError(native, unsupported) == unsupported);
    const auto remote = luciole::Location::fromUri("luciole-missing-provider://example.invalid/");
    const luciole::FileError denied{luciole::FileErrorKind::Denied, "Access denied"};
    TEST_CHECK(luciole::directoryOpenError(remote, denied) == denied);
    TEST_CHECK(luciole::directoryOpenError(luciole::Location::trash(), unsupported) == unsupported);
  }
  void remoteMountChecks(const std::string& uri, const std::string& mode) {
    const bool sftp = uri.starts_with("sftp://fixture@192.0.2.1:") || uri.starts_with("ssh://fixture@192.0.2.1:");
    TEST_CHECK(sftp || uri.starts_with("smb://127.0.0.1:"));
    Pump pump;
    auto browser = std::make_unique<luciole::BrowserController>(luciole::SortOptions{}, true);
    const auto target = luciole::Location::fromUri(uri);
    if (mode != "already" && mode != "server") {
      const auto file = luciole::gioFile(target);
      GError* error = nullptr;
      const luciole::GioPtr<GFileInfo> info(
          g_file_query_info(file.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, &error)
      );
      const luciole::GErrorPtr held(error);
      TEST_CHECK(!info && error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED));
    }
    unsigned prompts = 0, passwords = 0;
    browser->setChangedCallback([&] {
      auto* interaction = browser->mountInteraction();
      if (!interaction || !interaction->prompt()) {
        return;
      }
      ++prompts;
      const auto id = interaction->prompt()->id;
      TEST_CHECK(prompts <= 5);
      if (mode == "cancel") {
        interaction->cancel();
      } else if (mode == "navigate") {
        browser->open(luciole::Location::fromPath("/tmp"));
      } else if (mode == "destroy") {
        browser.reset();
      } else if (mode == "reject") {
        TEST_CHECK(sftp && !interaction->prompt()->password && interaction->prompt()->choices.size() == 2);
        TEST_CHECK(interaction->choose(id, 1));
      } else if (!interaction->prompt()->password) {
        TEST_CHECK(sftp && interaction->prompt()->choices.size() == 2);
        TEST_CHECK(interaction->choose(id, 0));
      } else {
        ++passwords;
        const auto* const password = sftp ? "fixture-pass" : "password";
        TEST_CHECK(interaction->answer(
            id,
            mode == "guest" ? luciole::MountCredentials{"", "", "", true}
                            : luciole::MountCredentials{
                                  sftp ? "fixture" : "lysec", sftp ? "" : "WORKGROUP",
                                  passwords == 1 && mode == "retry" ? "wrong" : password, false
                              }
        ));
      }
    });
    browser->open(target);
    pump.until([&] {
      return !browser
          || browser->model().snapshot().status == luciole::DirectoryStatus::Ready
          || browser->model().snapshot().status == luciole::DirectoryStatus::Error
          || browser->model().snapshot().status == luciole::DirectoryStatus::Cancelled;
    });
    if (mode == "destroy") {
      TEST_CHECK(!browser && prompts == 1);
    } else if (mode == "cancel") {
      TEST_CHECK(browser->model().snapshot().status == luciole::DirectoryStatus::Cancelled && prompts == 1);
    } else if (mode == "navigate") {
      TEST_CHECK(browser->model().snapshot().location == luciole::Location::fromPath("/tmp") && prompts == 1);
    } else if (mode == "reject") {
      TEST_CHECK(
          browser->model().snapshot().status == luciole::DirectoryStatus::Error && prompts == 1 && passwords == 0
      );
    } else {
      if (mode == "server") {
        const auto& server = browser->model().snapshot();
        TEST_CHECK(server.status == luciole::DirectoryStatus::Ready);
        const auto found =
            std::ranges::find_if(server.entries, [](const auto& entry) { return entry.name == "guest"; });
        TEST_CHECK(found != server.entries.end() && found->type == luciole::FileType::Mountable);
        bool launched = false;
        browser->setFileActivatedCallback([&](const auto&) { launched = true; });
        browser->activate(found->id);
        pump.until([&] {
          return browser->model().snapshot().status == luciole::DirectoryStatus::Ready
              || browser->model().snapshot().status == luciole::DirectoryStatus::Error;
        });
        TEST_CHECK(!launched);
      }
      const auto& snapshot = browser->model().snapshot();
      if (snapshot.error) {
        std::println(stderr, "Remote mount failed: {}", snapshot.error->message);
      }
      TEST_CHECK(
          snapshot.status == luciole::DirectoryStatus::Ready && (mode == "server" || snapshot.location == target)
      );
      TEST_CHECK(std::ranges::any_of(snapshot.entries, [](const auto& entry) { return entry.name == "fixture.txt"; }));
      TEST_CHECK(mode != "retry" || passwords == 2);
      TEST_CHECK(mode != "key" || passwords == 0);
      if (sftp) {
        const luciole::FileError unsupported{luciole::FileErrorKind::Unsupported, "Provider-specific error"};
        TEST_CHECK(luciole::directoryOpenError(target, unsupported) == unsupported);
      }
    }
    std::println(
        "{} mount {} checks passed ({} prompts, {} passwords)", sftp ? "SFTP" : "SMB", mode, prompts, passwords
    );
  }
  void identityChecks() {
    const luciole::GioPtr<GFileInfo> info(g_file_info_new());
    TEST_CHECK(!luciole::readFileIdentity(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_ID_FILE, "file");
    TEST_CHECK(!luciole::readFileIdentity(info.get()));
    g_file_info_set_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, true);
    TEST_CHECK(!luciole::readFileIdentity(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, "filesystem");
    TEST_CHECK((luciole::readFileIdentity(info.get()) == luciole::FileIdentity{"filesystem", "file"}));
  }
  void completionChecks() {
    Fixture fixture;
    Pump pump;
    luciole::PathCompletion completion;
    const auto base = luciole::Location::fromPath(fixture.path.native());
    luciole::PathSuggestions result;
    bool done = false;
    const auto receive = [&](luciole::PathSuggestions suggestions) {
      result = std::move(suggestions);
      done = true;
    };
    for (const auto* partial : {"admin:/", "admin://", "admin://host/", "sftp://%/"}) {
      done = false;
      completion.complete(partial, base, false, receive);
      TEST_CHECK(done && !result.error.empty() && result.items.empty());
    }
    done = false;
    completion.complete("A", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.error.empty() && result.items.size() == 3);
    for (const auto& item : result.items) {
      TEST_CHECK(luciole::resolveLocationText(item.text, base) == item.location);
    }
    done = false;
    completion.complete(".", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.items.size() == 1 && result.items.front().label == ".hidden");
    done = false;
    completion.complete(base.uri() + "/raw-", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(
        result.items.size() == 1
        && result.items.front().text.starts_with("file:")
        && result.items.front().text.contains("%FF")
    );
    TEST_CHECK(luciole::resolveLocationText(result.items.front().text, base) == result.items.front().location);
    done = false;
    completion.complete("A%20", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.items.empty());
    done = false;
    completion.complete(base.uri() + "/A%20", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.items.size() == 1 && result.items.front().label == "A space <literal> café");
    bool obsolete = false;
    done = false;
    completion.children(base, true, [&](const luciole::PathSuggestions&) { obsolete = true; });
    completion.complete("Alpha", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(!obsolete && result.items.size() == 1);
    done = false;
    completion.complete((fixture.path / "missing").native() + "/", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(!result.error.empty() && result.items.empty());
    for (int i = 0; i < 270; ++i) {
      fs::create_directory(fixture.path / ("many-" + std::to_string(i)));
    }
    done = false;
    completion.complete("many-", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.truncated && result.items.size() == 256);
    const auto home = luciole::Location::fromPath(g_get_home_dir());
    TEST_CHECK(luciole::resolveLocationText("~/", base) == home);
    TEST_CHECK(
        luciole::resolveLocationText("./Alpha", base) == luciole::Location::fromPath((fixture.path / "Alpha").native())
    );
    bool rejected = false;
    try {
      (void)luciole::resolveLocationText(std::string("bad\0name", 8), base);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
    auto destroyed = std::make_unique<luciole::PathCompletion>();
    destroyed->children(base, true, [&](const luciole::PathSuggestions&) { obsolete = true; });
    destroyed.reset();
    done = false;
    completion.children(base, true, receive);
    pump.until([&] { return done; });
    TEST_CHECK(!obsolete);
  }
  void terminalChecks() {
    Fixture fixture;
    Pump pump;
    const char* oldPath = g_getenv("PATH");
    const std::string original = oldPath ? oldPath : "";
    const luciole::GString shell(g_find_program_in_path("sh"));
    TEST_CHECK(shell != nullptr);
    const auto helper = fixture.path / "xdg-terminal-exec";
    const auto log = fixture.path / "terminal.log";
    {
      std::ofstream script(helper);
      script << "#!" << shell.get() << "\nprintf '%s\\0' \"$PWD\" \"$@\" > \"$LUCIOLE_TEST_TERMINAL_LOG\"\n";
    }
    fs::permissions(helper, fs::perms::owner_all);
    g_setenv("PATH", fixture.path.c_str(), true);
    g_setenv("LUCIOLE_TEST_TERMINAL_LOG", log.c_str(), true);
    luciole::TerminalLauncher launcher;
    launcher.setCommand(helper.native());
    g_setenv("PATH", original.c_str(), true);
    TEST_CHECK(launcher.available());
    const auto target = fixture.path / "quotes ' $() ; newline\n日本語";
    fs::create_directory(target);
    bool done = false;
    std::string error;
    const auto receive = [&](std::string message) {
      error = std::move(message);
      done = true;
    };
    launcher.open(luciole::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(error.empty());
    std::ifstream stream(log, std::ios::binary);
    const std::string output{std::istreambuf_iterator<char>(stream), {}};
    TEST_CHECK(output == target.native() + std::string(1, '\0') + "--dir=" + target.native() + std::string(1, '\0'));
    done = false;
    launcher.open(luciole::Location::fromUri("sftp://example.invalid/folder"), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!error.empty());
    done = false;
    launcher.open(luciole::Location::fromPath((fixture.path / "A file").native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!error.empty());
    g_setenv("PATH", "", true);
    luciole::TerminalLauncher missing;
    g_setenv("PATH", original.c_str(), true);
    TEST_CHECK(!missing.available());
    done = false;
    missing.open(luciole::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!error.empty());
    auto destroyed = std::make_unique<luciole::TerminalLauncher>();
    bool called = false;
    destroyed->open(luciole::Location::fromUri("sftp://example.invalid/folder"), [&](const std::string&) {
      called = true;
    });
    destroyed.reset();
    done = false;
    missing.open(luciole::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!called);
    g_unsetenv("LUCIOLE_TEST_TERMINAL_LOG");
  }
} // namespace
int main(int argc, char** argv) {
  if (argc == 4 && (std::string_view(argv[1]) == "--smb" || std::string_view(argv[1]) == "--sftp")) {
    remoteMountChecks(argv[2], argv[3]);
    return 0;
  }
  g_unsetenv("DISPLAY");
  g_unsetenv("WAYLAND_DISPLAY");
  g_unsetenv("WAYLAND_SOCKET");
  g_unsetenv("TERMINAL");
  try {
    mountInteractionChecks();
    mountLifetimeChecks();
    identityChecks();
    completionChecks();
    terminalChecks();
    std::println("navigation services checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "navigation services: {}", error.what());
    return 1;
  }
}
