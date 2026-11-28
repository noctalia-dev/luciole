#include "fs/directory_session.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <print>
#include <source_location>
#include <thread>
#include <utility>

namespace {
  using namespace luciole;
  enum class RequestKind : std::uint8_t { Listing, Access, Filesystem };
  struct Pending {
    RequestKind kind;
    GioPtr<GTask> task;
    std::string attributes;
  };
  struct Provider {
    std::filesystem::path path;
    std::deque<Pending> pending;
    std::vector<RequestKind> started;
    Provider();
    ~Provider();
    Location location() const { return *Location::fromPath(path.native()).administratorLocation(); }
    GioPtr<GTask> reply(RequestKind kind, std::optional<GIOErrorEnum> failure = std::nullopt);
  };
  struct AdminFile {
    GObject parent;
    Provider* provider;
    char* uri;
  };
  struct AdminFileClass {
    GObjectClass parent;
  };
  void fileInterface(GFileIface* iface);
  // GObject requires these generated C entry-point names.
  // NOLINTBEGIN(readability-identifier-naming)
  G_DEFINE_TYPE_WITH_CODE(AdminFile, admin_file, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_FILE, fileInterface))
  void admin_file_init(AdminFile*) {}
  void admin_file_class_init(AdminFileClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
      g_free(reinterpret_cast<AdminFile*>(object)->uri);
      G_OBJECT_CLASS(admin_file_parent_class)->finalize(object);
    };
  }
  // NOLINTEND(readability-identifier-naming)
  GFile* lookup(GVfs*, const char* uri, gpointer data) {
    auto* file = reinterpret_cast<AdminFile*>(g_object_new(admin_file_get_type(), nullptr));
    file->provider = static_cast<Provider*>(data);
    file->uri = g_strdup(uri);
    return G_FILE(file);
  }
  GioPtr<GFile> backing(GFile* file) {
    const auto* admin = reinterpret_cast<AdminFile*>(file);
    return GioPtr<GFile>(g_file_new_for_uri(("file:" + std::string(admin->uri + 6)).c_str()));
  }
  void request(
      GFile* source, RequestKind kind, const char* attributes, GCancellable* token, GAsyncReadyCallback callback,
      gpointer data
  ) {
    auto& provider = *reinterpret_cast<AdminFile*>(source)->provider;
    GioPtr<GTask> task(g_task_new(source, token, callback, data));
    // Deliver late successes too, so obsolete-request checks belong to the real consumer.
    g_task_set_check_cancellable(task.get(), FALSE);
    provider.started.push_back(kind);
    provider.pending.push_back({kind, std::move(task), attributes});
  }
  void fileInterface(GFileIface* iface) {
    iface->get_uri = +[](GFile* file) { return g_strdup(reinterpret_cast<AdminFile*>(file)->uri); };
    iface->get_uri_scheme = +[](GFile*) { return g_strdup("admin"); };
    iface->has_uri_scheme =
        +[](GFile*, const char* scheme) -> gboolean { return g_ascii_strcasecmp(scheme, "admin") == 0; };
    iface->is_native = +[](GFile*) -> gboolean { return FALSE; };
    iface->enumerate_children_async = +[](GFile* file, const char* attributes, GFileQueryInfoFlags, int,
                                          GCancellable* token, GAsyncReadyCallback callback, gpointer data) {
      request(file, RequestKind::Listing, attributes, token, callback, data);
    };
    iface->enumerate_children_finish = +[](GFile*, GAsyncResult* result, GError** error) {
      return static_cast<GFileEnumerator*>(g_task_propagate_pointer(G_TASK(result), error));
    };
    iface->query_info_async = +[](GFile* file, const char* attributes, GFileQueryInfoFlags, int, GCancellable* token,
                                  GAsyncReadyCallback callback, gpointer data) {
      request(file, RequestKind::Access, attributes, token, callback, data);
    };
    iface->query_info_finish = +[](GFile*, GAsyncResult* result, GError** error) {
      return static_cast<GFileInfo*>(g_task_propagate_pointer(G_TASK(result), error));
    };
    iface->query_filesystem_info_async =
        +[](GFile* file, const char* attributes, int, GCancellable* token, GAsyncReadyCallback callback,
            gpointer data) { request(file, RequestKind::Filesystem, attributes, token, callback, data); };
    iface->query_filesystem_info_finish = +[](GFile*, GAsyncResult* result, GError** error) {
      return static_cast<GFileInfo*>(g_task_propagate_pointer(G_TASK(result), error));
    };
  }
  Provider::Provider() {
    const luciole::GString directory(g_dir_make_tmp("luciole-admin-browse-XXXXXX", nullptr));
    TEST_CHECK(directory);
    path = directory.get();
    std::ofstream(path / "café.txt") << "unchanged contents";
    std::filesystem::create_directory(path / "folder");
    TEST_CHECK(
        g_vfs_register_uri_scheme(g_vfs_get_default(), "admin", lookup, this, nullptr, nullptr, nullptr, nullptr)
    );
  }
  Provider::~Provider() {
    TEST_CHECK(pending.empty());
    TEST_CHECK(g_vfs_unregister_uri_scheme(g_vfs_get_default(), "admin"));
    std::filesystem::remove_all(path);
  }
  GioPtr<GTask> Provider::reply(RequestKind kind, std::optional<GIOErrorEnum> failure) {
    TEST_CHECK(!pending.empty() && pending.front().kind == kind);
    auto current = std::move(pending.front());
    pending.pop_front();
    if (failure) {
      g_task_return_new_error(current.task.get(), G_IO_ERROR, *failure, "Fixture authorization refused");
    } else {
      const auto file = backing(G_FILE(g_task_get_source_object(current.task.get())));
      GError* error = nullptr;
      gpointer result = nullptr;
      switch (kind) {
      case RequestKind::Listing:
        result = g_file_enumerate_children(
            file.get(), current.attributes.c_str(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &error
        );
        break;
      case RequestKind::Access:
        result = g_file_query_info(
            file.get(), current.attributes.c_str(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &error
        );
        break;
      case RequestKind::Filesystem:
        result = g_file_info_new();
        g_file_info_set_attribute_boolean(G_FILE_INFO(result), G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);
        g_file_info_set_attribute_uint64(G_FILE_INFO(result), G_FILE_ATTRIBUTE_FILESYSTEM_FREE, 1234);
        break;
      }
      TEST_CHECK(!error && result);
      g_task_return_pointer(current.task.get(), result, g_object_unref);
    }
    return std::move(current.task);
  }
  void pump(const std::function<bool()>& done, std::source_location caller = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      luciole::test::check(std::chrono::steady_clock::now() < deadline, "pump deadline", caller);
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void finish(Provider& provider, RequestKind kind, std::optional<GIOErrorEnum> failure = std::nullopt) {
    const auto task = provider.reply(kind, failure);
    pump([&] { return g_task_get_completed(task.get()) != FALSE; });
  }
  constexpr RequestKind kSteps[] = {RequestKind::Listing, RequestKind::Access, RequestKind::Filesystem};
  void waitFor(Provider& provider, RequestKind kind) {
    pump([&] { return !provider.pending.empty(); });
    TEST_CHECK(provider.pending.size() == 1 && provider.pending.front().kind == kind);
  }
  void advance(Provider& provider, std::size_t step) {
    for (std::size_t index = 0; index < step; ++index) {
      waitFor(provider, kSteps[index]);
      finish(provider, kSteps[index]);
    }
    waitFor(provider, kSteps[step]);
  }
  void complete(Provider& provider, DirectorySession& session) {
    for (const auto kind : kSteps) {
      waitFor(provider, kind);
      finish(provider, kind);
    }
    pump([&] { return !session.snapshot().metadataLoading; });
    TEST_CHECK(provider.pending.empty());
    const auto& snapshot = session.snapshot();
    TEST_CHECK(snapshot.status == DirectoryStatus::Ready && snapshot.entries.size() == 2);
    TEST_CHECK(!snapshot.error && !snapshot.accessError && !snapshot.filesystemError);
    TEST_CHECK(snapshot.metadata.access.read == true && snapshot.metadata.access.write == true);
    TEST_CHECK(snapshot.metadata.filesystem.readOnly == false && snapshot.metadata.filesystem.freeBytes == 1234);
    TEST_CHECK(std::ranges::any_of(snapshot.entries, [](const auto& entry) {
      return entry.name == "café.txt" && entry.size == 18;
    }));
  }
  void testRefusal() {
    for (const auto error : {G_IO_ERROR_CANCELLED, G_IO_ERROR_PERMISSION_DENIED, G_IO_ERROR_NOT_SUPPORTED}) {
      for (std::size_t step = 0; step < std::size(kSteps); ++step) {
        Provider provider;
        DirectorySession session;
        session.open(provider.location());
        advance(provider, step);
        finish(provider, kSteps[step], error);
        pump([&] {
          return session.snapshot().status != DirectoryStatus::Loading && !session.snapshot().metadataLoading;
        });
        TEST_CHECK(provider.pending.empty() && provider.started.size() == step + 1);
        const auto& snapshot = session.snapshot();
        const auto kind = fileError(*GErrorPtr(g_error_new_literal(G_IO_ERROR, error, "Fixture"))).kind;
        TEST_CHECK(!snapshot.metadataLoading);
        if (step == 0) {
          TEST_CHECK(snapshot.status == DirectoryStatus::Error && snapshot.error->kind == kind);
          TEST_CHECK(snapshot.entries.empty());
        } else {
          TEST_CHECK(snapshot.status == DirectoryStatus::Ready && snapshot.entries.size() == 2);
          TEST_CHECK(snapshot.filesystemError && snapshot.filesystemError->kind == kind);
          if (step == 1) {
            TEST_CHECK(snapshot.accessError && snapshot.accessError->kind == kind);
            TEST_CHECK(!snapshot.metadata.access.read && !snapshot.metadata.filesystem.readOnly);
          }
        }
      }
    }
  }
  void testSuccessAndRetry() {
    Provider provider;
    DirectorySession session;
    session.open(provider.location());
    finish(provider, RequestKind::Listing, G_IO_ERROR_PERMISSION_DENIED);
    TEST_CHECK(session.snapshot().status == DirectoryStatus::Error);
    session.refresh();
    complete(provider, session);
    TEST_CHECK(provider.started.size() == 4);
    session.refresh();
    complete(provider, session);
    TEST_CHECK(provider.started.size() == 7);
  }
  void testCancellationAndDestruction() {
    for (std::size_t step = 0; step < std::size(kSteps); ++step) {
      for (const bool destroy : {false, true}) {
        Provider provider;
        auto session = std::make_unique<DirectorySession>();
        session->open(provider.location());
        advance(provider, step);
        if (destroy) {
          session.reset();
        } else {
          session->cancel();
        }
        finish(provider, kSteps[step]);
        TEST_CHECK(provider.pending.empty() && provider.started.size() == step + 1);
        if (session) {
          TEST_CHECK(session->snapshot().status == DirectoryStatus::Cancelled);
          TEST_CHECK(!session->snapshot().metadataLoading);
        }
      }
    }
  }
  void testObsoleteReplies() {
    for (std::size_t step = 0; step < std::size(kSteps); ++step) {
      Provider provider;
      DirectorySession session;
      session.open(provider.location());
      advance(provider, step);
      session.open(provider.location());
      const auto generation = session.snapshot().generation;
      finish(provider, kSteps[step]);
      TEST_CHECK(session.snapshot().generation == generation && session.snapshot().status == DirectoryStatus::Loading);
      complete(provider, session);
      TEST_CHECK(provider.started.size() == step + 4);
    }
  }
  void testIncompleteListing() {
    Provider provider;
    DirectorySession session(1);
    session.open(provider.location());
    finish(provider, RequestKind::Listing);
    pump([&] { return session.snapshot().status == DirectoryStatus::Error; });
    TEST_CHECK(provider.pending.empty() && provider.started.size() == 1);
    TEST_CHECK(!session.snapshot().metadataLoading && session.snapshot().entries.size() == 1);
  }
} // namespace

int main() {
  testRefusal();
  testSuccessAndRetry();
  testCancellationAndDestruction();
  testObsoleteReplies();
  testIncompleteListing();
  std::println("administrator browsing checks passed");
}
