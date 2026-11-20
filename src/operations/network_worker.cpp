#include "operations/network_worker.h"

#include "operations/history_mount.h"
#include "operations/network_history.h"

#include <fcntl.h>
#include <glib.h>
#include <map>

namespace {
  using namespace luciole;
  using Service = std::variant<
      std::shared_ptr<RenameService>, std::shared_ptr<CreateService>, std::shared_ptr<TrashService>,
      std::shared_ptr<EmptyTrashService>, std::shared_ptr<BatchRenameService>, std::shared_ptr<PermissionService>,
      std::shared_ptr<TransferService>, std::shared_ptr<RestoreService>, std::shared_ptr<ArchiveService>>;
  std::map<std::uint64_t, Service> services;

  Service makeService(const std::string& name, const NetworkMessage& arguments) {
    if (name == "rename") {
      return std::make_shared<RenameService>();
    }
    if (name == "create") {
      return std::make_shared<CreateService>();
    }
    if (name == "trash") {
      return std::make_shared<TrashService>();
    }
    if (name == "emptyTrash") {
      return std::make_shared<EmptyTrashService>();
    }
    if (name == "batchRename") {
      return std::make_shared<BatchRenameService>();
    }
    if (name == "permission") {
      return std::make_shared<PermissionService>();
    }
    if (name == "transfer") {
      return std::make_shared<TransferService>();
    }
    if (name == "restore") {
      return std::make_shared<RestoreService>();
    }
    if (name == "archive") {
      return std::make_shared<ArchiveService>(arguments.value("limits", ArchiveLimits{}));
    }
    throwFileError(FileErrorKind::InvalidArgument, "Unknown filesystem service.");
  }
  template <typename Receipt> std::shared_ptr<const Receipt> receipt(const NetworkMessage& args) {
    const auto& held = workerHistory(args.at("receipt").get<std::uint64_t>());
    if (const auto* value = std::get_if<std::shared_ptr<const Receipt>>(&held)) {
      return *value;
    }
    throwFileError(FileErrorKind::InvalidArgument, "Incorrect filesystem history type.");
  }
  bool invoke(RenameService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "prepare") {
      return service.prepare(args.at("source").get<Location>());
    }
    if (method == "rename") {
      return service.rename(args.at("name").get<std::string>());
    }
    if (method == "replay") {
      return service.replay(receipt<RenameReceipt>(args));
    }
    return false;
  }
  bool invoke(CreateService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "prepare") {
      return service.prepare(args.at("parent").get<Location>(), args.at("kind").get<CreateKind>());
    }
    if (method == "prepareTemplate") {
      return service.prepareTemplate(args.at("parent").get<Location>(), args.at("source").get<DocumentTemplate>());
    }
    if (method == "prepareLink") {
      return service.prepareLink(args.at("source").get<DirectoryEntry>());
    }
    if (method == "createLink") {
      return service.createLink(
          args.at("parent").get<Location>(), args.at("name").get<std::string>(), args.at("kind").get<CreateKind>()
      );
    }
    if (method == "create") {
      return service.create(args.at("name").get<std::string>());
    }
    if (method == "replay") {
      return service.replay(receipt<CreateReceipt>(args));
    }
    return false;
  }
  bool invoke(TrashService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "prepare") {
      auto sources = args.at("sources").get<std::vector<Location>>();
      const auto parent = args.at("parent").get<std::optional<Location>>();
      return parent ? service.prepare(*parent, std::move(sources)) : service.prepare(std::move(sources));
    }
    if (method == "trash") {
      return service.trash();
    }
    return false;
  }
  bool invoke(EmptyTrashService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "prepare") {
      return service.prepare();
    }
    if (method == "prepareSelection") {
      return service.prepareSelection(args.at("entries").get<std::vector<DirectoryEntry>>());
    }
    if (method == "prepareDeletion") {
      return service.prepareDeletion(args.at("entries").get<std::vector<DirectoryEntry>>());
    }
    if (method == "deleteSelection") {
      return service.deleteSelection(args.at("entries").get<std::vector<DirectoryEntry>>());
    }
    if (method == "empty") {
      return service.empty();
    }
    return false;
  }
  bool invoke(BatchRenameService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "prepare") {
      return service.prepare(args.at("entries").get<std::vector<DirectoryEntry>>());
    }
    if (method == "preview") {
      return service.preview(args.at("options").get<BatchRenameOptions>());
    }
    if (method == "apply") {
      return service.apply();
    }
    if (method == "replay") {
      return service.replay(receipt<BatchRenameReceipt>(args));
    }
    return false;
  }
  bool invoke(PermissionService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "prepare") {
      return service.prepare(args.at("entries").get<std::vector<DirectoryEntry>>());
    }
    if (method == "prepareEntry") {
      return service.prepare(args.at("entry").get<DirectoryEntry>());
    }
    if (method == "prepareLocation") {
      return service.prepare(args.at("location").get<Location>());
    }
    if (method == "apply") {
      return service.apply(args.at("edit").get<PermissionEdit>());
    }
    if (method == "applyRecursive") {
      return service.applyRecursive(args.at("edit").get<RecursivePermissionEdit>());
    }
    if (method == "applyAcl") {
      return service.applyAcl(args.at("edit").get<AclEdit>());
    }
    if (method == "applyAclPatch") {
      return service.applyAclPatch(args.at("edit").get<AclPatchEdit>());
    }
    return false;
  }
  bool invoke(TransferService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "start") {
      return service.start(args.at("request").get<TransferRequest>());
    }
    if (method == "retryFailed") {
      return service.retryFailed();
    }
    if (method == "replay") {
      return service.replay(receipt<MoveReceipt>(args));
    }
    if (method == "replayCopy") {
      return service.replayCopy(receipt<CopyReceipt>(args));
    }
    return false;
  }
  bool invoke(RestoreService& service, const std::string& method, const NetworkMessage& args) {
    if (method != "start") {
      return false;
    }
    auto entries = args.at("entries").get<std::vector<DirectoryEntry>>();
    if (const auto destination = args.at("destination").get<std::optional<Location>>()) {
      std::vector<Location> sources;
      sources.reserve(entries.size());
      for (const auto& entry : entries) {
        sources.push_back(entry.location());
      }
      return service.move(std::move(sources), *destination);
    }
    return service.start(std::move(entries));
  }
  bool invoke(ArchiveService& service, const std::string& method, const NetworkMessage& args) {
    if (method == "inspect") {
      return service.inspect(args.at("source").get<Location>());
    }
    if (method == "compress") {
      return service.compress(
          args.at("sources").get<std::vector<Location>>(), args.at("destination").get<Location>(),
          args.at("format").get<ArchiveFormat>(), args.at("password").get<std::string>()
      );
    }
    if (method == "extract") {
      return service.extract(
          args.at("source").get<Location>(), args.at("destination").get<Location>(),
          args.at("selected").get<std::vector<std::string>>(),
          args.at("expected").get<std::shared_ptr<const ArchiveCatalog>>()
      );
    }
    if (method == "shutdown") {
      service.shutdown();
      return true;
    }
    if (method == "openMember") {
      return service.openMember(args.at("path").get<std::string>());
    }
    return false;
  }

  template <typename T> NetworkMessage snapshot(T& service, bool final) {
    NetworkMessage result{
        {"snapshot", service.snapshot()},
        {"ready", false},
        {"collecting", false},
        {"total", 0},
        {"completed", 0},
        {"bytes", 0}
    };
    if constexpr (requires { service.canRename(); }) {
      result["ready"] = service.canRename();
    }
    if constexpr (requires { service.canCreate(); }) {
      result["ready"] = service.canCreate();
    }
    if constexpr (requires { service.canTrash(); }) {
      result["ready"] = service.canTrash();
    }
    if constexpr (requires { service.canEmpty(); }) {
      result["ready"] = service.canEmpty();
    }
    if constexpr (requires { service.completed(); }) {
      result["completed"] = service.completed();
    }
    if constexpr (requires { service.total(); }) {
      result["total"] = service.total();
    }
    if constexpr (requires { service.collecting(); }) {
      result["collecting"] = service.collecting();
    }
    if constexpr (requires { service.snapshot().receipt; }) {
      if (final) {
        result["receipt"] = retainHistory(service.takeReceipt());
      }
    }
    return result;
  }
  NetworkMessage snapshot(TransferService& service, bool final) {
    NetworkMessage result{
        {"snapshot",
         {{"action", service.action()},
          {"results", service.results()},
          {"report", service.report()},
          {"jobId", service.jobId()},
          {"retryable", service.retryable()},
          {"historyRetired", service.historyRetired()}}},
        {"ready", false},
        {"collecting", false},
        {"total", service.total()},
        {"completed", service.completed()},
        {"bytes", service.bytes()}
    };
    if (final) {
      result["receipt"] = retainHistory(service.takeReceipt());
      result["copyReceipt"] = retainHistory(service.takeCopyReceipt());
    }
    return result;
  }
  NetworkMessage snapshot(RestoreService& service, bool) {
    return {
        {"snapshot", {{"results", service.results()}}},
        {"ready", false},
        {"collecting", false},
        {"total", service.total()},
        {"completed", service.completed()},
        {"bytes", 0}
    };
  }
  NetworkMessage snapshot(ArchiveService& service, bool final) {
    return {
        {"snapshot",
         {{"phase", service.phase()},
          {"error", service.error()},
          {"catalog", final ? service.catalog() : nullptr},
          {"opened", final ? service.takeOpenedFile() : std::nullopt},
          {"results", service.results()}}},
        {"ready", false},
        {"collecting", false},
        {"total", service.items()},
        {"completed", 0},
        {"bytes", service.bytes()}
    };
  }

  template <typename T> std::array<std::uint64_t, 3> progress(T& service) {
    std::array<std::uint64_t, 3> value{};
    if constexpr (requires { service.bytes(); }) {
      value[0] = service.bytes();
    }
    if constexpr (requires { service.completed(); }) {
      value[1] = service.completed();
    }
    if constexpr (requires { service.total(); }) {
      value[2] = service.total();
    }
    return value;
  }
  template <typename T> bool question(T& service, const NetworkEvent& event) {
    if constexpr (requires { service.conflict(); }) {
      if (const auto conflict = service.conflict()) {
        auto value = snapshot(service, false);
        value["question"] = {{"conflict", *conflict}};
        const auto answer = event(value);
        const auto choice = answer.value("cancel", false) ? TransferConflictChoice::Cancel
                                                          : answer.at("choice").template get<TransferConflictChoice>();
        if constexpr (requires { service.resolve(conflict->id, choice, false); }) {
          (void)service.resolve(conflict->id, choice, answer.value("applyToAll", false));
        } else {
          (void)service.resolve(conflict->id, choice);
        }
        return true;
      }
    }
    if constexpr (requires { service.passwordRequest(); }) {
      if (const auto password = service.passwordRequest()) {
        auto value = snapshot(service, false);
        value["question"] = {{"password", *password}};
        const auto answer = event(value);
        if (answer.value("cancel", false)) {
          service.cancel();
        } else {
          (void)service.providePassword(password->id, answer.at("password").template get<std::string>());
        }
        return true;
      }
    }
    return false;
  }
} // namespace

luciole::WorkerProgressTimer::WorkerProgressTimer()
    : m_source(g_timeout_add(20, [](gpointer) -> gboolean { return G_SOURCE_CONTINUE; }, nullptr)) {}
luciole::WorkerProgressTimer::~WorkerProgressTimer() { g_source_remove(m_source); }

void luciole::releaseWorkerObjects(const NetworkMessage& request) {
  for (const auto& value : request.value("release", NetworkMessage::array())) {
    const auto id = value.get<std::uint64_t>();
    if (id & (std::uint64_t{1} << 63)) {
      releaseHistory(id);
    } else {
      services.erase(id);
    }
  }
}
luciole::NetworkMessage
luciole::networkOperationRequest(const NetworkMessage& request, const NetworkEvent& event, int descriptor) {
  if (request.at("action") == "historyMount") {
    const auto root = request.at("root").get<Location>();
    if (root.isNative() && descriptor < 0) {
      throwFileError(FileErrorKind::Unavailable, "Missing mount descriptor for history inspection.");
    }
    const HistoryMount mount(
        root, descriptor >= 0 ? luciole::checkedFd(::fcntl(descriptor, F_DUPFD_CLOEXEC, 3)) : luciole::Fd{}
    );
    return std::visit(
        [&](const auto& receipt) {
          if constexpr (requires { receipt->usesMount(mount); }) {
            return receipt->usesMount(mount);
          } else {
            return batchRenameUsesMount(*receipt, mount);
          }
        },
        luciole::workerHistory(request.at("receipt").get<std::uint64_t>())
    );
  }

  if (request.at("action") == "reverse") {
    return reverseHistory(request, event);
  }
  std::vector<Fd> mounted;
  if (const auto roots = request.at("arguments").find("mounts"); roots != request.at("arguments").end()) {
    for (const auto& root : *roots) {
      MountConfiguration config{
          .path = root.at("path").get<std::string>(),
          .source = root.at("source").get<std::string>(),
          .type = root.at("type").get<std::string>(),
          .options = {},
          .automount = true
      };
      const auto location = Location::fromPath(config.path);
      mounted.push_back(openSystemMount(location, CancelToken{}, std::move(config)));
    }
  }
  const auto id = request.at("job").get<std::uint64_t>();
  auto found = services.find(id);
  if (found == services.end()) {
    found = services.emplace(id, makeService(request.at("service").get<std::string>(), request.at("arguments"))).first;
  }
  return std::visit(
      [&](const auto& service) -> NetworkMessage {
        if (networkWorkerCancelled()) {
          throwFileError(FileErrorKind::Cancelled, "Filesystem request cancelled before it started.");
        }
        if (!invoke(*service, request.at("method").get<std::string>(), request.at("arguments"))) {
          throwFileError(FileErrorKind::InvalidArgument, "The filesystem operation is no longer available.");
        }
        const WorkerProgressTimer progressTimer;
        auto changed = std::make_shared<bool>(false);
        service->setChangedCallback([changed] { *changed = true; });
        auto previous = snapshot(*service, false);
        auto sent = std::chrono::steady_clock::now();
        auto lastProgress = progress(*service);
        while (service->busy()) {
          if (networkWorkerCancelled()) {
            service->cancel();
          }
          if (question(*service, event)) {
            sent = std::chrono::steady_clock::now();
          }
          const auto current = progress(*service);
          if ((lastProgress[0] == 0 && current[0] != 0)
              || ((*changed || current != lastProgress)
                  && std::chrono::steady_clock::now() - sent >= std::chrono::milliseconds(50))) {
            *changed = false;
            auto value = snapshot(*service, false);
            if (value != previous) {
              (void)event(value);
              previous = std::move(value);
            }
            sent = std::chrono::steady_clock::now();
            lastProgress = current;
          }
          (void)g_main_context_iteration(nullptr, TRUE);
        }
        while (CopyReceipt::cleanupPending() || CreateReceipt::cleanupPending() || MoveReceipt::cleanupPending()) {
          (void)g_main_context_iteration(nullptr, TRUE);
        }
        auto result = snapshot(*service, true);
        result["cleanupErrors"] = CopyReceipt::takeCleanupErrors();
        return result;
      },
      found->second
  );
}
