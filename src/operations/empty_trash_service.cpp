#include "operations/empty_trash_service.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "operations/transfer_copy.h"
#include "operations/trash_item.h"

#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace {
  using Targets = std::vector<luciole::TrashItem>;
  struct Size {
    std::optional<std::uint64_t> bytes = 0;
    bool incomplete = false;
  };
  Size measureSize(const Targets& targets, const luciole::CancelToken& token) {
    struct Folder {
      luciole::Location location;
      unsigned int depth;
    };
    Size reply;
    std::vector<Folder> pending;
    constexpr const char* attributes = "standard::name,standard::type,standard::is-symlink,standard::size";
    auto* cancellable = luciole::gioCancellable(token);
    const auto inspect = [&](const luciole::Location& location, GFileInfo* info, unsigned int depth) {
      const auto type = g_file_info_get_file_type(info);
      if (type == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_symlink(info)) {
        if (depth >= 256 || pending.size() >= 65536) {
          reply.incomplete = true;
        } else {
          pending.push_back({location, depth});
        }
      } else if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) == G_FILE_ATTRIBUTE_TYPE_UINT64) {
        const auto size = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
        if (size > std::numeric_limits<std::uint64_t>::max() - *reply.bytes) {
          reply.incomplete = true;
        } else {
          *reply.bytes += size;
        }
      } else {
        reply.incomplete = true;
      }
    };
    for (const auto& target : targets) {
      if (token.cancelled()) {
        reply.incomplete = true;
        return reply;
      }
      const luciole::GioPtr<GFileInfo> info(g_file_query_info(
          luciole::gioFile(target.location).get(), attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
      ));
      if (info) {
        inspect(target.location, info.get(), 0);
      } else {
        reply.incomplete = true;
      }
    }
    while (!pending.empty()) {
      if (token.cancelled()) {
        reply.incomplete = true;
        return reply;
      }
      const auto folder = std::move(pending.back());
      pending.pop_back();
      const luciole::GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
          luciole::gioFile(folder.location).get(), attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
      ));
      if (!enumerator) {
        reply.incomplete = true;
        continue;
      }
      for (;;) {
        GError* raw = nullptr;
        const luciole::GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), cancellable, &raw));
        const luciole::GErrorPtr error(raw);
        if (!info) {
          reply.incomplete = reply.incomplete || error != nullptr;
          break;
        }
        const char* name = g_file_info_get_name(info.get());
        if (name == nullptr
            || *name == '\0'
            || std::string_view(name).contains('/')
            || std::string_view(name) == "."
            || std::string_view(name) == "..") {
          reply.incomplete = true;
          continue;
        }
        inspect(folder.location.child(name), info.get(), folder.depth + 1);
      }
      if (!g_file_enumerator_close(enumerator.get(), cancellable, nullptr)) {
        reply.incomplete = true;
      }
    }
    return reply;
  }
  std::vector<luciole::EmptyTrashResult> pendingResults(const Targets& targets) {
    std::vector<luciole::EmptyTrashResult> results;
    for (const auto& target : targets) {
      results.push_back({target.location, target.displayName, luciole::EmptyTrashOutcome::Cancelled, {}});
    }
    return results;
  }
  luciole::EmptyTrashOutcome failedDelete(luciole::FileErrorKind kind) {
    using Kind = luciole::FileErrorKind;
    using Outcome = luciole::EmptyTrashOutcome;
    switch (kind) {
    case Kind::Missing:
      return Outcome::Skipped;
    case Kind::Denied:
    case Kind::Unsupported:
    case Kind::ReadOnly:
    case Kind::NotEmpty:
    case Kind::InvalidArgument:
      return Outcome::Failed;
    default:
      return Outcome::Unconfirmed;
    }
  }
  // Deletes name inside parent through descriptors, so a path swapped in by another process is never followed.
  void removeTree(int parent, const std::string& name) {
    using namespace luciole::transfer;
    const bool folder = S_ISDIR(info(parent, name).st_mode);
    if (folder) {
      const auto fd = directory(parent, name);
      for (const auto& child : children(fd.get())) {
        removeTree(fd.get(), child);
      }
    }
    if (::unlinkat(parent, name.c_str(), folder ? AT_REMOVEDIR : 0) != 0) {
      fail("Could not delete item");
    }
  }
} // namespace

struct luciole::EmptyTrashService::State {
  EmptyTrashSnapshot snapshot;
  std::shared_ptr<Targets> targets;
  CancelToken token;
  std::atomic<std::size_t> completed{0};
  bool active = false;
  bool emptyRequested = false;
  bool immediate = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  template <typename Reply> using Done = void (*)(const std::shared_ptr<State>&, const CancelToken&, JobResult<Reply>);
  // Runs work under the current token; done receives that same token.
  template <typename Reply>
  static void launch(const std::shared_ptr<State>& owner, std::move_only_function<Reply()> work, Done<Reply> done) {
    owner->active = true;
    const auto token = owner->token;
    runJob<Reply>(token, std::move(work), [owner, token, done](JobResult<Reply> result) {
      owner->active = false;
      done(owner, token, std::move(result));
    });
    owner->notify();
  }
  static void prepared(const std::shared_ptr<State>& owner, const CancelToken& token, JobResult<Targets> reply) {
    owner->snapshot.error = reply ? std::nullopt : std::optional(reply.error());
    if (token.cancelled()) {
      owner->snapshot.status = EmptyTrashStatus::Cancelled;
      owner->targets.reset();
    } else if (!reply) {
      owner->snapshot.status = EmptyTrashStatus::Error;
      owner->targets.reset();
    } else {
      owner->targets = std::make_shared<Targets>(std::move(*reply));
      owner->snapshot.results = pendingResults(*owner->targets);
      if (owner->immediate) {
        owner->snapshot.status = EmptyTrashStatus::Emptying;
        startEmpty(owner);
        return;
      }
      owner->snapshot.status = EmptyTrashStatus::Ready;
      launch<Size>(owner, [targets = owner->targets, token] { return measureSize(*targets, token); }, measured);
      return;
    }
    owner->notify();
  }
  static void measured(const std::shared_ptr<State>& owner, const CancelToken& token, JobResult<Size> reply) {
    if (reply) {
      owner->snapshot.size = reply->bytes;
      owner->snapshot.sizeIncomplete = reply->incomplete;
    } else {
      // A job skipped by cancellation reports nothing rather than an incomplete size.
      owner->snapshot.size = std::nullopt;
      owner->snapshot.sizeIncomplete = reply.error().kind != FileErrorKind::Cancelled;
    }
    if (std::exchange(owner->emptyRequested, false)) {
      owner->token = CancelToken{};
      startEmpty(owner);
      return;
    }
    if (token.cancelled()) {
      owner->snapshot.status = EmptyTrashStatus::Cancelled;
      owner->targets.reset();
    }
    owner->notify();
  }
  static void emptied(
      const std::shared_ptr<State>& owner, const CancelToken& token, JobResult<std::vector<EmptyTrashResult>> reply
  ) {
    owner->targets.reset();
    if (reply) {
      owner->snapshot.error.reset();
      owner->snapshot.results = std::move(*reply);
      owner->snapshot.status = EmptyTrashStatus::Finished;
    } else {
      owner->snapshot.error = reply.error();
      owner->snapshot.status = token.cancelled() ? EmptyTrashStatus::Cancelled : EmptyTrashStatus::Error;
    }
    owner->notify();
  }
  static void startEmpty(const std::shared_ptr<State>& owner);
};

luciole::EmptyTrashService::EmptyTrashService() : m_state(std::make_shared<State>()) {}
luciole::EmptyTrashService::~EmptyTrashService() {
  m_state->changed = {};
  cancel();
}
void luciole::EmptyTrashService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::EmptyTrashService::busy() const { return m_state->active; }
bool luciole::EmptyTrashService::pending() const {
  return busy() || m_state->snapshot.status == EmptyTrashStatus::Ready;
}
bool luciole::EmptyTrashService::canEmpty() const {
  return m_state->snapshot.status == EmptyTrashStatus::Ready && m_state->targets && !m_state->targets->empty();
}
std::size_t luciole::EmptyTrashService::completed() const { return m_state->completed.load(); }
const luciole::EmptyTrashSnapshot& luciole::EmptyTrashService::snapshot() const { return m_state->snapshot; }
void luciole::EmptyTrashService::cancel() {
  m_state->emptyRequested = false;
  m_state->token.cancel();
  m_state->targets.reset();
  if (!busy() && m_state->snapshot.status != EmptyTrashStatus::Finished) {
    m_state->snapshot.status = EmptyTrashStatus::Cancelled;
  }
}
bool luciole::EmptyTrashService::prepare() { return prepareTargets(std::nullopt); }
bool luciole::EmptyTrashService::prepareSelection(std::vector<DirectoryEntry> selection) {
  if (selection.empty() || selection.size() > 4096) {
    return false;
  }
  return prepareTargets(std::move(selection));
}
bool luciole::EmptyTrashService::prepareDeletion(std::vector<DirectoryEntry> selection) {
  if (selection.empty() || selection.size() > 4096) {
    return false;
  }
  return prepareTargets(std::move(selection), true);
}
bool luciole::EmptyTrashService::deleteSelection(std::vector<DirectoryEntry> selection) {
  if (selection.empty() || selection.size() > 4096) {
    return false;
  }
  const bool outsideTrash = !selection.front().location().isTrash();
  return prepareTargets(std::move(selection), outsideTrash, true);
}
bool luciole::EmptyTrashService::prepareTargets(
    std::optional<std::vector<DirectoryEntry>> selection, bool outsideTrash, bool immediate
) {
  if (busy()) {
    return false;
  }
  m_state->targets.reset();
  m_state->immediate = immediate;
  m_state->token = CancelToken{};
  m_state->completed.store(0);
  m_state->snapshot = {
      .status = EmptyTrashStatus::Preparing,
      .results = {},
      .error = {},
      .size = std::nullopt,
      .sizeIncomplete = false,
      .selection = selection.has_value(),
      .outsideTrash = outsideTrash
  };
  State::launch<Targets>(
      m_state,
      [token = m_state->token, selection = std::move(selection), outsideTrash] {
        if (!selection) {
          return listTrashItems(token);
        }
        Targets targets;
        std::set<std::string> seen;
        for (const auto& entry : *selection) {
          const auto location = entry.location();
          if (!(outsideTrash ? location.isNative() && location.parent().has_value() : isTopLevelTrashItem(location))
              || !seen.insert(location.uri()).second) {
            throwFileError(FileErrorKind::InvalidArgument, "Select distinct items from a supported folder.");
          }
          const auto current = queryTrashItem(location, token);
          const auto expected = readTrashItemIdentity(entry);
          if (expected != current.identity()) {
            throwFileError(FileErrorKind::Changed, "The selected item changed. Select it again before deleting it.");
          }
          targets.push_back({location, entry.displayName(), expected});
        }
        return targets;
      },
      State::prepared
  );
  return true;
}
bool luciole::EmptyTrashService::empty() {
  if (!canEmpty()) {
    return false;
  }
  m_state->snapshot.status = EmptyTrashStatus::Emptying;
  if (busy()) {
    m_state->emptyRequested = true;
    m_state->token.cancel();
    m_state->notify();
  } else {
    m_state->token = CancelToken{};
    State::startEmpty(m_state);
  }
  return true;
}
void luciole::EmptyTrashService::State::startEmpty(const std::shared_ptr<State>& owner) {
  launch<std::vector<EmptyTrashResult>>(
      owner,
      [owner, token = owner->token, targets = owner->targets, outsideTrash = owner->snapshot.outsideTrash] {
        auto results = pendingResults(*targets);
        for (std::size_t index = 0; index < targets->size(); ++index) {
          if (token.cancelled()) {
            break;
          }
          const auto& target = (*targets)[index];
          auto& result = results[index];
          try {
            // Pin the confirmed item before checking it, so a later swap at its path is detected.
            Fd parent;
            std::string name;
            struct stat pinned{};
            if (outsideTrash) {
              const std::filesystem::path path(*target.location.nativePath());
              name = path.filename().native();
              parent = transfer::opened(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
              pinned = transfer::info(parent.get(), name);
            }
            if (target.identity != queryTrashItem(target.location, token).identity()
                || (outsideTrash && !sameIdentity(pinned, transfer::info(parent.get(), name)))) {
              result.outcome = EmptyTrashOutcome::Failed;
              result.error = FileError{
                  .kind = FileErrorKind::Changed,
                  .message = "The item changed after confirmation was prepared. Select it again before deleting it."
              };
            } else {
              if (token.cancelled()) {
                break;
              }
              if (outsideTrash) {
                try {
                  removeTree(parent.get(), name);
                  result.outcome = EmptyTrashOutcome::Deleted;
                } catch (const std::exception&) {
                  result.error = currentFileError();
                  result.outcome = EmptyTrashOutcome::Unconfirmed;
                }
                owner->completed.fetch_add(1);
                continue;
              }
              GError* raw = nullptr;
              // Finish the current provider request before observing cancellation.
              const bool removed = g_file_delete(gioFile(target.location).get(), nullptr, &raw);
              const GErrorPtr error(raw);
              if (removed) {
                result.outcome = EmptyTrashOutcome::Deleted;
              } else {
                result.error = error
                    ? fileError(*error)
                    : FileError{.kind = FileErrorKind::Other, .message = "Could not confirm deletion."};
                result.outcome = failedDelete(result.error->kind);
              }
            }
          } catch (const std::exception&) {
            auto error = currentFileError();
            if (error.kind == FileErrorKind::Cancelled) {
              break;
            }
            result.outcome =
                error.kind == FileErrorKind::Missing ? EmptyTrashOutcome::Skipped : EmptyTrashOutcome::Failed;
            result.error = std::move(error);
          }
          owner->completed.fetch_add(1);
        }
        return results;
      },
      emptied
  );
}
