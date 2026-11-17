#include "clipboard/text_clipboard.h"

#include "fs/gio_interop.h"
#include "fs/location.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
  constexpr std::size_t kTextLimit = 1024 * 1024;
  constexpr auto kFileDropReadTimeout = std::chrono::seconds(120);
  struct DragIcon {
    wl_surface* surface = nullptr;
    wl_buffer* buffer = nullptr;
    ~DragIcon() {
      if (surface) {
        wl_surface_destroy(surface);
      }
      if (buffer) {
        wl_buffer_destroy(buffer);
      }
    }
    static std::unique_ptr<DragIcon> create(wl_compositor* compositor, wl_shm* shm, const LoadedImageFile* image) {
      if (!compositor
          || !shm
          || !image
          || image->width <= 0
          || image->height <= 0
          || image->width > 256
          || image->height > 256) {
        return {};
      }
      const int width = image->width >= image->height ? 40 : std::max(1, 40 * image->width / image->height);
      const int height = image->height >= image->width ? 40 : std::max(1, 40 * image->height / image->width);
      const auto bytes = static_cast<std::size_t>(width) * height * 4;
      if (image->rgba.size() != static_cast<std::size_t>(image->width) * image->height * 4) {
        return {};
      }
      const int fd = memfd_create("luciole-drag", MFD_CLOEXEC);
      if (fd < 0) {
        return {};
      }
      if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return {};
      }
      void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mapping == MAP_FAILED) {
        ::close(fd);
        return {};
      }
      auto* pixels = static_cast<std::uint32_t*>(mapping);
      for (std::size_t i = 0; i < bytes / 4; ++i) {
        const auto x = (i % width) * image->width / width;
        const auto y = (i / width) * image->height / height;
        const auto* rgba = &image->rgba[(y * image->width + x) * 4];
        const std::uint32_t alpha = rgba[3];
        pixels[i] =
            (alpha << 24) | ((rgba[0] * alpha / 255) << 16) | ((rgba[1] * alpha / 255) << 8) | (rgba[2] * alpha / 255);
      }
      auto icon = std::make_unique<DragIcon>();
      auto* pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(bytes));
      icon->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
      wl_shm_pool_destroy(pool);
      munmap(mapping, bytes);
      ::close(fd);
      icon->surface = wl_compositor_create_surface(compositor);
      return icon;
    }
    void commit() const {
      wl_surface_attach(surface, buffer, 0, 0);
      wl_surface_damage(surface, 0, 0, 256, 256);
      wl_surface_commit(surface);
    }
  };
  struct PipeTransfer {
    int fd;
    std::string text;
    bool sending;
    bool valid = true;
    std::chrono::seconds timeout{5};
    ~PipeTransfer() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  };

  void transfer(GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
    auto& pipe = *static_cast<PipeTransfer*>(data);
    sigset_t blocked{}, previous{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    sigset_t pending{};
    sigpending(&pending);
    const bool pendingPipe = sigismember(&pending, SIGPIPE) == 1;
    const int flags = fcntl(pipe.fd, F_GETFL);
    if (flags < 0 || fcntl(pipe.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      pipe.valid = false;
    }
    const auto deadline = std::chrono::steady_clock::now() + pipe.timeout;
    std::size_t offset = 0;
    std::array<char, 8192> buffer{};
    while (pipe.valid && !g_cancellable_is_cancelled(cancellable)) {
      if (pipe.sending && offset == pipe.text.size()) {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        pipe.valid = false;
        break;
      }
      pollfd fd{pipe.fd, static_cast<short>(pipe.sending ? POLLOUT : POLLIN), 0};
      const int ready = ::poll(&fd, 1, 50);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready < 0) {
        pipe.valid = false;
        break;
      }
      if (ready == 0) {
        continue;
      }
      const auto count = pipe.sending ? ::write(pipe.fd, pipe.text.data() + offset, pipe.text.size() - offset)
                                      : ::read(pipe.fd, buffer.data(), buffer.size());
      if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
        continue;
      }
      if (count < 0) {
        pipe.valid = false;
        break;
      }
      if (count == 0) {
        break;
      }
      if (pipe.sending) {
        offset += static_cast<std::size_t>(count);
      } else {
        pipe.text.append(buffer.data(), static_cast<std::size_t>(count));
        if (pipe.text.size() > kTextLimit) {
          pipe.valid = false;
        }
      }
    }
    if (!pendingPipe) {
      const timespec noWait{};
      (void)sigtimedwait(&blocked, nullptr, &noWait);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    pipe.valid = pipe.valid && !g_cancellable_is_cancelled(cancellable);
    if (!pipe.sending) {
      pipe.valid = pipe.valid
          && !pipe.text.contains('\0')
          && g_utf8_validate(pipe.text.data(), static_cast<gssize>(pipe.text.size()), nullptr);
    }
    // Closing before notification also gives receiving clients their EOF promptly.
    ::close(std::exchange(pipe.fd, -1));
    g_task_return_boolean(task, pipe.valid);
  }
} // namespace

struct luciole::WaylandTextClipboard::State : std::enable_shared_from_this<State> {
  struct Offer {
    std::unordered_set<std::string> types;
    std::uint32_t actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    std::uint32_t action = 0;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    GioPtr<GCancellable> reading{g_cancellable_new()};
  };
  struct Reconciliation {
    std::uint64_t revision;
    wl_data_source* owner;
    wl_data_offer* offer;
    FilePayload remaining;
  };
  struct Source {
    std::unordered_map<std::string, std::string> data;
    bool drag = false;
    bool dropped = false;
    std::unique_ptr<DragIcon> icon = nullptr;
    std::function<void()> finished;
    std::optional<FilePayload> files;
    std::function<void(std::optional<DragAction>)> actionChanged = nullptr;
    std::uint32_t action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    bool accepted = false;

    void notifyAction() const {
      if (!drag || dropped || !actionChanged) {
        return;
      }
      std::optional<DragAction> selectedAction;
      if (accepted && action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
        selectedAction = DragAction::Copy;
      } else if (accepted && action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) {
        selectedAction = DragAction::Move;
      } else if (accepted && action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
        selectedAction = DragAction::Ask;
      }
      const auto callback = actionChanged;
      callback(selectedAction);
    }
  };
  wl_display* display = nullptr;
  wl_callback* focusSync = nullptr;
  wl_data_device_manager* manager = nullptr;
  wl_compositor* compositor = nullptr;
  wl_shm* shm = nullptr;
  wl_data_device* device = nullptr;
  wl_data_offer* selected = nullptr;
  wl_data_offer* dragging = nullptr;
  wl_data_source* owned = nullptr;
  std::function<std::uint32_t()> serial;
  std::unordered_map<wl_data_offer*, Offer> offers;
  std::unordered_map<wl_data_source*, Source> sources;
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  GioPtr<GCancellable> reading{g_cancellable_new()};
  std::optional<std::string> text;
  std::optional<FilePayload> files;
  std::function<void()> changed;
  std::function<FileDropTarget(wl_surface*, float, float)> hover;
  std::function<void()> leave;
  FileDropTarget target;
  wl_surface* dragSurface = nullptr;
  float dragX = 0, dragY = 0;
  std::uint32_t dragSerial = 0;
  bool pending = false;
  bool active = true;
  std::uint64_t epoch = 0;
  std::optional<std::uint64_t> moving;
  std::optional<Reconciliation> reconciliation;
  std::vector<std::function<void(std::optional<std::string>)>> callbacks;

  void discard(wl_data_offer* offer) {
    const auto found = offers.find(offer);
    if (found != offers.end()) {
      g_cancellable_cancel(found->second.reading.get());
      offers.erase(found);
      wl_data_offer_destroy(offer);
    }
  }
  void notify() const {
    if (active && changed) {
      changed();
    }
  }
  void received() {
    auto waiting = std::exchange(callbacks, {});
    const auto value = text;
    for (const auto& callback : waiting) {
      callback(value);
    }
  }
  void reset() {
    ++epoch;
    reconciliation.reset();
    if (focusSync) {
      wl_callback_destroy(std::exchange(focusSync, nullptr));
    }
    g_cancellable_cancel(reading.get());
    reading.reset(g_cancellable_new());
    pending = false;
    text.reset();
    files.reset();
    received();
    notify();
  }
  void read(
      wl_data_offer* offer, const char* mime, GCancellable* cancel,
      std::function<void(std::optional<std::string>)> callback, std::chrono::seconds timeout = std::chrono::seconds(5)
  ) {
    int fds[2];
    if (!offer || !offers.contains(offer) || ::pipe2(fds, O_CLOEXEC) != 0) {
      callback(std::nullopt);
      return;
    }
    wl_data_offer_receive(offer, mime, fds[1]);
    ::close(fds[1]);
    using Callback = std::function<void(std::optional<std::string>)>;
    GTask* task = g_task_new(
        nullptr, cancel,
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Callback> callback(static_cast<Callback*>(data));
          const auto* pipe = static_cast<PipeTransfer*>(g_task_get_task_data(G_TASK(result)));
          (*callback)(pipe->valid ? std::optional(pipe->text) : std::nullopt);
        },
        new Callback(std::move(callback))
    );
    g_task_set_task_data(
        task, new PipeTransfer{.fd = fds[0], .text = {}, .sending = false, .timeout = timeout},
        [](gpointer data) { delete static_cast<PipeTransfer*>(data); }
    );
    g_task_run_in_thread(task, transfer);
    g_object_unref(task);
  }
  void selection(wl_data_offer* offer) {
    const auto retained = shared_from_this();
    if (selected != offer) {
      discard(selected);
    }
    selected = offer;
    // Focus changes can re-offer our own immutable selection. Cancellation, not
    // focus, ends source ownership and its cut state.
    if (owned) {
      return;
    }
    reset();
    if (!active || !offer || !offers.contains(offer)) {
      return;
    }
    const auto& types = offers.at(offer).types;
    const char* mime = types.contains("text/plain;charset=utf-8") ? "text/plain;charset=utf-8"
        : types.contains("text/plain")                            ? "text/plain"
                                                                  : nullptr;
    if (!mime) {
      return;
    }
    pending = true;
    read(offer, mime, reading.get(), [self = shared_from_this(), revision = epoch](auto value) {
      if (!self->active || self->epoch != revision) {
        return;
      }
      self->pending = false;
      self->text = std::move(value);
      self->received();
    });
  }
  wl_data_source* source(Source value) {
    auto* source = wl_data_device_manager_create_data_source(manager);
    sources.emplace(source, std::move(value));
    static const wl_data_source_listener listener{
        .target =
            [](void* data, wl_data_source* source, const char* mime) {
              auto& state = *static_cast<State*>(data);
              if (const auto found = state.sources.find(source); found != state.sources.end()) {
                found->second.accepted = mime != nullptr;
                found->second.notifyAction();
              }
            },
        .send =
            [](void* data, wl_data_source* source, const char* mime, int fd) {
              auto& state = *static_cast<State*>(data);
              const auto found = state.sources.find(source);
              if (found == state.sources.end() || !mime || !found->second.data.contains(mime)) {
                ::close(fd);
                return;
              }
              GTask* task = g_task_new(nullptr, state.cancellable.get(), nullptr, nullptr);
              g_task_set_task_data(task, new PipeTransfer{fd, found->second.data.at(mime), true}, [](gpointer data) {
                delete static_cast<PipeTransfer*>(data);
              });
              g_task_run_in_thread(task, transfer);
              g_object_unref(task);
            },
        .cancelled =
            [](void* data, wl_data_source* source) {
              auto& state = *static_cast<State*>(data);
              if (state.owned == source) {
                state.owned = nullptr;
                state.reset();
              }
              auto finished = std::move(state.sources.at(source).finished);
              state.sources.erase(source);
              wl_data_source_destroy(source);
              if (finished) {
                finished();
              }
            },
        .dnd_drop_performed =
            [](void* data, wl_data_source* source) {
              auto& state = *static_cast<State*>(data);
              if (const auto found = state.sources.find(source); found != state.sources.end()) {
                found->second.dropped = true;
                found->second.icon.reset();
                auto finished = std::exchange(found->second.finished, {});
                if (finished) {
                  finished();
                }
              }
            },
        .dnd_finished =
            [](void* data, wl_data_source* source) {
              // URI destinations own filesystem moves. Never delete on finish too.
              auto& state = *static_cast<State*>(data);
              auto finished = std::move(state.sources.at(source).finished);
              state.sources.erase(source);
              wl_data_source_destroy(source);
              if (finished) {
                finished();
              }
            },
        .action =
            [](void* data, wl_data_source* source, std::uint32_t action) {
              auto& state = *static_cast<State*>(data);
              if (const auto found = state.sources.find(source); found != state.sources.end()) {
                found->second.action = action;
                found->second.notifyAction();
              }
            }
    };
    wl_data_source_add_listener(source, &listener, this);
    for (const auto& [mime, unused] : sources.at(source).data) {
      wl_data_source_offer(source, mime.c_str());
    }
    return source;
  }
  bool publish(FilePayload payload) {
    Source value;
    for (const auto mime : {kUriListMime, kGnomeFilesMime, kKdeCutMime}) {
      auto bytes = encodeFilePayload(payload, mime);
      if (!bytes) {
        return false;
      }
      value.data.emplace(mime, std::move(*bytes));
    }
    value.data.emplace("text/plain;charset=utf-8", value.data.at(std::string(kUriListMime)));
    value.data.emplace("text/plain", value.data.at(std::string(kUriListMime)));
    auto* outgoing = source(std::move(value));
    discard(std::exchange(selected, nullptr));
    reset();
    owned = outgoing;
    files = std::move(payload);
    text = sources.at(outgoing).data.at("text/plain");
    wl_data_device_set_selection(device, outgoing, serial());
    notify();
    return true;
  }
  void reconcile() {
    if (!reconciliation) {
      return;
    }
    if (reconciliation->revision != epoch
        || reconciliation->owner != owned
        || (!owned && reconciliation->offer != selected)) {
      reconciliation.reset();
      return;
    }
    const auto inputSerial = serial();
    if (inputSerial == 0) {
      return;
    }
    auto pending = std::move(*std::exchange(reconciliation, std::nullopt));
    if (!pending.remaining.locations.empty()) {
      if (!publish(pending.remaining)) {
        reconciliation = std::move(pending);
      }
    } else {
      owned = nullptr;
      discard(std::exchange(selected, nullptr));
      reset();
      wl_data_device_set_selection(device, nullptr, inputSerial);
    }
  }
  void focusEntered() {
    if (!reconciliation || serial() == 0) {
      return;
    }
    if (focusSync) {
      wl_callback_destroy(std::exchange(focusSync, nullptr));
    }
    // Drain ownership notifications already in flight before a deferred write.
    // This is an asynchronous barrier, never a nested dispatch or roundtrip.
    focusSync = wl_display_sync(display);
    static const wl_callback_listener listener{.done = [](void* data, wl_callback* callback, std::uint32_t) {
      auto& state = *static_cast<State*>(data);
      state.focusSync = nullptr;
      wl_callback_destroy(callback);
      state.reconcile();
    }};
    wl_callback_add_listener(focusSync, &listener, this);
  }
  void updateDrag() {
    target = {};
    if (!active || !dragging || !offers.contains(dragging)) {
      return;
    }
    auto& offer = offers.at(dragging);
    const bool files = offer.types.contains(std::string(kUriListMime));
    const bool extraction = !files
        && offer.types.contains(std::string(kExtractionServiceMime))
        && offer.types.contains(std::string(kExtractionPathMime));
    if ((files || extraction) && hover) {
      target = hover(dragSurface, dragX, dragY);
    }
    if (extraction) {
      if (!target.extract || target.copyOnly) {
        target = {};
      }
      target.preferred = DragAction::Copy;
    }
    const bool acceptedTarget = extraction ? static_cast<bool>(target.extract) : static_cast<bool>(target.receive);
    if (files
        && target.receive
        && !target.copyOnly
        && !target.moveOnly
        && offer.types.contains(std::string(kAskDropMime))) {
      target.preferred = DragAction::Ask;
    }
    const bool modern = wl_data_offer_get_version(dragging) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION;
    const auto mask = target.moveOnly   ? (modern && !target.copyOnly ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE : 0)
        : target.copyOnly || extraction ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                                        : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
            | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
            | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
    const auto supported = offer.actions & mask;
    if (!(supported & (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE))) {
      target = {};
    }
    const bool acceptedOffer = acceptedTarget
        && (supported & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
            || (!extraction && (supported & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)));
    if (modern) {
      std::uint32_t preferred = target.preferred == DragAction::Copy ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
          : target.preferred == DragAction::Ask                      ? WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK
                                                                     : WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
      if ((preferred & supported) == 0) {
        preferred = (supported & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                                                                         : WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
      }
      // Request Ask exclusively when selected, while preserving the source's final action mask.
      const auto accepted = preferred == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK ? preferred : supported;
      wl_data_offer_set_actions(dragging, acceptedOffer ? accepted : 0, acceptedOffer ? preferred : 0);
    }
    const auto mime = extraction ? kExtractionServiceMime : kUriListMime;
    wl_data_offer_accept(dragging, dragSerial, acceptedOffer ? mime.data() : nullptr);
  }
  void extractionDrop(
      wl_data_offer* offer, const std::shared_ptr<int>& token,
      std::function<void(ExtractionOffer, DropCompletion)> receiver, std::function<void(std::string)> failed,
      bool modern
  ) {
    const auto live = [self = shared_from_this(), offer, token] {
      const auto current = self->offers.find(offer);
      return self->active && current != self->offers.end() && current->second.lifetime == token;
    };
    read(
        offer, kExtractionServiceMime.data(), offers.at(offer).reading.get(),
        [self = shared_from_this(), offer, modern, live, receiver = std::move(receiver),
         failed = std::move(failed)](auto service) mutable {
          if (!live()) {
            return;
          }
          if (!service || service->empty() || service->size() > 4096) {
            if (failed) {
              failed("Invalid archive extraction offer.");
            }
            self->discard(offer);
            return;
          }
          self->read(
              offer, kExtractionPathMime.data(), self->offers.at(offer).reading.get(),
              [self, offer, modern, live, service = std::move(*service), receiver = std::move(receiver),
               failed = std::move(failed)](auto path) {
                if (!live()) {
                  return;
                }
                if (!path || path->empty() || path->size() > 4096) {
                  if (failed) {
                    failed("Invalid archive extraction offer.");
                  }
                  self->discard(offer);
                  return;
                }
                const auto finished = std::make_shared<bool>(false);
                const auto finish = [self, offer, modern, live, finished](bool success) {
                  if (!live() || std::exchange(*finished, true)) {
                    return;
                  }
                  if (success && modern) {
                    wl_data_offer_finish(offer);
                  }
                  self->discard(offer);
                };
                receiver({service, std::move(*path)}, finish);
              }
          );
        }
    );
  }
  void drop() {
    auto* offer = std::exchange(dragging, nullptr);
    auto receiver = std::move(target.receive);
    auto extractor = std::move(target.extract);
    auto failed = std::move(target.failed);
    const bool prompt = target.preferred == DragAction::Ask;
    const auto found = offers.find(offer);
    const auto action = found == offers.end() ? 0 : found->second.action;
    const bool modern = offer && wl_data_offer_get_version(offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION;
    if (leave) {
      leave();
    }
    const bool extraction = found != offers.end()
        && !found->second.types.contains(std::string(kUriListMime))
        && found->second.types.contains(std::string(kExtractionServiceMime))
        && found->second.types.contains(std::string(kExtractionPathMime));
    if (!offer
        || found == offers.end()
        || (extraction ? !extractor : !receiver)
        || (modern
            && action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
            && action != WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
            && action != WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)) {
      discard(offer);
      return;
    }
    const auto token = found->second.lifetime;
    if (extraction) {
      if (modern && action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
        discard(offer);
      } else {
        extractionDrop(offer, token, std::move(extractor), std::move(failed), modern);
      }
      return;
    }
    const auto supported = found->second.actions;
    const auto live = [self = shared_from_this(), offer, token] {
      const auto current = self->offers.find(offer);
      return self->active && current != self->offers.end() && current->second.lifetime == token;
    };
    read(
        offer, kUriListMime.data(), found->second.reading.get(),
        [self = shared_from_this(), offer, action, modern, prompt, supported, live, receiver = std::move(receiver),
         failed = std::move(failed)](auto bytes) {
          if (!live()) {
            return;
          }
          auto payload = bytes ? parseFilePayload(kUriListMime, *bytes)
                               : std::expected<FilePayload, std::string>(std::unexpected("Transfer failed."));
          if (!payload) {
            if (failed) {
              failed(
                  bytes ? "Invalid dropped file list: " + payload.error()
                        : "Could not read the dropped file list from the source application."
              );
            }
            self->discard(offer);
            return;
          }
          struct Resolution {
            std::uint32_t action;
            bool chosen = false, finished = false;
          };
          const auto resolution = std::make_shared<Resolution>(
              modern ? action : static_cast<std::uint32_t>(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
          );
          const bool ask = modern && action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
          const bool copy = ask ? (supported & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) != 0
                                : !modern || action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
          const bool move = ask ? (supported & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) != 0
                                : modern && action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
          DropOptions options{
              .action = ask || prompt ? DragAction::Ask
                  : move              ? DragAction::Move
                                      : DragAction::Copy,
              .canCopy = copy,
              .canMove = move,
              .select = [resolution, live, offer, ask, copy, move](TransferAction selected) {
                if (!live()
                    || resolution->chosen
                    || resolution->finished
                    || (selected == TransferAction::Move ? !move : !copy)) {
                  return false;
                }
                resolution->chosen = true;
                resolution->action = selected == TransferAction::Move ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
                                                                      : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
                if (ask) {
                  wl_data_offer_set_actions(offer, resolution->action, resolution->action);
                }
                return true;
              }
          };
          const auto finish = [self, offer, modern, live, resolution](bool success) {
            if (!live() || resolution->finished) {
              return;
            }
            resolution->finished = true;
            if (success && modern && resolution->action != WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
              wl_data_offer_finish(offer);
            }
            self->discard(offer);
          };
          receiver(std::move(*payload), std::move(options), finish);
        },
        kFileDropReadTimeout
    );
  }
};

luciole::WaylandTextClipboard::WaylandTextClipboard(
    wl_display* display, wl_data_device_manager* manager, wl_seat* seat, std::function<std::uint32_t()> serial,
    wl_compositor* compositor, wl_shm* shm
)
    : m_state(std::make_shared<State>()) {
  auto& state = *m_state;
  state.display = display;
  state.manager = manager;
  state.compositor = compositor;
  state.shm = shm;
  state.serial = std::move(serial);
  if (!manager || !seat) {
    return;
  }
  state.device = wl_data_device_manager_get_data_device(manager, seat);
  static const wl_data_device_listener listener{
      .data_offer =
          [](void* data, wl_data_device*, wl_data_offer* offer) {
            auto& state = *static_cast<State*>(data);
            state.offers.try_emplace(offer);
            static const wl_data_offer_listener offers{
                .offer = [](void* data, wl_data_offer* offer,
                            const char* mime) { static_cast<State*>(data)->offers[offer].types.insert(mime); },
                .source_actions =
                    [](void* data, wl_data_offer* offer, std::uint32_t actions) {
                      auto& state = *static_cast<State*>(data);
                      state.offers[offer].actions = actions;
                      if (state.dragging == offer) {
                        state.updateDrag();
                      }
                    },
                .action = [](void* data, wl_data_offer* offer,
                             std::uint32_t action) { static_cast<State*>(data)->offers[offer].action = action; }
            };
            wl_data_offer_add_listener(offer, &offers, &state);
          },
      .enter =
          [](void* data, wl_data_device*, std::uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y,
             wl_data_offer* offer) {
            auto& state = *static_cast<State*>(data);
            state.dragging = offer;
            state.dragSurface = surface;
            state.dragSerial = serial;
            state.dragX = static_cast<float>(wl_fixed_to_double(x));
            state.dragY = static_cast<float>(wl_fixed_to_double(y));
            state.updateDrag();
          },
      .leave =
          [](void* data, wl_data_device*) {
            auto& state = *static_cast<State*>(data);
            state.discard(std::exchange(state.dragging, nullptr));
            state.target = {};
            if (state.leave) {
              state.leave();
            }
          },
      .motion =
          [](void* data, wl_data_device*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
            auto& state = *static_cast<State*>(data);
            state.dragX = static_cast<float>(wl_fixed_to_double(x));
            state.dragY = static_cast<float>(wl_fixed_to_double(y));
            state.updateDrag();
          },
      .drop = [](void* data, wl_data_device*) { static_cast<State*>(data)->drop(); },
      .selection = [](void* data, wl_data_device*,
                      wl_data_offer* offer) { static_cast<State*>(data)->selection(offer); }
  };
  wl_data_device_add_listener(state.device, &listener, &state);
}
luciole::WaylandTextClipboard::~WaylandTextClipboard() {
  auto& state = *m_state;
  state.active = false;
  if (state.focusSync) {
    wl_callback_destroy(std::exchange(state.focusSync, nullptr));
  }
  state.callbacks.clear();
  state.hover = {};
  state.leave = {};
  state.changed = {};
  g_cancellable_cancel(state.reading.get());
  g_cancellable_cancel(state.cancellable.get());
  for (const auto& [offer, offered] : state.offers) {
    g_cancellable_cancel(offered.reading.get());
    wl_data_offer_destroy(offer);
  }
  for (const auto& [source, unused] : state.sources) {
    wl_data_source_destroy(source);
  }
  state.offers.clear();
  state.sources.clear();
  if (state.device) {
    if (wl_data_device_get_version(state.device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
      wl_data_device_release(state.device);
    } else {
      wl_data_device_destroy(state.device);
    }
  }
}
bool luciole::WaylandTextClipboard::available() const { return m_state->device && m_state->serial() != 0; }
std::optional<std::string> luciole::WaylandTextClipboard::clipboardText() { return m_state->text; }
void luciole::WaylandTextClipboard::requestClipboardText(std::function<void(std::optional<std::string>)> callback) {
  if (m_state->pending) {
    m_state->callbacks.push_back(std::move(callback));
  } else {
    callback(m_state->text);
  }
}
void luciole::WaylandTextClipboard::setClipboardText(std::string text) {
  if (!available()
      || text.size() > kTextLimit
      || text.contains('\0')
      || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return;
  }
  auto& state = *m_state;
  auto* source =
      state.source({.data = {{"text/plain;charset=utf-8", text}, {"text/plain", text}}, .finished = {}, .files = {}});
  state.discard(std::exchange(state.selected, nullptr));
  state.reset();
  state.owned = source;
  state.text = std::move(text);
  wl_data_device_set_selection(state.device, source, state.serial());
  state.received();
}
bool luciole::WaylandTextClipboard::setFiles(FilePayload payload) {
  return available() && m_state->publish(std::move(payload));
}
bool luciole::WaylandTextClipboard::hasFiles() const {
  const auto& state = *m_state;
  if (state.moving == state.epoch) {
    return false;
  }
  if (state.files) {
    return !state.files->locations.empty();
  }
  if (const auto found = state.offers.find(state.selected); found != state.offers.end()) {
    return found->second.types.contains(std::string(kGnomeFilesMime))
        || found->second.types.contains(std::string(kUriListMime));
  }
  return false;
}
bool luciole::WaylandTextClipboard::isCut(const Location& location) const {
  return m_state->owned
      && m_state->files
      && m_state->files->cut
      && std::ranges::contains(m_state->files->locations, location);
}
void luciole::WaylandTextClipboard::requestFiles(std::function<void(std::optional<ClipboardFiles>)> callback) {
  auto self = m_state;
  if (!available() || !hasFiles()) {
    callback(std::nullopt);
    return;
  }
  const auto revision = self->epoch;
  if (self->files) {
    callback(ClipboardFiles{*self->files, revision});
    return;
  }
  auto* offer = self->selected;
  const auto& types = self->offers.at(offer).types;
  const bool gnome = types.contains(std::string(kGnomeFilesMime));
  const bool kde = !gnome && types.contains(std::string(kKdeCutMime));
  const auto mime = gnome ? kGnomeFilesMime : kUriListMime;
  // Request the KDE flag alongside the URI data before the offer can disappear.
  struct ReadFiles {
    std::optional<FilePayload> payload;
    bool flagDone = false, dataDone = false, invalid = false;
    std::string flag;
    std::function<void(std::optional<ClipboardFiles>)> callback;
  };
  auto read = std::make_shared<ReadFiles>();
  read->flagDone = !kde;
  read->callback = std::move(callback);
  const auto finish = [self, read, revision] {
    if (!read->flagDone || !read->dataDone || !read->callback) {
      return;
    }
    auto callback = std::exchange(read->callback, {});
    if (!self->active || self->epoch != revision || read->invalid || !read->payload) {
      callback(std::nullopt);
      return;
    }
    read->payload->cut = read->payload->cut || read->flag == "1";
    callback(ClipboardFiles{std::move(*read->payload), revision});
  };
  if (kde) {
    self->read(offer, kKdeCutMime.data(), self->reading.get(), [read, finish](auto bytes) {
      read->invalid = !bytes || (*bytes != "0" && *bytes != "1");
      if (bytes) {
        read->flag = std::move(*bytes);
      }
      read->flagDone = true;
      finish();
    });
  }
  self->read(offer, mime.data(), self->reading.get(), [read, finish, mime](auto bytes) {
    if (bytes) {
      if (auto payload = parseFilePayload(mime, *bytes)) {
        read->payload = std::move(*payload);
      }
    }
    read->dataDone = true;
    finish();
  });
}
bool luciole::WaylandTextClipboard::claimMove(std::uint64_t revision) {
  if (revision != m_state->epoch || m_state->moving == revision || m_state->reconciliation) {
    return false;
  }
  m_state->moving = revision;
  m_state->notify();
  return true;
}
void luciole::WaylandTextClipboard::finishMove(
    const ClipboardFiles& selection, const std::vector<TransferResult>& results
) {
  if (m_state->moving != selection.revision) {
    return;
  }
  m_state->moving.reset();
  if (m_state->epoch != selection.revision) {
    m_state->notify();
    return;
  }
  auto remaining = remainingCutFiles(selection.payload, results);
  if (remaining.locations.size() == selection.payload.locations.size()) {
    m_state->notify();
    return;
  }
  // Keep outgoing sources immutable. Ownership cancellation or a fresh foreign
  // offer retires this receipt before a later focus event can publish it.
  m_state->reconciliation = State::Reconciliation{selection.revision, m_state->owned, m_state->selected, remaining};
  if (m_state->owned) {
    m_state->files = std::move(remaining);
  }
  m_state->reconcile();
  m_state->notify();
}
void luciole::WaylandTextClipboard::keyboardFocusEntered() { m_state->focusEntered(); }
void luciole::WaylandTextClipboard::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::WaylandTextClipboard::startDrag(
    FilePayload payload, wl_surface* origin, std::uint32_t pressSerial, const LoadedImageFile* image,
    std::function<void()> finished, std::optional<DragAction> forcedAction,
    std::function<void(std::optional<DragAction>)> actionChanged
) {
  if (!m_state->device
      || wl_data_device_manager_get_version(m_state->manager) < 3
      || !origin
      || pressSerial == 0
      || std::ranges::any_of(m_state->sources, [](const auto& item) {
           return item.second.drag && !item.second.dropped;
         })) {
    return false;
  }
  auto bytes = encodeFilePayload(payload, kUriListMime);
  if (!bytes) {
    return false;
  }
  auto icon = DragIcon::create(m_state->compositor, m_state->shm, image);
  auto* iconSurface = icon ? icon->surface : nullptr;
  State::Source outgoing{
      .data = {{std::string(kUriListMime), std::move(*bytes)}},
      .drag = true,
      .icon = std::move(icon),
      .finished = std::move(finished),
      .files = std::move(payload),
      .actionChanged = std::move(actionChanged)
  };
  if (forcedAction == DragAction::Ask) {
    outgoing.data.emplace(kAskDropMime, "1");
  }
  auto* source = m_state->source(std::move(outgoing));
  const auto actions = forcedAction == DragAction::Copy ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
      : forcedAction == DragAction::Move                ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
                                                        : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
          | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
          | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
  wl_data_source_set_actions(source, actions);
  wl_data_device_start_drag(m_state->device, source, origin, iconSurface, pressSerial);
  if (iconSurface) {
    m_state->sources.at(source).icon->commit();
  }
  return true;
}
void luciole::WaylandTextClipboard::refreshDrag() { m_state->updateDrag(); }
void luciole::WaylandTextClipboard::cancelDrag() {
  for (auto it = m_state->sources.begin(); it != m_state->sources.end(); ++it) {
    if (it->second.drag && !it->second.dropped) {
      auto* source = it->first;
      auto finished = std::move(it->second.finished);
      m_state->sources.erase(it);
      wl_data_source_destroy(source);
      if (finished) {
        finished();
      }
      break;
    }
  }
}
const luciole::FilePayload* luciole::WaylandTextClipboard::dragFiles() const {
  for (const auto& [source, value] : m_state->sources) {
    (void)source;
    if (value.drag && !value.dropped && value.files) {
      return &*value.files;
    }
  }
  return nullptr;
}
void luciole::WaylandTextClipboard::setDropCallbacks(
    std::function<FileDropTarget(wl_surface*, float, float)> hover, std::function<void()> leave
) {
  m_state->hover = std::move(hover);
  m_state->leave = std::move(leave);
}
