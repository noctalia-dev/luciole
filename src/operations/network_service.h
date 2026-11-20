#pragma once

#include "fs/system_mounts.h"
#include "operations/network_operation.h"
#include "operations/network_types.h"

namespace luciole {
  // The main process knows configured targets even after systemd unloads an idle unit.
  struct OperationTargets {
    IoLane lane = IoLane::Native;
    std::vector<MountConfiguration> mounts;
    void set(std::span<const Location> locations, std::optional<Location> extra = {});
    void set(std::initializer_list<Location> locations) { set(std::span(locations)); }
    void set(std::span<const DirectoryEntry> entries, bool restoring = false);
    void add(const Location& location);
  };

  struct TransferView {
    TransferAction action = TransferAction::Move;
    std::vector<TransferResult> results;
    std::string report;
    std::uint64_t jobId = 0;
    std::size_t retryable = 0;
    bool historyRetired = false;
    std::shared_ptr<const MoveReceipt> receipt;
    std::shared_ptr<const CopyReceipt> copyReceipt;
  };
  struct RestoreView {
    std::vector<RestoreResult> results;
  };
  struct ArchiveView {
    ArchivePhase phase = ArchivePhase::Idle;
    std::optional<FileError> error;
    std::shared_ptr<const ArchiveCatalog> catalog;
    std::optional<Location> opened;
    std::vector<TransferResult> results;
  };
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TransferView, action, results, report, jobId, retryable, historyRetired)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RestoreView, results)
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArchiveView, phase, error, catalog, opened, results)

  template <typename Snapshot> struct ServiceView {
    Snapshot snapshot;
    bool ready = false, collecting = false;
    std::size_t completed = 0, total = 0;
    std::uint64_t bytes = 0;
  };
  void
  decodeHistory(RenameSnapshot& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process);
  void
  decodeHistory(CreateSnapshot& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process);
  void decodeHistory(
      BatchRenameSnapshot& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process
  );
  void
  decodeHistory(TransferView& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process);
  template <typename Snapshot>
  void decodeHistory(Snapshot&, const NetworkMessage&, const std::shared_ptr<NetworkProcess>&) {}

  void beginRemote(RenameSnapshot&, std::string_view method);
  void beginRemote(CreateSnapshot&, std::string_view method);
  void beginRemote(TrashSnapshot&, std::string_view method);
  void beginRemote(EmptyTrashSnapshot&, std::string_view method);
  void beginRemote(BatchRenameSnapshot&, std::string_view method);
  void beginRemote(PermissionSnapshot&, std::string_view method);
  void beginRemote(TransferView&, std::string_view method);
  void beginRemote(RestoreView&, std::string_view method);
  void beginRemote(ArchiveView&, std::string_view method);
  void failRemote(RenameSnapshot&, FileError error);
  void failRemote(CreateSnapshot&, FileError error);
  void failRemote(TrashSnapshot&, FileError error);
  void failRemote(EmptyTrashSnapshot&, FileError error);
  void failRemote(BatchRenameSnapshot&, FileError error);
  void failRemote(PermissionSnapshot&, FileError error);
  void failRemote(TransferView&, const FileError& error);
  void failRemote(RestoreView&, const FileError& error);
  void failRemote(ArchiveView&, FileError error);

  // Mirrors plain service facts. Filesystem descriptors and history proofs remain in the helper.
  template <typename Snapshot> class NetworkService {
  public:
    explicit NetworkService(std::string name) : m_operation(std::move(name)) {}
    ServiceView<Snapshot> view;
    std::function<void()> changed;
    OperationTargets targets;
    bool busy() const { return m_operation.busy(); }
    void retain(std::shared_ptr<const void> proof) { m_proof = std::move(proof); }
    bool invokeLatest(std::string method, NetworkMessage arguments) {
      if (busy()) {
        m_latest = std::pair{std::move(method), std::move(arguments)};
        beginRemote(view.snapshot, m_latest->first);
        return true;
      }
      return invoke(std::move(method), std::move(arguments));
    }
    void useProcess(std::shared_ptr<NetworkProcess> process) {
      targets = {};
      targets.lane = process->lane() == IoLane::NetworkOperation ? IoLane::Network : IoLane::Native;
      m_operation.useProcess(std::move(process));
    }
    void cancel() {
      m_latest.reset();
      m_cancelled = true;
      m_operation.cancel();
      view.ready = false;
      if constexpr (requires { view.snapshot.canApply; }) {
        view.snapshot.canApply = false;
      }
    }
    bool invoke(std::string method, NetworkMessage arguments = NetworkMessage::object(), bool mutating = false) {
      if (busy()) {
        return false;
      }
      m_cancelled = false;
      beginRemote(view.snapshot, method);
      view.completed = 0;
      view.bytes = 0;
      if (method != "shutdown") {
        for (const auto& mount : targets.mounts) {
          arguments["mounts"].push_back({{"path", mount.path}, {"source", mount.source}, {"type", mount.type}});
        }
      }
      const bool accepted = m_operation.template invoke<ServiceView<Snapshot>>(
          std::move(method), std::move(arguments), targets.lane, mutating,
          [](const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process) {
            ServiceView<Snapshot> result;
            result.snapshot = value.at("snapshot").template get<Snapshot>();
            result.ready = value.at("ready").template get<bool>();
            result.collecting = value.at("collecting").template get<bool>();
            result.completed = value.at("completed").template get<std::size_t>();
            result.total = value.at("total").template get<std::size_t>();
            result.bytes = value.at("bytes").template get<std::uint64_t>();
            decodeHistory(result.snapshot, value, process);
            return result;
          },
          [this](JobResult<ServiceView<Snapshot>> result, bool final) {
            if (result) {
              if constexpr (std::is_same_v<Snapshot, TransferView>) {
                result->snapshot.jobId = view.snapshot.jobId;
              }
              if constexpr (std::is_same_v<Snapshot, ArchiveView>) {
                if (!final) {
                  result->snapshot.catalog = std::move(view.snapshot.catalog);
                }
              } else if constexpr (std::is_same_v<Snapshot, TransferView> || std::is_same_v<Snapshot, RestoreView>) {
                if (result->snapshot.results.empty()) {
                  result->snapshot.results = std::move(view.snapshot.results);
                }
              }
              view = std::move(*result);
            } else {
              view.ready = false;
              failRemote(view.snapshot, std::move(result.error()));
              if constexpr (std::is_same_v<Snapshot, RestoreView>) {
                view.completed = view.total;
              }
            }
            if (m_cancelled) {
              view.ready = false;
              if constexpr (std::is_same_v<Snapshot, EmptyTrashSnapshot> || std::is_same_v<Snapshot, TrashSnapshot>) {
                using Status = decltype(view.snapshot.status);
                if (view.snapshot.status == Status::Ready) {
                  view.snapshot.status = Status::Cancelled;
                }
              }
              if constexpr (requires { view.snapshot.canApply; }) {
                view.snapshot.canApply = false;
              }
            }
            if (final) {
              m_proof.reset();
              if (m_latest) {
                auto next = std::exchange(m_latest, std::nullopt);
                (void)invoke(std::move(next->first), std::move(next->second));
                return;
              }
            }
            notify();
          }
      );
      if (accepted) {
        notify();
      }
      return accepted;
    }
    std::optional<TransferConflict> conflict() const {
      auto value = m_operation.question();
      if (!value || !value->contains("conflict")) {
        return std::nullopt;
      }
      auto result = value->at("conflict").template get<TransferConflict>();
      result.id = value->at("id").template get<std::uint64_t>();
      return result;
    }
    std::optional<ArchivePasswordRequest> password() const {
      auto value = m_operation.question();
      if (!value || !value->contains("password")) {
        return std::nullopt;
      }
      auto result = value->at("password").template get<ArchivePasswordRequest>();
      result.id = value->at("id").template get<std::uint64_t>();
      return result;
    }
    bool answer(std::uint64_t id, NetworkMessage value) { return m_operation.answer(id, std::move(value)); }

  private:
    void notify() const {
      const auto callback = changed;
      if (callback) {
        callback();
      }
    }
    bool m_cancelled = false;
    std::shared_ptr<const void> m_proof;
    std::optional<std::pair<std::string, NetworkMessage>> m_latest;
    NetworkOperation m_operation;
  };
} // namespace luciole
