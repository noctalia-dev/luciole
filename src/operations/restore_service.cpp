#include "operations/restore_service.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "operations/conflict_gate.h"
#include "operations/trash_item.h"

#include <atomic>
#include <set>
#include <unistd.h>

namespace {
  luciole::Location parentOf(const luciole::Location& location) {
    auto parent = location.parent();
    if (!parent) {
      luciole::throwFileError(luciole::FileErrorKind::Unavailable, "The original folder is unavailable.");
    }
    return std::move(*parent);
  }
  luciole::Location numbered(const luciole::Location& original, bool directory, std::size_t number) {
    const auto parent = parentOf(original);
    const auto base = original.basename().value_or(std::string{});
    const auto dot = base.find_last_of('.');
    const bool extension = !directory && dot != std::string::npos && dot != 0;
    auto stem = extension ? base.substr(0, dot) : base;
    const auto suffix = extension ? base.substr(dot) : std::string{};
    const auto counter = " (" + std::to_string(number) + ")";
    const auto limit = ::pathconf(parent.nativePath()->c_str(), _PC_NAME_MAX);
    if (limit > 0 && stem.size() + counter.size() + suffix.size() > static_cast<std::size_t>(limit)) {
      if (counter.size() + suffix.size() >= static_cast<std::size_t>(limit)) {
        luciole::throwFileError(luciole::FileErrorKind::NameTooLong, "The renamed file name would be too long.");
      }
      auto bytes = static_cast<std::size_t>(limit) - counter.size() - suffix.size();
      if (g_utf8_validate(stem.data(), static_cast<gssize>(stem.size()), nullptr)) {
        while (bytes > 0 && (static_cast<unsigned char>(stem[bytes]) & 0xc0U) == 0x80U) {
          --bytes;
        }
      }
      stem.resize(bytes);
    }
    return parent.child(stem + counter + suffix);
  }
  bool collision(luciole::FileErrorKind kind) {
    using Kind = luciole::FileErrorKind;
    return kind == Kind::Exists || kind == Kind::WouldMerge || kind == Kind::IsDirectory;
  }
  luciole::RestoreOutcome failedMove(luciole::FileErrorKind kind) {
    using Kind = luciole::FileErrorKind;
    switch (kind) {
    case Kind::Missing:
    case Kind::Denied:
    case Kind::Unsupported:
    case Kind::ReadOnly:
    case Kind::InvalidArgument:
    case Kind::NameTooLong:
    case Kind::WouldRecurse:
    case Kind::NotDirectory:
    case Kind::NoSpace:
      return luciole::RestoreOutcome::Failed;
    default:
      return luciole::RestoreOutcome::Unconfirmed;
    }
  }
} // namespace

struct luciole::RestoreService::State {
  CancelToken token;
  bool active = false;
  std::size_t total = 0;
  std::atomic<std::size_t> completed{0};
  std::vector<RestoreResult> results;
  std::function<void()> changed;
  ConflictGate<TransferConflict, TransferConflictChoice> gate;

  static void notify(const std::shared_ptr<State>& owner) {
    auto* source = g_idle_source_new();
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          const auto& state = *static_cast<std::shared_ptr<State>*>(data);
          const auto callback = state->changed;
          if (callback) {
            callback();
          }
          return G_SOURCE_REMOVE;
        },
        new std::shared_ptr<State>(owner), [](gpointer data) { delete static_cast<std::shared_ptr<State>*>(data); }
    );
    g_source_attach(source, nullptr);
    g_source_unref(source);
  }
  static TransferConflictChoice
  ask(const std::shared_ptr<State>& owner, const CancelToken& token, const DirectoryEntry& entry,
      const Location& destination, const TrashItemInfo& existing) {
    const auto choice = owner->gate.ask(
        TransferConflict{
            0,
            entry.location(),
            destination,
            destination.displayName().value_or(std::string{}),
            entry.type == FileType::Directory,
            existing.type == FileType::Directory,
            entry.size,
            existing.size,
            entry.modifiedSeconds,
            existing.modified,
            false,
            false,
            {}
        },
        token, [&owner] { notify(owner); }
    );
    return choice.value_or(TransferConflictChoice::Cancel);
  }
  static void restore(
      const std::shared_ptr<State>& owner, const CancelToken& token, const DirectoryEntry& entry, RestoreResult& result
  ) {
    const auto expected = readTrashItemIdentity(entry);
    const auto& trash = entry.trashInfo();
    const auto source = entry.location();
    if (!isTopLevelTrashItem(source) || !trash.original || !trash.original->nativePath()) {
      throwFileError(FileErrorKind::InvalidArgument, "This item has no usable original location.");
    }
    const auto original = *trash.original;
    auto destination = original;
    const auto parent = parentOf(original);
    const auto parentInfo = queryTrashItem(parent, token, true);
    if (!parentInfo.id || parentInfo.type != FileType::Directory) {
      throwFileError(FileErrorKind::Unavailable, "The original folder is unavailable.");
    }
    std::optional<TrashItemIdentity> replace;
    std::size_t number = 0;
    for (std::size_t attempt = 0; attempt < 10000; ++attempt) {
      if (token.cancelled()) {
        return;
      }
      if (expected != queryTrashItem(source, token).identity()) {
        throwFileError(FileErrorKind::Changed, "The selected item changed. It was left in Trash.");
      }
      if (queryTrashItem(parent, token, true).id != parentInfo.id) {
        throwFileError(FileErrorKind::Changed, "The original folder changed. The item was left in Trash.");
      }
      if (replace && *replace != queryTrashItem(destination, token).identity()) {
        replace.reset();
      }
      result.destination = destination;
      if (token.cancelled()) {
        return;
      }
      GError* raw = nullptr;
      auto flags = static_cast<GFileCopyFlags>(
          G_FILE_COPY_NOFOLLOW_SYMLINKS
          | G_FILE_COPY_NO_FALLBACK_FOR_MOVE
          | (replace ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_NONE)
      );
      // Observe cancellation between complete provider moves, never midway through publication.
      const bool moved =
          g_file_move(gioFile(source).get(), gioFile(destination).get(), flags, nullptr, nullptr, nullptr, &raw);
      const GErrorPtr error(raw);
      if (moved) {
        result.outcome = RestoreOutcome::Restored;
        return;
      }
      auto failure = error ? fileError(*error)
                           : FileError{.kind = FileErrorKind::Other, .message = "Could not confirm restoration."};
      if (!collision(failure.kind)) {
        result.outcome = failedMove(failure.kind);
        result.error = std::move(failure);
        return;
      }
      replace.reset();
      if (number > 0) {
        destination = numbered(original, entry.type == FileType::Directory, ++number);
        continue;
      }
      const auto existing = queryTrashItem(destination, token);
      const auto choice = ask(owner, token, entry, destination, existing);
      if (choice == TransferConflictChoice::Cancel) {
        return;
      }
      if (choice == TransferConflictChoice::Skip) {
        result.outcome = RestoreOutcome::Skipped;
        return;
      }
      if (choice == TransferConflictChoice::KeepBoth) {
        destination = numbered(original, entry.type == FileType::Directory, ++number);
      } else if (entry.type != FileType::Directory && existing.type != FileType::Directory) {
        replace = existing.identity();
      } else {
        throwFileError(FileErrorKind::Unsupported, "Folder replacement is unavailable. Keep both or skip the item.");
      }
    }
    throwFileError(FileErrorKind::Exists, "Too many name collisions. The item was left in Trash.");
  }
};

luciole::RestoreService::RestoreService() : m_state(std::make_shared<State>()) {}
luciole::RestoreService::~RestoreService() {
  m_state->changed = {};
  cancel();
}
bool luciole::RestoreService::busy() const { return m_state->active; }
std::size_t luciole::RestoreService::total() const { return m_state->total; }
std::size_t luciole::RestoreService::completed() const { return m_state->completed.load(); }
const std::vector<luciole::RestoreResult>& luciole::RestoreService::results() const { return m_state->results; }
void luciole::RestoreService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void luciole::RestoreService::cancel() {
  m_state->token.cancel();
  m_state->gate.wake();
}
std::optional<luciole::TransferConflict> luciole::RestoreService::conflict() const { return m_state->gate.pending(); }
bool luciole::RestoreService::resolve(std::uint64_t id, TransferConflictChoice choice) {
  return m_state->gate.resolve(id, choice, [&](const TransferConflict&) {
    if (choice == TransferConflictChoice::Merge) {
      return false;
    }
    if (choice == TransferConflictChoice::Cancel) {
      m_state->token.cancel();
    }
    return true;
  });
}
bool luciole::RestoreService::start(std::vector<DirectoryEntry> selection) {
  if (busy() || selection.empty() || selection.size() > 4096) {
    return false;
  }
  std::set<std::string> sources;
  for (const auto& entry : selection) {
    const auto location = entry.location();
    if (!isTopLevelTrashItem(location) || !sources.insert(location.uri()).second) {
      return false;
    }
  }
  m_state->active = true;
  m_state->total = selection.size();
  m_state->completed.store(0);
  m_state->results.clear();
  m_state->token = CancelToken{};
  runJob<std::vector<RestoreResult>>(
      [owner = m_state, token = m_state->token, selection = std::move(selection)] {
        std::vector<RestoreResult> results;
        std::set<std::string> seen;
        for (const auto& entry : selection) {
          results.push_back({entry.location(), std::nullopt, entry.displayName(), RestoreOutcome::Cancelled, {}});
        }
        for (std::size_t index = 0; index < selection.size(); ++index) {
          if (token.cancelled()) {
            break;
          }
          const auto& entry = selection[index];
          auto& result = results[index];
          try {
            if (!seen.insert(entry.location().uri()).second) {
              throwFileError(FileErrorKind::InvalidArgument, "The restore selection contains a repeated item.");
            }
            State::restore(owner, token, entry, result);
          } catch (const std::exception&) {
            if (!token.cancelled()) {
              result.outcome = RestoreOutcome::Failed;
              result.error = currentFileError();
            }
          }
          owner->completed.fetch_add(1);
        }
        return results;
      },
      [owner = m_state](JobResult<std::vector<RestoreResult>> reply) {
        if (reply) {
          owner->results = std::move(*reply);
        }
        owner->active = false;
        const auto callback = owner->changed;
        if (callback) {
          callback();
        }
      }
  );
  State::notify(m_state);
  return true;
}
