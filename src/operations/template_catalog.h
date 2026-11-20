#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/location.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace luciole {
  struct TemplateRevision {
    std::uint64_t device, inode, size;
    std::int64_t modifiedSeconds, modifiedNanos, changedSeconds, changedNanos;
    bool operator==(const TemplateRevision&) const = default;
  };
  TemplateRevision templateRevision(const struct stat& info);
  struct DocumentTemplate {
    Location location;
    std::string name;
    std::string label;
    TemplateRevision revision;
  };
  struct TemplateListing {
    std::vector<DocumentTemplate> items;
    std::optional<FileError> error;
    bool truncated = false;
  };
  TemplateListing readTemplates(const CancelToken& token);
  // Reads the configured XDG Templates directory on a worker; obsolete replies are discarded.
  class TemplateCatalog {
  public:
    TemplateCatalog();
    ~TemplateCatalog();
    TemplateCatalog(const TemplateCatalog&) = delete;
    TemplateCatalog& operator=(const TemplateCatalog&) = delete;
    void load(std::function<void(TemplateListing)> completed);
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
