#include "thumbnails/preview_service.h"

#include "fs/gio_interop.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  bool sameSource(const luciole::DirectoryEntry& a, const luciole::DirectoryEntry& b) {
    return a.id == b.id
        && a.location == b.location
        && a.displayName == b.displayName
        && a.icon == b.icon
        && a.access.read == b.access.read
        && a.identity == b.identity
        && a.etag == b.etag
        && a.size == b.size
        && a.modifiedSeconds == b.modifiedSeconds
        && a.contentRevision == b.contentRevision
        && a.contentType == b.contentType
        && a.type == b.type
        && a.symlink == b.symlink;
  }
  std::string stamp(const struct stat& s) {
    return std::format(
        "{}:{}:{}:{}:{}:{}:{}", s.st_dev, s.st_ino, s.st_size, s.st_mtim.tv_sec, s.st_mtim.tv_nsec, s.st_ctim.tv_sec,
        s.st_ctim.tv_nsec
    );
  }
  struct TextResult {
    std::string text, message;
    bool error = false;
  };
  TextResult readText(const luciole::DirectoryEntry& entry, GCancellable* cancel) {
    using namespace luciole;
    const auto path = entry.location.nativePath();
    if (!path) {
      return {{}, "Text previews are available for local files."};
    }
    const GioPtr<GFileInfo> fs(
        g_file_query_filesystem_info(gioFile(entry.location).get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, cancel, nullptr)
    );
    if (!fs
        || !g_file_info_has_attribute(fs.get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE)
        || g_file_info_get_attribute_boolean(fs.get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE)) {
      return {{}, "Text previews are available for local files."};
    }
    struct File {
      int fd;
      ~File() {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    } file{::open(path->c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    struct stat before{};
    if (file.fd < 0 || ::fstat(file.fd, &before) < 0) {
      return {{}, "Could not read this file.", true};
    }
    if (!S_ISREG(before.st_mode)) {
      return {{}, "This item does not have a text preview."};
    }
    if ((entry.size && *entry.size != static_cast<std::uint64_t>(before.st_size))
        || (entry.modifiedSeconds && *entry.modifiedSeconds != static_cast<std::uint64_t>(before.st_mtim.tv_sec))) {
      return {{}, "The file changed. Select it again.", true};
    }
    const GioPtr<GFileInfo> info(g_file_query_info(
        gioFile(entry.location).get(),
        G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_ETAG_VALUE
                                 "," G_FILE_ATTRIBUTE_TIME_CHANGED
                                 ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr
    ));
    if (!info
        || (entry.identity && readFileIdentity(info.get()) != entry.identity)
        || (entry.etag && (!g_file_info_get_etag(info.get()) || *entry.etag != g_file_info_get_etag(info.get())))) {
      return {{}, "The file changed. Select it again.", true};
    }
    if (entry.contentRevision) {
      const auto seconds = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_CHANGED);
      const auto nanos = g_file_info_has_attribute(info.get(), "time::changed-nsec")
          ? g_file_info_get_attribute_uint32(info.get(), "time::changed-nsec")
          : g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_TIME_CHANGED_USEC) * 1000U;
      if (*entry.contentRevision != std::to_string(seconds) + ':' + std::to_string(nanos)) {
        return {{}, "The file changed. Select it again.", true};
      }
    }
    constexpr std::size_t limit = 64U * 1024U;
    std::string bytes;
    std::array<char, 4096> buffer{};
    while (bytes.size() < limit && !g_cancellable_is_cancelled(cancel)) {
      const auto count = ::read(file.fd, buffer.data(), std::min(buffer.size(), limit - bytes.size()));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        return {{}, "Could not read this file.", true};
      }
      if (!count) {
        break;
      }
      bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    struct stat after{}, named{};
    if (g_cancellable_is_cancelled(cancel)) {
      return {};
    }
    if (::fstat(file.fd, &after) < 0
        || ::lstat(path->c_str(), &named) < 0
        || stamp(before) != stamp(after)
        || stamp(before) != stamp(named)) {
      return {{}, "The file changed while reading the preview.", true};
    }
    bool truncated = static_cast<std::uint64_t>(before.st_size) > bytes.size();
    if (bytes.starts_with("\xEF\xBB\xBF")) {
      bytes.erase(0, 3);
    }
    const char* end = nullptr;
    if (!g_utf8_validate(bytes.data(), static_cast<gssize>(bytes.size()), &end)) {
      if (!truncated
          || bytes.data() + bytes.size() - end > 3
          || g_utf8_get_char_validated(end, bytes.data() + bytes.size() - end) != static_cast<gunichar>(-2)) {
        return {{}, "This file does not contain supported UTF-8 text."};
      }
      bytes.resize(static_cast<std::size_t>(end - bytes.data()));
    }
    std::string text;
    std::size_t lines = 0, columns = 0;
    for (const char* p = bytes.c_str(); *p; p = g_utf8_next_char(p)) {
      if (text.size() >= 8192 || lines >= 200) {
        truncated = true;
        break;
      }
      const auto character = g_utf8_get_char(p);
      if (character == '\n') {
        ++lines;
        columns = 0;
        text += '\n';
      } else if (columns++ < 240) {
        if (character == '\t') {
          text += "    ";
        } else if (!g_unichar_iscntrl(character)) {
          text.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        }
      } else if (columns == 241) {
        text += "…";
        truncated = true;
      }
    }
    return {std::move(text), truncated ? "Preview truncated" : bytes.empty() ? "Empty file" : ""};
  }
} // namespace

struct luciole::PreviewService::State : std::enable_shared_from_this<State> {
  PreviewSnapshot view;
  ThumbnailService thumbnails{ThumbnailPurpose::Preview};
  PreviewMetadataService metadata;
  std::shared_ptr<const FileIconImage> pendingImage;
  GioPtr<GCancellable> cancel;
  std::function<void()> changed;
  std::uint64_t serial = 0;
  int pixels = 0;
  ThumbnailOptions options;
  bool running = false, pending = false, alive = true;
  void resetSelection() {
    ++serial;
    pending = false;
    if (cancel) {
      g_cancellable_cancel(cancel.get());
    }
    view = {};
    pendingImage.reset();
    thumbnails.endFrame();
  }
  void imageChanged() {
    if (!pendingImage || !pendingImage->ready) {
      return;
    }
    if (pendingImage->pixels) {
      view.image = pendingImage;
    }
    view.imageUpgradeFailed = !pendingImage->pixels;
    view.status = view.image && view.image->pixels ? PreviewStatus::Ready : PreviewStatus::Unavailable;
    view.message = pendingImage->pixels ? ""
        : view.image                    ? "Larger preview unavailable."
                                        : "No preview available for this file.";
    pendingImage.reset();
  }
  void notify() {
    if (changed) {
      changed();
    }
  }
  void start() {
    if (!alive || running || !pending || !view.entry) {
      return;
    }
    pending = false;
    running = true;
    cancel.reset(g_cancellable_new());
    struct Work {
      std::shared_ptr<State> owner;
      DirectoryEntry entry;
      std::uint64_t serial;
    };
    auto* work = new Work{shared_from_this(), *view.entry, serial};
    const GioPtr<GTask> task(g_task_new(
        nullptr, cancel.get(),
        [](GObject*, GAsyncResult* result, gpointer) {
          auto* task = G_TASK(result);
          auto& work = *static_cast<Work*>(g_task_get_task_data(task));
          const auto state = work.owner;
          const std::unique_ptr<TextResult> text(static_cast<TextResult*>(g_task_propagate_pointer(task, nullptr)));
          state->running = false;
          if (state->alive && work.serial == state->serial && text) {
            state->view.text = text->text;
            state->view.message = text->message;
            state->view.status = text->error                             ? PreviewStatus::Error
                : (!text->text.empty() || text->message == "Empty file") ? PreviewStatus::Ready
                                                                         : PreviewStatus::Unavailable;
            state->notify();
          }
          state->start();
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), work, [](gpointer p) { delete static_cast<Work*>(p); });
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
      auto result = std::make_unique<TextResult>(readText(static_cast<Work*>(data)->entry, cancel));
      g_task_return_pointer(task, result.release(), [](gpointer p) { delete static_cast<TextResult*>(p); });
    });
  }
};
luciole::PreviewService::PreviewService() : m_state(std::make_shared<State>()) {
  const std::weak_ptr<State> weak = m_state;
  m_state->metadata.setChangedCallback([weak] {
    if (const auto state = weak.lock(); state && state->alive && state->view.entry) {
      state->view.metadata = state->metadata.fields();
      state->notify();
    }
  });
  m_state->thumbnails.setChangedCallback([weak] {
    const auto state = weak.lock();
    if (!state || !state->alive || !state->pendingImage) {
      return;
    }
    state->imageChanged();
    state->notify();
  });
}
luciole::PreviewService::~PreviewService() {
  clear();
  m_state->alive = false;
  m_state->changed = {};
}
void luciole::PreviewService::clear() {
  m_state->metadata.clear();
  if (!m_state->view.entry && !m_state->pendingImage && !m_state->pending && m_state->thumbnails.reservedBytes() == 0) {
    return;
  }
  m_state->resetSelection();
  m_state->thumbnails.invalidate();
}
void luciole::PreviewService::show(
    std::optional<DirectoryEntry> entry, int pixels, ThumbnailOptions options,
    std::shared_ptr<const FileIconImage> prepared
) {
  pixels = ThumbnailService::pixelSizeClass(
      pixels,
      entry && entry->contentType && entry->contentType->starts_with("image/") ? ThumbnailPurpose::Preview
                                                                               : ThumbnailPurpose::Thumbnail
  );
  if (!entry) {
    if (m_state->view.entry) {
      clear();
      m_state->notify();
    }
    return;
  }
  const bool unchanged = m_state->view.entry && sameSource(*entry, *m_state->view.entry) && options == m_state->options;
  if (unchanged
      && (pixels == m_state->pixels
          || (pixels < m_state->pixels && !m_state->view.imageUpgradeFailed)
          || luciole::contentTypeIsText(entry->contentType.value_or("")))) {
    if (prepared && prepared->pixels && (!m_state->view.image || !m_state->view.image->pixels)) {
      m_state->view.image = std::move(prepared);
      m_state->view.status = PreviewStatus::Ready;
      m_state->view.message = m_state->view.imageUpgradeFailed ? "Larger preview unavailable." : "";
      m_state->notify();
    }
    return;
  }
  if (unchanged && m_state->view.image && m_state->view.image->pixels) {
    prepared = m_state->view.image;
  }
  m_state->resetSelection();
  auto& state = *m_state;
  state.pixels = pixels;
  state.options = options;
  state.view.entry = *entry;
  state.metadata.show(*entry, options);
  state.view.metadata = state.metadata.fields();
  state.view.status = PreviewStatus::Unavailable;
  state.view.message = entry->type == luciole::FileType::Directory ? "Folder" : "No preview available for this file.";
  if (entry->type == luciole::FileType::Regular && !entry->symlink && entry->access.read != false) {
    state.view.status = PreviewStatus::Loading;
    state.view.message = "Loading preview…";
    if (luciole::contentTypeIsText(entry->contentType.value_or(""))) {
      state.pending = true;
      state.start();
    } else {
      options.policy = ThumbnailPolicy::All;
      state.thumbnails.setOptions(options);
      if (prepared && prepared->pixels) {
        state.view.image = std::move(prepared);
        state.view.status = PreviewStatus::Ready;
        state.view.message.clear();
      }
      state.pendingImage = state.thumbnails.request(*entry, pixels);
      state.imageChanged();
      if (!state.pendingImage && !state.view.image) {
        state.view.status = PreviewStatus::Unavailable;
        state.view.message = "No preview available for this file.";
      }
    }
  }
  state.notify();
}
void luciole::PreviewService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
const luciole::PreviewSnapshot& luciole::PreviewService::snapshot() const { return m_state->view; }
bool luciole::PreviewService::busy() const {
  return m_state->metadata.busy()
      || m_state->running
      || m_state->pending
      || m_state->thumbnails.activeJobs() != 0
      || m_state->thumbnails.queuedJobs() != 0;
}
