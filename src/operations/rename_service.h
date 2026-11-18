#pragma once

#include "fs/file_error.h"
#include "fs/location.h"
#include "operations/rename_receipt.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace luciole {
  enum class RenameStatus { Idle, Preparing, Ready, Renaming, Succeeded, Error, Cancelled };
  struct RenameSnapshot {
    RenameStatus status = RenameStatus::Idle;
    std::optional<Location> source;
    std::optional<Location> destination;
    std::optional<FileError> error;
    std::shared_ptr<const RenameReceipt> receipt;
  };

  // One native rename. Preparation pins identities while the editor is open.
  class RenameService {
  public:
    RenameService();
    ~RenameService();
    RenameService(const RenameService&) = delete;
    RenameService& operator=(const RenameService&) = delete;
    bool prepare(const Location& source);
    bool rename(std::string name);
    bool replay(std::shared_ptr<const RenameReceipt> receipt);
    void cancel();
    bool busy() const;
    bool canRename() const;
    const RenameSnapshot& snapshot() const;
    // Hand optional history to its application owner without retaining descriptors in the report.
    std::shared_ptr<const RenameReceipt> takeReceipt();
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
