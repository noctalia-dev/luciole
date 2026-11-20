#include "operations/network_history.h"

#include "operations/history_mount.h"
#include "operations/network_worker.h"

#include <exception>
#include <glib.h>
#include <map>

namespace {
  std::map<std::uint64_t, luciole::WorkerHistory> receipts;
  std::uint64_t nextReceipt = std::uint64_t{1} << 63;

  std::shared_ptr<luciole::NetworkHistory>
  handle(const luciole::NetworkMessage& value, const std::shared_ptr<luciole::NetworkProcess>& process) {
    return std::shared_ptr<luciole::NetworkHistory>(
        new luciole::NetworkHistory{process, value.at("id").get<std::uint64_t>()}
    );
  }
  class Rename final : public luciole::RenameReceipt {
  public:
    Rename(const luciole::NetworkMessage& value, const std::shared_ptr<luciole::NetworkProcess>& process)
        : RenameReceipt(
              value.at("source").get<luciole::Location>(), value.at("destination").get<luciole::Location>(), {}
          ),
          history(handle(value, process)) {}
    bool usesMount(const luciole::HistoryMount& mount) const override { return history->usesMount(mount); }
    std::shared_ptr<luciole::NetworkHistory> history;
    std::shared_ptr<const luciole::RenameReceipt> reverse(const luciole::CancelToken& token) const override {
      luciole::RenameSnapshot snapshot;
      luciole::decodeHistory(snapshot, history->reverse(token), history->process);
      return snapshot.receipt;
    }
  };
  class Create final : public luciole::CreateReceipt {
  public:
    Create(const luciole::NetworkMessage& value, const std::shared_ptr<luciole::NetworkProcess>& process)
        : CreateReceipt(
              value.at("kind").get<luciole::CreateKind>(), value.at("destination").get<luciole::Location>(), {}
          ),
          history(handle(value, process)) {}
    bool usesMount(const luciole::HistoryMount& mount) const override { return history->usesMount(mount); }
    std::shared_ptr<luciole::NetworkHistory> history;
    luciole::CreateReplayResult reverse(const luciole::CancelToken& token) const override {
      const auto value = history->reverse(token);
      luciole::CreateSnapshot snapshot;
      luciole::decodeHistory(snapshot, value, history->process);
      return {
          .receipt = std::move(snapshot.receipt),
          .error = value.at("error").get<std::optional<luciole::FileError>>(),
          .applied = value.at("applied").get<bool>(),
          .removed = value.at("removed").get<bool>(),
          .retireHistory = value.at("retireHistory").get<bool>()
      };
    }
  };
  struct TransferHistory {
    TransferHistory(const luciole::NetworkMessage& value, const std::shared_ptr<luciole::NetworkProcess>& process)
        : owner(handle(value, process)), count(value.at("size").get<std::size_t>()),
          locations(value.at("locations").get<std::vector<luciole::TransferResult>>()) {}

    std::vector<luciole::TransferResult> failedLocations(const luciole::FileError& error) const {
      auto results = locations;
      for (auto& item : results) {
        item.error = error;
      }
      return results;
    }

    luciole::TransferView reverse(const luciole::CancelToken& token, std::atomic<std::size_t>& completed) const {
      luciole::TransferView result;
      if (token.cancelled()) {
        result.results = failedLocations({luciole::FileErrorKind::Cancelled, "Undo cancelled."});
        for (auto& item : result.results) {
          item.outcome = luciole::TransferOutcome::Cancelled;
        }
        completed = count;
        return result;
      }
      const auto reply = owner->reverse(token, &completed);
      luciole::decodeHistory(result, reply, owner->process);
      completed = reply.at("results").size();
      reply.at("results").get_to(result.results);
      reply.at("retireHistory").get_to(result.historyRetired);
      return result;
    }

    std::shared_ptr<luciole::NetworkHistory> owner;
    std::size_t count;
    std::vector<luciole::TransferResult> locations;
  };

  class Move final : public luciole::MoveReceipt {
  public:
    Move(const luciole::NetworkMessage& value, const std::shared_ptr<luciole::NetworkProcess>& process)
        : history(value, process) {}
    bool usesMount(const luciole::HistoryMount& mount) const override { return history.owner->usesMount(mount); }
    TransferHistory history;
    std::size_t size() const override { return history.count; }
    std::vector<luciole::TransferResult> failedLocations(const luciole::FileError& error) const override {
      return history.failedLocations(error);
    }
    luciole::MoveReplayResult
    reverse(const luciole::CancelToken& token, std::atomic<std::size_t>& completed) const override {
      auto result = history.reverse(token, completed);
      return {std::move(result.results), std::move(result.receipt), result.historyRetired};
    }
  };
  class Copy final : public luciole::CopyReceipt {
  public:
    Copy(const luciole::NetworkMessage& value, const std::shared_ptr<luciole::NetworkProcess>& process)
        : CopyReceipt(value.at("action").get<luciole::TransferAction>()), history(value, process) {}
    bool usesMount(const luciole::HistoryMount& mount) const override { return history.owner->usesMount(mount); }
    TransferHistory history;
    std::size_t size() const override { return history.count; }
    std::vector<luciole::TransferResult> failedLocations(const luciole::FileError& error) const override {
      return history.failedLocations(error);
    }
    luciole::CopyReplayResult
    reverse(const luciole::CancelToken& token, std::atomic<std::size_t>& completed) const override {
      auto result = history.reverse(token, completed);
      return {std::move(result.results), std::move(result.copyReceipt), result.historyRetired};
    }
  };
  const luciole::FileError unconfirmed{
      luciole::FileErrorKind::Changed, "The previous operation could not be confirmed."
  };
} // namespace

bool luciole::NetworkHistory::usesMount(const HistoryMount& mount) const {
  return process
      ->call(
          {{"action", "historyMount"}, {"receipt", id}, {"root", mount.root()}}, CancelToken{}, {},
          std::chrono::seconds(10), mount.descriptor()
      )
      .get<bool>();
}
luciole::NetworkHistory::~NetworkHistory() { process->discard(id, true); }
luciole::NetworkMessage
luciole::NetworkHistory::reverse(const CancelToken& token, std::atomic<std::size_t>* completed) const {
  if (token.cancelled()) {
    throwFileError(FileErrorKind::Cancelled, "Undo cancelled.");
  }
  try {
    return process->call(
        {{"action", "reverse"}, {"receipt", id}, {"mutating", true}}, token, [completed](const NetworkMessage& value) {
          if (completed) {
            *completed = value.at("completed").get<std::size_t>();
          }
          return NetworkMessage{};
        }
    );
  } catch (...) {
    auto error = currentFileError();
    if (error.kind == FileErrorKind::Unavailable || error.kind == FileErrorKind::Cancelled) {
      error = {
          FileErrorKind::Changed,
          "Undo was interrupted. Changes may be incomplete; refresh the affected folders before retrying."
      };
    }
    throw FileErrorException(std::move(error));
  }
}

std::shared_ptr<luciole::NetworkHistory> luciole::historyHandle(const std::shared_ptr<const RenameReceipt>& value) {
  const auto* remote = dynamic_cast<const Rename*>(value.get());
  return remote ? remote->history : nullptr;
}
std::shared_ptr<luciole::NetworkHistory> luciole::historyHandle(const std::shared_ptr<const CreateReceipt>& value) {
  const auto* remote = dynamic_cast<const Create*>(value.get());
  return remote ? remote->history : nullptr;
}
std::shared_ptr<luciole::NetworkHistory> luciole::historyHandle(const std::shared_ptr<const MoveReceipt>& value) {
  const auto* remote = dynamic_cast<const Move*>(value.get());
  return remote ? remote->history.owner : nullptr;
}
std::shared_ptr<luciole::NetworkHistory> luciole::historyHandle(const std::shared_ptr<const CopyReceipt>& value) {
  const auto* remote = dynamic_cast<const Copy*>(value.get());
  return remote ? remote->history.owner : nullptr;
}

void luciole::decodeHistory(
    RenameSnapshot& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process
) {
  if (value.contains("receipt") && !value.at("receipt").is_null()) {
    snapshot.receipt = std::make_shared<Rename>(value.at("receipt"), process);
  }
}
void luciole::decodeHistory(
    CreateSnapshot& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process
) {
  if (value.contains("receipt") && !value.at("receipt").is_null()) {
    snapshot.receipt = std::make_shared<Create>(value.at("receipt"), process);
  }
}
void luciole::decodeHistory(
    BatchRenameSnapshot& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process
) {
  if (value.contains("receipt") && !value.at("receipt").is_null()) {
    snapshot.receipt = remoteBatchReceipt(handle(value.at("receipt"), process));
  }
}
void luciole::decodeHistory(
    TransferView& snapshot, const NetworkMessage& value, const std::shared_ptr<NetworkProcess>& process
) {
  if (value.contains("receipt") && !value.at("receipt").is_null()) {
    snapshot.receipt = std::make_shared<Move>(value.at("receipt"), process);
  }
  if (value.contains("copyReceipt") && !value.at("copyReceipt").is_null()) {
    snapshot.copyReceipt = std::make_shared<Copy>(value.at("copyReceipt"), process);
  }
}

luciole::NetworkMessage luciole::retainHistory(WorkerHistory receipt) {
  return std::visit(
      [&](const auto& value) -> NetworkMessage {
        if (!value) {
          return nullptr;
        }
        const auto id = ++nextReceipt;
        NetworkMessage result{{"id", id}};
        using T = std::remove_cvref_t<decltype(value)>::element_type;
        if constexpr (std::is_same_v<T, const RenameReceipt>) {
          result["source"] = value->source();
          result["destination"] = value->destination();
        }
        if constexpr (std::is_same_v<T, const CreateReceipt>) {
          result["kind"] = value->kind();
          result["destination"] = value->destination();
        }
        if constexpr (std::is_same_v<T, const MoveReceipt> || std::is_same_v<T, const CopyReceipt>) {
          result["size"] = value->size();
          result["locations"] = value->failedLocations(unconfirmed);
        }
        if constexpr (std::is_same_v<T, const CopyReceipt>) {
          result["action"] = value->action();
        }
        receipts.emplace(id, std::move(receipt));
        return result;
      },
      receipt
  );
}
const luciole::WorkerHistory& luciole::workerHistory(std::uint64_t id) {
  const auto found = receipts.find(id);
  if (found == receipts.end()) {
    throwFileError(FileErrorKind::Changed, "The filesystem history is no longer available.");
  }
  return found->second;
}
void luciole::releaseHistory(std::uint64_t id) { receipts.erase(id); }
luciole::NetworkMessage luciole::reverseHistory(const NetworkMessage& request, const NetworkEvent& event) {
  const auto held = workerHistory(request.at("receipt").get<std::uint64_t>());
  const CancelToken token;
  std::atomic<std::size_t> completed{0};
  std::optional<JobResult<NetworkMessage>> reply;
  runJob<NetworkMessage>(
      [held, token, &completed] {
        return std::visit(
            [&](const auto& receipt) -> NetworkMessage {
              using T = std::remove_cvref_t<decltype(receipt)>::element_type;
              if constexpr (std::is_same_v<T, const RenameReceipt>) {
                return {{"receipt", retainHistory(receipt->reverse(token))}};
              } else if constexpr (std::is_same_v<T, const CreateReceipt>) {
                const auto result = receipt->reverse(token);
                return {
                    {"receipt", retainHistory(result.receipt)},
                    {"error", result.error},
                    {"applied", result.applied},
                    {"removed", result.removed},
                    {"retireHistory", result.retireHistory}
                };
              } else if constexpr (std::is_same_v<T, const MoveReceipt> || std::is_same_v<T, const CopyReceipt>) {
                const auto result = receipt->reverse(token, completed);
                return {
                    {std::is_same_v<T, const MoveReceipt> ? "receipt" : "copyReceipt", retainHistory(result.receipt)},
                    {"results", result.results},
                    {"retireHistory", result.retireHistory}
                };
              } else {
                throwFileError(FileErrorKind::Unsupported, "Batch history must be replayed through its service.");
              }
            },
            held
        );
      },
      [&](auto value) { reply.emplace(std::move(value)); }
  );
  const WorkerProgressTimer progressTimer;
  std::size_t sent = 0;
  std::exception_ptr progressError;
  while (!reply) {
    if (networkWorkerCancelled()) {
      token.cancel();
    }
    (void)g_main_context_iteration(nullptr, TRUE);
    if (const auto count = completed.load(); !progressError && count != sent) {
      try {
        (void)event({{"completed", count}});
        sent = count;
      } catch (...) {
        // The job still uses the progress counter until its completion callback runs.
        progressError = std::current_exception();
        token.cancel();
      }
    }
  }
  if (progressError) {
    std::rethrow_exception(progressError);
  }
  if (!*reply) {
    throw FileErrorException(reply->error());
  }
  return std::move(**reply);
}
