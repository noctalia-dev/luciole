#include "browser/directory_model.h"
#include "event_loop/glib_poll_source.h"
#include "fs/location_mount.h"
#include "fs/transfer_types.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/provider_item.h"
#include "operations/rename_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_item.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  using namespace luciole;
  namespace fs = std::filesystem;
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string name = "/tmp/luciole-admin-XXXXXX";
      TEST_CHECK(::mkdtemp(name.data()));
      root = name;
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& ready) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!ready()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 2) >= 0);
        m_source.dispatch(fds, first);
      }
    }

  private:
    GlibPollSource m_source;
  };
  template <class Work> void fails(FileErrorKind kind, Work work) {
    bool failed = false;
    try {
      work();
    } catch (const FileErrorException& error) {
      TEST_CHECK(error.error().kind == kind);
      failed = true;
    }
    TEST_CHECK(failed);
  }
  std::string bytes(const Location& location) {
    char* data = nullptr;
    gsize size = 0;
    GError* raw = nullptr;
    TEST_CHECK(g_file_load_contents(gioFile(location).get(), nullptr, &data, &size, nullptr, &raw));
    const luciole::GString owned(data);
    const GErrorPtr error(raw);
    return {data, size};
  }
  void write(GOutputStream* stream, std::string_view bytes) {
    TEST_CHECK(g_output_stream_write_all(stream, bytes.data(), bytes.size(), nullptr, nullptr, nullptr));
  }
  void testLocations() {
    for (const auto& path : {"/", "/usr/share", "/tmp/space # café%?.txt"}) {
      const auto native = Location::fromPath(path);
      const auto admin = *native.administratorLocation();
      TEST_CHECK(admin.isAdmin() && !admin.isNative() && !admin.nativePath());
      TEST_CHECK(admin.unprivilegedLocation() == native);
      TEST_CHECK(transferLocationSupported(admin));
      TEST_CHECK(transferLocationsEqual(native, admin));
      TEST_CHECK(transferLocationsEqual(admin, native));
      TEST_CHECK(!admin.administratorLocation());
    }
    const auto diagnostic =
        directoryOpenError(Location::fromUri("admin:///usr/share"), {FileErrorKind::Unsupported, "Not supported"});
    TEST_CHECK(diagnostic.message.contains("Polkit"));
    const auto parent = Location::fromPath("/tmp/a");
    const auto child = parent.child("b");
    TEST_CHECK(transferLocationInside(*child.administratorLocation(), parent));
    TEST_CHECK(transferLocationInside(child, *parent.administratorLocation()));
    TEST_CHECK(!transferLocationInside(Location::fromPath("/tmp/ab"), *parent.administratorLocation()));
    TEST_CHECK(Location::fromUri("ADMIN:///etc").isAdmin());
    for (const auto* uri : {"sftp://host/etc", "trash:///"}) {
      const auto location = Location::fromUri(uri);
      TEST_CHECK(!location.isAdmin());
      TEST_CHECK(!location.administratorLocation());
      TEST_CHECK(!location.unprivilegedLocation());
    }
    for (const auto* uri : {"admin://host/etc", "admin://user@/etc", "admin:///etc?query", "admin:///etc#part"}) {
      bool rejected = false;
      try {
        (void)Location::fromUri(uri);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      TEST_CHECK(rejected);
    }
  }
  void testProviderPrimitives() {
    Fixture fixture;
    const auto root = Location::fromPath(fixture.root.native());
    const CancelToken token;
    const ProviderItem parent(root, token, true);
    const auto file =
        createProviderItem(parent, "café #.txt", false, token, [](auto* stream) { write(stream, "original"); });
    TEST_CHECK(bytes(file) == "original");
    fails(FileErrorKind::Exists, [&] {
      (void)createProviderItem(parent, "café #.txt", false, token, [](auto* stream) { write(stream, "replacement"); });
    });
    TEST_CHECK(bytes(file) == "original");
    const auto before = std::distance(fs::directory_iterator(fixture.root), fs::directory_iterator{});
    fails(FileErrorKind::NoSpace, [&] {
      (void)createProviderItem(parent, "failed", false, token, [](auto* stream) {
        write(stream, "partial");
        throwFileError(FileErrorKind::NoSpace, "Injected write failure.");
      });
    });
    TEST_CHECK(std::distance(fs::directory_iterator(fixture.root), fs::directory_iterator{}) == before);
    CancelToken cancel;
    fails(FileErrorKind::Cancelled, [&] {
      (void)createProviderItem(parent, "cancelled", false, cancel, [&](auto* stream) {
        write(stream, "partial");
        cancel.cancel();
      });
    });
    TEST_CHECK(!fs::exists(fixture.root / "cancelled"));
    const ProviderItem original(file, token);
    const auto renamed = renameProviderItem(original, "renamed.txt", token);
    TEST_CHECK(bytes(renamed) == "original");
    fails(FileErrorKind::Missing, [&] { original.requireCurrent(token); });
    const ProviderItem changed(renamed, token);
    std::ofstream(fixture.root / "renamed.txt") << "changed bytes";
    fails(FileErrorKind::Changed, [&] { changed.requireCurrent(token); });
    const auto folder = createProviderItem(parent, "folder", true, token, {});
    fs::create_directory_symlink(fixture.root / "folder", fixture.root / "folder-link");
    const auto throughLink = createProviderItem(
        ProviderItem(root.child("folder-link"), token, true), "through-link.txt", false, token,
        [](auto* stream) { write(stream, "linked parent"); }
    );
    TEST_CHECK(bytes(throughLink) == "linked parent");
    fs::create_directories(fixture.root / "folder/nested");
    std::ofstream(fixture.root / "canary") << "outside";
    fs::create_symlink(fixture.root / "canary", fixture.root / "folder/nested/link");
    bool effects = false;
    deleteProviderTree(ProviderItem(folder, token), token, effects);
    TEST_CHECK(effects && !fs::exists(fixture.root / "folder"));
    TEST_CHECK(bytes(root.child("canary")) == "outside");
    bool cancelledEffects = false;
    fails(FileErrorKind::Cancelled, [&] {
      deleteProviderTree(ProviderItem(root.child("canary"), token), cancel, cancelledEffects);
    });
    TEST_CHECK(!cancelledEffects && bytes(root.child("canary")) == "outside");
  }
  void provider(const fs::path& path) {
    TEST_CHECK(::getuid() != 0);
    struct stat fixtureInfo{};
    TEST_CHECK(::stat(path.c_str(), &fixtureInfo) == 0 && fixtureInfo.st_uid == 0 && (fixtureInfo.st_mode & 0022) == 0);
    const auto native = Location::fromPath(path.native());
    const auto root = *native.administratorLocation();
    std::ofstream denied(path / "ordinary-write-must-fail");
    TEST_CHECK(!denied);
    Pump pump;
    LocationMount mount;
    std::optional<std::expected<Location, FileError>> mounted;
    mount.start(root, [&](auto result) { mounted = std::move(result); });
    pump.until([&] { return mounted.has_value(); });
    if (!*mounted) {
      throw std::runtime_error(mounted->error().message);
    }
    TEST_CHECK(bytes(root.child(".admin-access-test")) == "Disposable Luciole administrator test fixture\n");
    const GioPtr<GFileEnumerator> preflight(g_file_enumerate_children(
        gioFile(root).get(), "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(preflight);
    const GioPtr<GFileInfo> marker(g_file_enumerator_next_file(preflight.get(), nullptr, nullptr));
    TEST_CHECK(marker && std::string_view(g_file_info_get_name(marker.get())) == ".admin-access-test");
    TEST_CHECK(!GioPtr<GFileInfo>(g_file_enumerator_next_file(preflight.get(), nullptr, nullptr)));
    TEST_CHECK(g_file_enumerator_close(preflight.get(), nullptr, nullptr));
    CreateService create;
    const auto make = [&](const std::string& name, CreateKind kind) {
      TEST_CHECK(create.prepare(root, kind));
      pump.until([&] { return !create.busy(); });
      TEST_CHECK(create.canCreate());
      TEST_CHECK(create.create(name));
      pump.until([&] { return !create.busy(); });
      if (create.snapshot().error) {
        throw std::runtime_error(create.snapshot().error->message);
      }
      TEST_CHECK(create.snapshot().status == CreateStatus::Succeeded && !create.snapshot().receipt);
    };
    make("created folder", CreateKind::Folder);
    make("empty.txt", CreateKind::EmptyFile);
    TEST_CHECK(bytes(root.child("empty.txt")).empty());
    TEST_CHECK(create.prepareImage(root, "exact image bytes"));
    pump.until([&] { return !create.busy(); });
    TEST_CHECK(create.create("image.bin"));
    pump.until([&] { return !create.busy(); });
    TEST_CHECK(create.snapshot().status == CreateStatus::Succeeded);
    TEST_CHECK(bytes(root.child("image.bin")) == "exact image bytes");
    RenameService rename;
    TEST_CHECK(rename.prepare(root.child("image.bin")));
    pump.until([&] { return !rename.busy(); });
    TEST_CHECK(rename.canRename() && rename.rename("renamed.bin"));
    pump.until([&] { return !rename.busy(); });
    TEST_CHECK(rename.snapshot().status == RenameStatus::Succeeded && !rename.snapshot().receipt);
    TEST_CHECK(bytes(root.child("renamed.bin")) == "exact image bytes");
    TEST_CHECK(rename.prepare(root.child("renamed.bin")));
    pump.until([&] { return !rename.busy(); });
    TEST_CHECK(rename.rename("empty.txt"));
    pump.until([&] { return !rename.busy(); });
    TEST_CHECK(
        rename.snapshot().status == RenameStatus::Error && rename.snapshot().error->kind == FileErrorKind::Exists
    );
    TEST_CHECK(bytes(root.child("renamed.bin")) == "exact image bytes");
    Fixture source;
    std::ofstream(source.root / "upload.txt") << "unprivileged original";
    const auto input = Location::fromPath(source.root.native());
    TransferService transfer;
    transfer.useOperationHistory();
    const auto transferOne = [&](const Location& from, const Location& to, TransferAction action) {
      TEST_CHECK(transfer.start({{from}, to, action}));
      pump.until([&] {
        if (const auto conflict = transfer.conflict()) {
          TEST_CHECK(transfer.resolve(conflict->id, TransferConflictChoice::Replace));
        }
        return !transfer.busy();
      });
      if (transfer.results().front().error) {
        throw std::runtime_error(transfer.results().front().error->message);
      }
      TEST_CHECK(
          transfer.results().front().outcome
          == (action == TransferAction::Move ? TransferOutcome::Moved : TransferOutcome::Copied)
      );
      TEST_CHECK(!transfer.copyReceipt() && !transfer.receipt());
    };
    transferOne(input.child("upload.txt"), root, TransferAction::Copy);
    TEST_CHECK(bytes(root.child("upload.txt")) == "unprivileged original");
    transferOne(root.child("upload.txt"), input, TransferAction::Copy);
    TEST_CHECK(bytes(input.child("upload.txt")) == "unprivileged original");
    transferOne(root.child("renamed.bin"), root.child("created folder"), TransferAction::Move);
    TEST_CHECK(bytes(root.child("created folder/renamed.bin")) == "exact image bytes");
    TEST_CHECK(transfer.start({{root.child("created folder")}, root.child("created folder"), TransferAction::Copy}));
    pump.until([&] { return !transfer.busy(); });
    TEST_CHECK(transfer.results().front().error->kind == FileErrorKind::WouldRecurse);
    TEST_CHECK(transfer.start({{native.child("upload.txt")}, root, TransferAction::Move}));
    pump.until([&] { return !transfer.busy(); });
    TEST_CHECK(transfer.results().front().outcome == TransferOutcome::Skipped);
    GError* linkError = nullptr;
    TEST_CHECK(g_file_make_symbolic_link(gioFile(root.child("alias")).get(), path.c_str(), nullptr, &linkError));
    const GErrorPtr ownedLinkError(linkError);
    transferOne(input.child("upload.txt"), root.child("alias"), TransferAction::Copy);
    TEST_CHECK(create.prepare(root.child("alias"), CreateKind::EmptyFile));
    pump.until([&] { return !create.busy(); });
    TEST_CHECK(create.canCreate() && create.create("alias-empty.txt"));
    pump.until([&] { return !create.busy(); });
    TEST_CHECK(create.snapshot().status == CreateStatus::Succeeded);
    TEST_CHECK(bytes(root.child("alias-empty.txt")).empty());
    TEST_CHECK(
        transfer.start({{root.child("created folder")}, root.child("alias/created folder"), TransferAction::Copy})
    );
    pump.until([&] { return !transfer.busy(); });
    TEST_CHECK(transfer.results().front().error->kind == FileErrorKind::WouldRecurse);
    ProviderItem rootItem(root, CancelToken{}, true);
    const auto nested = createProviderItem(
        ProviderItem(root.child("created folder"), CancelToken{}), "created folder", true, CancelToken{}, {}
    );
    (void)createProviderItem(ProviderItem(nested, CancelToken{}), "canary.txt", false, CancelToken{}, [](auto* stream) {
      write(stream, "never replace a selected source parent");
    });
    TEST_CHECK(transfer.start({{root.child("alias/created folder/created folder")}, root, TransferAction::Copy}));
    pump.until([&] {
      if (const auto conflict = transfer.conflict()) {
        TEST_CHECK(transfer.resolve(conflict->id, TransferConflictChoice::Replace));
      }
      return !transfer.busy();
    });
    TEST_CHECK(transfer.results().front().error->kind == FileErrorKind::WouldRecurse);
    TEST_CHECK(bytes(nested.child("canary.txt")) == "never replace a selected source parent");
    const auto templatePath = source.root / "template.txt";
    std::ofstream(templatePath) << "document template bytes";
    struct stat templateInfo{};
    TEST_CHECK(::stat(templatePath.c_str(), &templateInfo) == 0);
    TEST_CHECK(create.prepareTemplate(
        root, {Location::fromPath(templatePath.native()), "template.txt", "Template", templateRevision(templateInfo)}
    ));
    pump.until([&] { return !create.busy(); });
    TEST_CHECK(create.canCreate() && create.create("document.txt"));
    pump.until([&] { return !create.busy(); });
    TEST_CHECK(create.snapshot().status == CreateStatus::Succeeded);
    TEST_CHECK(bytes(root.child("document.txt")) == "document template bytes");
    TEST_CHECK(!std::ifstream(path / "document.txt"));
    transferOne(root.child("document.txt"), input, TransferAction::Copy);
    TEST_CHECK(bytes(input.child("document.txt")) == "document template bytes");
    struct stat downloaded{};
    TEST_CHECK(::stat((source.root / "document.txt").c_str(), &downloaded) == 0 && downloaded.st_uid == ::getuid());
    TEST_CHECK(create.prepare(root, CreateKind::EmptyFile));
    create.cancel();
    pump.until([&] { return !create.busy(); });
    TEST_CHECK(create.snapshot().status == CreateStatus::Cancelled && !create.canCreate());
    const auto tree = createProviderItem(rootItem, "deletion", true, CancelToken{}, {});
    const ProviderItem treeItem(tree, CancelToken{});
    (void)createProviderItem(treeItem, "body.txt", false, CancelToken{}, [](auto* stream) {
      write(stream, "delete only this");
    });
    DirectoryEntry entry{
        .id = 1, .place = tree, .name = "deletion", .type = FileType::Directory, .size = {}, .modifiedSeconds = {}
    };
    const auto metadata = queryTrashItem(tree, CancelToken{});
    entry.setIdentity(metadata.id);
    entry.setEtag(metadata.etag);
    EmptyTrashService erase;
    TEST_CHECK(erase.prepareDeletion({entry}));
    pump.until([&] { return erase.canEmpty() || (!erase.busy() && erase.snapshot().error); });
    if (erase.snapshot().error) {
      throw std::runtime_error(erase.snapshot().error->message);
    }
    TEST_CHECK(erase.empty());
    pump.until([&] { return !erase.busy(); });
    TEST_CHECK(erase.snapshot().results.front().outcome == EmptyTrashOutcome::Deleted);
    TEST_CHECK(bytes(root.child("upload.txt")) == "unprivileged original");
    std::println("real administrator provider checks passed as uid {}", ::getuid());
  }
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--provider") {
      provider(argv[2]);
      return 0;
    }
    testLocations();
    testProviderPrimitives();
    std::println("administrator access checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "administrator access: {}", error.what());
    return 1;
  }
}
