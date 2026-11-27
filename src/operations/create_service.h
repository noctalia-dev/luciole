#pragma once

#include "fs/directory_session.h"
#include "fs/file_error.h"
#include "fs/location.h"
#include "operations/template_catalog.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace luciole {
  class CreateReceipt;
  enum class CreateKind { Folder, EmptyFile, Document, Image, SymbolicLink, HardLink };
  enum class CreateStatus { Idle, Preparing, Ready, Creating, Succeeded, Partial, Error, Cancelled };
  struct CreateSnapshot {
    CreateStatus status = CreateStatus::Idle;
    CreateKind kind = CreateKind::Folder;
    std::optional<Location> parent;
    std::optional<Location> destination;
    std::optional<FileError> error;
    std::shared_ptr<const CreateReceipt> receipt;
    bool removed = false, retireHistory = false;
  };

  // One native or administrator file/folder creation. Preparation captures the destination while editing.
  class CreateService {
  public:
    CreateService();
    ~CreateService();
    CreateService(const CreateService&) = delete;
    CreateService& operator=(const CreateService&) = delete;
    bool prepare(const Location& parent, CreateKind kind);
    bool prepareTemplate(const Location& parent, DocumentTemplate source);
    // Prepares a file holding `bytes`, such as a pasted clipboard image.
    bool prepareImage(const Location& parent, std::string bytes);
    bool prepareLink(const DirectoryEntry& source);
    bool createLink(const Location& parent, std::string name, CreateKind kind);
    bool create(std::string name);
    bool replay(std::shared_ptr<const CreateReceipt> receipt);
    void cancel();
    bool busy() const;
    bool canCreate() const;
    const CreateSnapshot& snapshot() const;
    // Hand optional history to its application owner without retaining descriptors in the report.
    std::shared_ptr<const CreateReceipt> takeReceipt();
    void setChangedCallback(std::function<void()> callback);

  private:
    bool prepareTarget(
        const Location& parent, CreateKind kind, std::optional<DocumentTemplate> source,
        std::optional<std::string> image = std::nullopt
    );
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
