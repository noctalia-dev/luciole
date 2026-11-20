#include "thumbnails/preview_metadata.h"

#include "fs/gio_interop.h"
#include "fs/network_info.h"
#include "fs/network_job.h"
#include "thumbnails/metadata_probe.h"

#include <utility>

namespace {
  bool sameSource(const luciole::DirectoryEntry& a, const luciole::DirectoryEntry& b) {
    return a.id == b.id
        && a.location() == b.location()
        && a.identity() == b.identity()
        && a.etag() == b.etag()
        && a.size == b.size
        && a.modifiedSeconds == b.modifiedSeconds
        && a.contentRevision == b.contentRevision
        && a.contentType == b.contentType
        && a.type == b.type
        && a.symlink == b.symlink
        && a.access.read == b.access.read;
  }
} // namespace

struct luciole::PreviewMetadataService::State : std::enable_shared_from_this<State> {
  std::optional<DirectoryEntry> entry;
  ThumbnailOptions options;
  MetadataFields fields;
  CancelToken cancel;
  std::function<void()> changed;
  std::uint64_t serial = 0;
  bool alive = true, running = false, pending = false;
  void reset() {
    ++serial;
    cancel.cancel();
    entry.reset();
    fields.clear();
    pending = false;
  }
  void start() {
    if (!alive || running || !pending || !entry) {
      return;
    }
    pending = false;
    running = true;
    cancel = CancelToken{};
    runIoJob<MetadataFields>(
        ioLane(entry->location()) == IoLane::Network ? IoLane::NetworkPreviewProbe : IoLane::PreviewProbe, cancel,
        [source = *entry, options = options, token = cancel] {
          return probePreviewMetadata(source, options, gioCancellable(token));
        },
        [self = shared_from_this(), serial = serial](auto reply) {
          self->running = false;
          if (self->alive && serial == self->serial) {
            self->fields = reply ? std::move(*reply) : MetadataFields{};
            if (self->changed) {
              self->changed();
            }
          }
          self->start();
        }
    );
  }
};

luciole::PreviewMetadataService::PreviewMetadataService() : m_state(std::make_shared<State>()) {}
luciole::PreviewMetadataService::~PreviewMetadataService() {
  m_state->alive = false;
  m_state->changed = {};
  clear();
}
void luciole::PreviewMetadataService::clear() {
  if (m_state->entry) {
    m_state->reset();
  }
}
void luciole::PreviewMetadataService::show(const DirectoryEntry& entry, ThumbnailOptions options) {
  auto& state = *m_state;
  if (state.entry && sameSource(*state.entry, entry) && state.options.maxFileMiB == options.maxFileMiB) {
    return;
  }
  state.reset();
  state.entry = entry;
  state.options = options;
  state.pending = entry.location().isNative();
  state.start();
}
void luciole::PreviewMetadataService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
const luciole::MetadataFields& luciole::PreviewMetadataService::fields() const { return m_state->fields; }
bool luciole::PreviewMetadataService::busy() const { return m_state->running || m_state->pending; }
