#include "browser/selection_details.h"

#include "browser/entry_details.h"
#include "browser/location_presentation.h"
#include "fs/file_query.h"
#include "fs/gio_ptr.h"
#include "fs/network_info.h"
#include "fs/network_job.h"

#include <algorithm>
#include <format>
#include <limits>

namespace {
  struct Common {
    std::optional<std::string> first;
    bool mixed = false, unknown = false;
    void add(std::optional<std::string> value) {
      if (!value) {
        unknown = true;
      } else if (!first) {
        first = std::move(value);
      } else if (first != value) {
        mixed = true;
      }
    }
    std::string text() const {
      if (!first) {
        return "Unknown";
      }
      const auto value = mixed ? "Mixed" : *first;
      return unknown ? value + " (some unknown)" : value;
    }
  };
  struct TimeRange {
    std::optional<std::uint64_t> first, last;
    bool unknown = false;
    void add(std::optional<std::uint64_t> value) {
      if (!value || *value > G_MAXINT64) {
        unknown = true;
        return;
      }
      first = first ? std::min(*first, *value) : *value;
      last = last ? std::max(*last, *value) : *value;
    }
    std::string text() const {
      if (!first) {
        return "Unknown";
      }
      auto value = luciole::metadataTime(first);
      const auto end = luciole::metadataTime(last);
      if (value != end) {
        value += " to " + end;
      }
      return unknown ? value + " (some unknown)" : value;
    }
  };
  luciole::SelectionFacts
  inspect(const std::vector<luciole::DirectoryEntry>& entries, std::size_t total, const luciole::CancelToken& token) {
    Common owner, group, mode, location;
    TimeRange modified, accessed, created;
    std::uint64_t bytes = 0;
    std::size_t folders = 0, files = 0, links = 0, other = 0, unavailable = 0, unknownSizes = 0;
    bool overflow = false;
    for (const auto& source : entries) {
      if (token.cancelled()) {
        return {};
      }
      try {
        const auto sourceLocation = source.location();
        const auto details = luciole::readFileDetails(sourceLocation, token);
        const auto identity = source.identity();
        const auto etag = source.etag();
        if ((identity && identity != details.identity)
            || (etag && etag != details.etag)
            || (source.contentRevision && source.contentRevision != details.contentRevision)) {
          throw luciole::FileErrorException({luciole::FileErrorKind::Changed, "The selected item changed"});
        }
        if (details.type == luciole::FileType::Symlink) {
          ++links;
        } else if (details.type == luciole::FileType::Directory) {
          ++folders;
        } else if (details.type == luciole::FileType::Regular) {
          ++files;
        } else {
          ++other;
        }
        if (details.type != luciole::FileType::Directory) {
          if (!details.size) {
            ++unknownSizes;
          } else if (*details.size > std::numeric_limits<std::uint64_t>::max() - bytes) {
            overflow = true;
          } else {
            bytes += *details.size;
          }
        }
        const auto text = [](luciole::InternedText value) {
          return value ? std::optional<std::string>(*value) : std::nullopt;
        };
        owner.add(text(details.metadata.owner));
        group.add(text(details.metadata.group));
        mode.add(details.metadata.mode ? std::optional(std::format("{:04o}", *details.metadata.mode)) : std::nullopt);
        const auto parent = sourceLocation.parent();
        location.add(parent ? std::optional(luciole::locationEditText(*parent)) : std::nullopt);
        modified.add(details.modifiedSeconds);
        accessed.add(details.metadata.accessedSeconds);
        created.add(details.metadata.createdSeconds);
      } catch (const luciole::FileErrorException&) {
        ++unavailable;
        owner.unknown = group.unknown = mode.unknown = location.unknown = true;
        modified.unknown = accessed.unknown = created.unknown = true;
      }
    }
    const bool limited = entries.size() != total;
    if (limited) {
      owner.unknown = group.unknown = mode.unknown = location.unknown = true;
      modified.unknown = accessed.unknown = created.unknown = true;
    }
    const luciole::GString size(g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS));
    const bool partial = limited || unavailable || unknownSizes;
    luciole::SelectionFacts fields{
        {"Inspected", std::format("{} of {} items", entries.size() - unavailable, total)},
        {"Files", std::to_string(files)},
        {"Folders", std::to_string(folders)},
        {"Links", std::to_string(links)},
        {"Other", std::to_string(other)},
        {"File/link size", overflow ? "Too large to total" : (partial ? "At least " : "") + std::string(size.get())},
        {"Owner", owner.text()},
        {"Group", group.text()},
        {"Permissions", mode.text()},
        {"Location", location.text()},
        {"Modified", modified.text()},
        {"Accessed", accessed.text()},
        {"Created", created.text()}
    };
    if (folders) {
      fields.emplace_back("Folder contents", "Not counted");
    }
    if (unavailable) {
      fields.emplace_back("Unavailable", std::format("{} missing, changed or inaccessible items", unavailable));
    }
    if (unknownSizes) {
      fields.emplace_back("Unknown sizes", std::to_string(unknownSizes));
    }
    if (limited) {
      fields.emplace_back("Inspection limit", "Only the first 4096 selected items were inspected");
    }
    return fields;
  }
} // namespace

struct luciole::SelectionDetailsService::State : std::enable_shared_from_this<State> {
  std::vector<DirectoryEntry> entries;
  std::size_t total = 0;
  SelectionFacts fields;
  CancelToken cancel;
  std::function<void()> changed;
  std::uint64_t serial = 0;
  bool alive = true, running = false, pending = false;
  void start() {
    if (!alive || running || !pending) {
      return;
    }
    pending = false;
    running = true;
    cancel = CancelToken{};
    runIoJob<SelectionFacts>(
        std::ranges::any_of(entries, [](const auto& entry) { return ioLane(entry.location()) == IoLane::Network; })
            ? IoLane::Network
            : IoLane::Native,
        cancel, [entries = entries, total = total, token = cancel] { return inspect(entries, total, token); },
        [self = shared_from_this(), serial = serial](auto reply) {
          self->running = false;
          if (self->alive && serial == self->serial) {
            self->fields = reply ? std::move(*reply) : SelectionFacts{{"Status", "Metadata unavailable"}};
            if (self->changed) {
              self->changed();
            }
          }
          self->start();
        }
    );
  }
};
luciole::SelectionDetailsService::SelectionDetailsService() : m_state(std::make_shared<State>()) {}
luciole::SelectionDetailsService::~SelectionDetailsService() {
  m_state->alive = false;
  m_state->changed = {};
  clear();
}
void luciole::SelectionDetailsService::clear() {
  ++m_state->serial;
  m_state->cancel.cancel();
  m_state->pending = false;
  m_state->entries.clear();
  m_state->fields.clear();
}
void luciole::SelectionDetailsService::show(std::span<const DirectoryEntry> entries) {
  clear();
  m_state->total = entries.size();
  m_state->entries.assign(entries.begin(), entries.begin() + std::min<std::size_t>(4096, entries.size()));
  m_state->fields = {{"Status", "Reading selected items…"}};
  m_state->pending = true;
  m_state->start();
}
void luciole::SelectionDetailsService::setChangedCallback(std::function<void()> changed) {
  m_state->changed = std::move(changed);
}
const luciole::SelectionFacts& luciole::SelectionDetailsService::fields() const { return m_state->fields; }
bool luciole::SelectionDetailsService::busy() const { return m_state->running || m_state->pending; }
