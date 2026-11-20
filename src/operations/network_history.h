#pragma once

#include "operations/copy_receipt.h"
#include "operations/move_receipt.h"
#include "operations/network_service.h"

#include <variant>

namespace luciole {
  struct NetworkHistory {
    std::shared_ptr<NetworkProcess> process;
    std::uint64_t id;
    bool usesMount(const HistoryMount& mount) const;
    ~NetworkHistory();
    NetworkMessage reverse(const CancelToken& token, std::atomic<std::size_t>* completed = nullptr) const;
  };
  std::shared_ptr<NetworkHistory> historyHandle(const std::shared_ptr<const RenameReceipt>& value);
  std::shared_ptr<NetworkHistory> historyHandle(const std::shared_ptr<const CreateReceipt>& value);
  std::shared_ptr<NetworkHistory> historyHandle(const std::shared_ptr<const MoveReceipt>& value);
  std::shared_ptr<NetworkHistory> historyHandle(const std::shared_ptr<const CopyReceipt>& value);
  std::shared_ptr<NetworkHistory> historyHandle(const std::shared_ptr<const BatchRenameReceipt>& receipt);
  std::shared_ptr<const BatchRenameReceipt> remoteBatchReceipt(std::shared_ptr<NetworkHistory> history);

  using WorkerHistory = std::variant<
      std::shared_ptr<const RenameReceipt>, std::shared_ptr<const CreateReceipt>, std::shared_ptr<const MoveReceipt>,
      std::shared_ptr<const CopyReceipt>, std::shared_ptr<const BatchRenameReceipt>>;
  NetworkMessage retainHistory(WorkerHistory receipt);
  const WorkerHistory& workerHistory(std::uint64_t id);
  void releaseHistory(std::uint64_t id);
  NetworkMessage reverseHistory(const NetworkMessage& request, const NetworkEvent& event);
} // namespace luciole
