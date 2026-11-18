#include "operations/move_receipt.h"

#include "fs/background_job.h"
#include "fs/file_error.h"
#include "fs/posix_fd.h"
#include "operations/copy_history_tree.h"
#include "operations/copy_receipt.h"
#include "operations/history_mount.h"
#include "operations/history_revisions.h"
#include "operations/move_history_internal.h"
#include "operations/transfer_copy.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <optional>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>

bool luciole::MoveFolder::usesMount(const HistoryMount& mount) const {
  return (m_from && mount.contains(m_from->get()))
      || (m_target && mount.contains(m_target->get()))
      || (m_item && m_item->usesMount(mount));
}

bool luciole::MoveEffect::usesMount(const HistoryMount& mount) const {
  return mount.contains(m_identity->from.get())
      || mount.contains(m_identity->to.get())
      || mount.contains(m_identity->item.get())
      || (m_identity->root && mount.contains(m_identity->root->parent.get()))
      || (m_previous && m_previous->usesMount(mount))
      || (m_retained && m_retained->usesMount(mount));
}

bool luciole::MoveReceipt::usesMount(const HistoryMount& mount) const {
  return std::ranges::any_of(m_effects, [&](const auto& effect) { return effect->usesMount(mount); })
      || std::ranges::any_of(m_folders, [&](const auto& folder) { return folder->usesMount(mount); });
}

namespace {
  std::atomic<std::size_t> pendingMoveCleanup = 0;
  std::shared_ptr<const luciole::MoveIdentity> ownIdentity(
      luciole::Fd from, luciole::Fd to, luciole::Fd item, std::shared_ptr<luciole::HistoryRevisions> revisions,
      std::shared_ptr<const luciole::MoveHistoryRoot> root
  ) {
    auto identity = std::make_unique<luciole::MoveIdentity>(
        std::move(from), std::move(to), std::move(item), std::move(revisions), std::move(root)
    );
    return std::shared_ptr<const luciole::MoveIdentity>(
        identity.release(), [worker = std::this_thread::get_id()](const luciole::MoveIdentity* retired) {
          auto owned = std::unique_ptr<const luciole::MoveIdentity>(retired);
          if (std::this_thread::get_id() == worker) {
            return;
          }
          ++pendingMoveCleanup;
          luciole::detail::runInThread(
              [owned = std::move(owned)] mutable { owned.reset(); }, [] { --pendingMoveCleanup; }
          );
        }
    );
  }
  luciole::Fd currentParent(const luciole::Location& location, int held, const luciole::MoveParents& parents = {}) {
    auto parent = location.parent();
    if (!parent || !parent->isNative()) {
      luciole::throwFileError(
          luciole::FileErrorKind::InvalidArgument, "Move history requires native containing folders."
      );
    }
    if (const auto planned = parents.find(parent->uri()); planned != parents.end()) {
      if (!luciole::sameIdentity(luciole::statFd(planned->second), luciole::statFd(held))) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "The retained source parent changed.");
      }
      return luciole::checkedFd(::fcntl(held, F_DUPFD_CLOEXEC, 0));
    }
    auto current = luciole::checkedFd(::open(parent->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!luciole::sameIdentity(luciole::statFd(current.get()), luciole::statFd(held))) {
      luciole::throwFileError(
          luciole::FileErrorKind::Changed, "A containing folder changed. Move history is no longer valid."
      );
    }
    return current;
  }
  struct stat namedStamp(int parent, const luciole::Location& location) {
    struct stat value{};
    const auto name = location.basename();
    if (!name || name->empty() || *name == "." || *name == "..") {
      luciole::throwFileError(luciole::FileErrorKind::InvalidArgument, "Invalid move history name.");
    }
    if (::fstatat(parent, name->c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
      luciole::throwErrno(errno);
    }
    return value;
  }
  bool invalidates(const luciole::FileError& error) {
    return error.kind == luciole::FileErrorKind::Changed
        || error.kind == luciole::FileErrorKind::Missing
        || error.kind == luciole::FileErrorKind::NotDirectory;
  }
  std::optional<std::vector<std::string>> relativePath(const luciole::Location& ancestor, luciole::Location location) {
    std::vector<std::string> path;
    while (path.size() <= 64) {
      if (location == ancestor) {
        std::ranges::reverse(path);
        return path;
      }
      const auto parent = location.parent();
      const auto name = location.basename();
      if (!parent || !name) {
        break;
      }
      path.push_back(*name);
      location = *parent;
    }
    return std::nullopt;
  }
  void checkCancelled(const luciole::CancelToken& token) {
    if (token.cancelled()) {
      luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Move reversal cancelled.");
    }
  }
} // namespace

bool luciole::MoveReceipt::cleanupPending() { return pendingMoveCleanup != 0; }

luciole::MoveEffect::MoveEffect(
    Location source, Location destination, std::shared_ptr<const MoveIdentity> identity, struct stat revision,
    bool reversed, std::shared_ptr<const CopyEffect> previous, std::shared_ptr<const CopyEffect> retained,
    std::shared_ptr<const MoveTrees> trees, std::shared_ptr<const MoveTrees> previousTrees
)
    : m_source(std::move(source)), m_destination(std::move(destination)), m_identity(std::move(identity)),
      m_revision(revision), m_reversed(reversed), m_previous(std::move(previous)), m_retained(std::move(retained)),
      m_trees(std::move(trees)), m_previousTrees(std::move(previousTrees)) {}

const std::shared_ptr<const luciole::MoveHistoryRoot>& luciole::MoveEffect::retentionRoot() const {
  return m_identity->root;
}
int luciole::MoveEffect::retentionParent() const { return m_identity->root ? m_identity->root->parent.get() : -1; }
int luciole::MoveEffect::sourceParent() const { return m_reversed ? m_identity->to.get() : m_identity->from.get(); }
int luciole::MoveEffect::destinationParent() const {
  return m_reversed ? m_identity->from.get() : m_identity->to.get();
}

void luciole::MoveEffect::validate(
    const struct stat& revision, const MoveParents& parents, bool moving, const CancelToken& token
) const {
  checkCancelled(token);
  (void)currentParent(m_source, sourceParent(), parents);
  const auto destination = currentParent(m_destination, destinationParent(), parents);
  if (m_previous) {
    m_previous->validate(token, parents);
  }
  if (!m_trees) {
    m_identity->revisions->validate(revision, statFd(m_identity->item.get()));
  }
  if (m_retained) {
    m_retained->validate(token, parents);
    if (!sameIdentity(statFd(m_identity->item.get()), statFd(m_retained->item()))) {
      throwFileError(FileErrorKind::Changed, "The retained incoming move changed.");
    }
  } else {
    if (m_trees) {
      const auto& tree = moving || m_reversed ? m_trees->before : m_trees->after;
      CopyHistoryTree proof(*tree, m_trees->changed);
      (void)proof.finish(destination.get(), *m_destination.basename(), *m_identity->revisions, token);
      if (!sameFileRevision(statFd(m_identity->item.get()), namedStamp(destination.get(), m_destination))) {
        throwFileError(FileErrorKind::Changed, "The moved folder changed during validation.");
      }
    } else {
      m_identity->revisions->validate(revision, namedStamp(destination.get(), m_destination));
    }
  }
}

void luciole::MoveEffect::requireVacant(const MoveParents& parents) const {
  const auto target = currentParent(m_source, sourceParent(), parents);
  struct stat value{};
  if (::fstatat(target.get(), m_source.basename()->c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0) {
    throwFileError(
        FileErrorKind::Exists, "The return name is occupied: " + m_source.displayName().value_or("Item") + "."
    );
  }
  if (errno != ENOENT) {
    throwErrno(errno);
  }
}

std::shared_ptr<const luciole::MoveEffect> luciole::MoveEffect::capture(
    const Location& source, const Location& destination, int from, int to, int item,
    std::shared_ptr<HistoryRevisions> revisions, std::shared_ptr<const MoveHistoryRoot> root
) {
  if (source == destination || source.isTrash() || destination.isTrash()) {
    throwFileError(FileErrorKind::InvalidArgument, "Invalid native move history.");
  }
  if (!revisions) {
    revisions = std::make_shared<HistoryRevisions>();
  }
  const auto guard = revisions->lock();
  auto proof = ownIdentity(
      checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(to, F_DUPFD_CLOEXEC, 0)),
      checkedFd(::fcntl(item, F_DUPFD_CLOEXEC, 0)), revisions, std::move(root)
  );
  auto effect = std::shared_ptr<MoveEffect>(new MoveEffect(source, destination, std::move(proof), statFd(item), false));
  effect->validate(effect->m_revision);
  return effect;
}

std::shared_ptr<const luciole::MoveEffect> luciole::MoveEffect::prepareReplacement(
    const Location& source, const Location& destination, int from, int to, std::size_t& remaining,
    const CancelToken& token, std::shared_ptr<HistoryRevisions> revisions, std::shared_ptr<const MoveHistoryRoot> root
) {
  if (!revisions) {
    revisions = std::make_shared<HistoryRevisions>();
  }
  const auto guard = revisions->lock();
  auto item = checkedFd(::openat(from, source.basename()->c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
  if (statFd(item.get()).st_dev != statFd(to).st_dev) {
    return {};
  }
  auto previous = CopyEffect::prepareAt(destination, to, to, *destination.basename(), remaining, token, revisions);
  auto identity = ownIdentity(
      checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(to, F_DUPFD_CLOEXEC, 0)), std::move(item),
      revisions, std::move(root)
  );
  const auto revision = statFd(identity->item.get());
  return std::shared_ptr<const MoveEffect>(
      new MoveEffect(source, destination, std::move(identity), revision, false, std::move(previous))
  );
}

std::shared_ptr<const luciole::MoveEffect> luciole::MoveEffect::publishedReplacement() const {
  m_identity->revisions->renamed(m_revision, statFd(m_identity->item.get()));
  auto expected = m_identity->revisions->expected(m_revision);
  if (!sameFileRevision(expected, statFd(m_identity->item.get()))
      || !sameFileRevision(expected, namedStamp(destinationParent(), m_destination))) {
    throwFileError(FileErrorKind::Changed, "The incoming move changed during replacement. Inspect both locations.");
  }
  auto previous = m_previous->retainedFrom(
      sourceParent(), *m_source.basename(), m_identity->root ? m_identity->root->parent.get() : -1
  );
  return std::shared_ptr<const MoveEffect>(
      new MoveEffect(m_source, m_destination, m_identity, expected, false, std::move(previous))
  );
}

bool luciole::MoveEffect::encloses(const Location& location) const {
  return S_ISDIR(m_revision.st_mode) && relativePath(m_destination, location).has_value();
}

std::shared_ptr<const luciole::MoveEffect> luciole::MoveEffect::captureContents(
    std::size_t& remaining, const CancelToken& token, const MoveParents& parents
) const {
  auto result = std::shared_ptr<MoveEffect>(new MoveEffect(*this));
  if (!m_trees && S_ISDIR(m_revision.st_mode)) {
    validate(m_revision, parents, false, token);
    auto proof = CopyEffect::prepareAt(
        m_destination, destinationParent(), destinationParent(), *m_destination.basename(), remaining, token,
        m_identity->revisions, parents
    );
    auto trees = std::make_shared<MoveTrees>();
    trees->before = trees->after = proof->m_tree;
    result->m_trees = std::move(trees);
  }
  return result;
}

std::vector<std::shared_ptr<const luciole::MoveEffect>> luciole::MoveReceipt::compose(
    const std::vector<std::shared_ptr<const MoveEffect>>& effects, bool refresh, const CancelToken& token,
    const std::vector<std::shared_ptr<const CopyEffect>>& copies
) {
  auto result = effects;
  std::size_t remaining = 16384;
  for (std::size_t i = 0; i < effects.size(); ++i) {
    const auto& effect = effects[i];
    if (!effect->m_trees || (effect->m_trees->complete && !refresh && copies.empty())) {
      continue;
    }
    CopyHistoryTree draft(*effect->m_trees->before);
    for (std::size_t j = i + 1; j < effects.size(); ++j) {
      const auto& child = effects[j];
      const auto path = relativePath(effect->m_destination, child->m_destination);
      checkCancelled(token);
      if (!path) {
        if (relativePath(child->m_destination, effect->m_destination)) {
          break; // A later replacement carries this version into retained storage.
        }
        continue;
      }
      if (path->empty() && child->m_previous) {
        break;
      }
      if (path->empty() || child->m_retained || child->m_reversed) {
        throwFileError(FileErrorKind::Unsupported, "This ancestor replacement cannot supply Move history.");
      }
      const transfer::Tree leaf{.name = "item", .metadata = child->m_revision, .children = {}};
      if (S_ISDIR(child->m_revision.st_mode) && !child->m_trees) {
        throwFileError(FileErrorKind::Changed, "An overlapping moved folder has no content proof.");
      }
      // Physical predecessors validate in whole-action preflight, including retained-parent bindings.
      draft.replace(
          *path, child->m_trees ? *child->m_trees->before : leaf,
          child->m_previous ? child->m_previous->m_tree.get() : nullptr, *effect->m_identity->revisions, token
      );
    }
    for (const auto& copied : copies) {
      checkCancelled(token);
      const auto path = relativePath(effect->m_destination, copied->m_destination);
      if (!path) {
        continue;
      }
      if (path->empty()) {
        throwFileError(FileErrorKind::Unsupported, "Mixed parent replacement history is unavailable.");
      }
      draft.replace(
          *path, *copied->m_tree, copied->m_previous ? copied->m_previous->m_tree.get() : nullptr,
          *effect->m_identity->revisions, token
      );
    }
    auto trees = std::make_shared<MoveTrees>(*effect->m_trees);
    trees->changed = draft.changedFolders();
    auto directory = checkedFd(
        ::open(
            ("/proc/self/fd/" + std::to_string(effect->m_identity->item.get())).c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC
        )
    );
    trees->after = std::make_shared<const transfer::Tree>(
        draft.finish(directory.get(), ".", *effect->m_identity->revisions, token, &remaining)
    );
    trees->complete = true;
    auto next = std::shared_ptr<MoveEffect>(new MoveEffect(*effect));
    next->m_trees = std::move(trees);
    result[i] = std::move(next);
  }
  return result;
}

std::vector<luciole::Location> luciole::MoveEffect::retainForRecovery() const {
  auto paths = m_previous ? m_previous->retainForRecovery() : std::vector<Location>{};
  if (m_retained) {
    auto extra = m_retained->retainForRecovery();
    paths.insert(paths.end(), extra.begin(), extra.end());
  }
  return paths;
}
std::vector<luciole::Location> luciole::MoveFolder::retainForRecovery() const { return m_item->retainForRecovery(); }

std::shared_ptr<const luciole::MoveFolder> luciole::MoveFolder::retain(
    const Location& source, int from, const Location& folder, int target, const CancelToken& token,
    std::shared_ptr<HistoryRevisions> revisions, const std::shared_ptr<const MoveHistoryRoot>& root
) {
  if (!revisions) {
    revisions = std::make_shared<HistoryRevisions>();
  }
  const auto guard = revisions->lock();
  const auto name = *source.basename();
  const auto retainingFolder = root ? root->location : folder;
  if (root) {
    target = root->parent.get();
  }
  if (transfer::info(from, name).st_dev != statFd(target).st_dev || !sameMount(from, target)) {
    return {};
  }
  std::size_t remaining = 1;
  auto proof = CopyEffect::prepareAt(retainingFolder.child(name), target, from, name, remaining, token, revisions);
  auto result = std::shared_ptr<MoveFolder>(new MoveFolder(source, retainingFolder));
  result->m_from = std::make_shared<const Fd>(checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)));
  result->m_target = std::make_shared<const Fd>(checkedFd(::fcntl(target, F_DUPFD_CLOEXEC, 0)));
  result->m_item = proof->retainedFrom(from, name);
  return result;
}

luciole::MoveParents
luciole::MoveReceipt::parents(const CancelToken& token, const std::vector<std::pair<Location, int>>& extra) const {
  MoveParents result;
  for (const auto& folder : m_folders) {
    if (folder->m_item->m_hidden) {
      result.emplace(folder->m_source.uri(), folder->m_item->item());
    }
  }
  if (!m_undone || m_sequence) {
    return result;
  }
  std::map<std::string, const MoveEffect*> ancestors;
  for (const auto& effect : m_effects) {
    if (effect->m_trees && effect->m_reversed && !effect->m_trees->changed.empty()) {
      ancestors.emplace(effect->m_source.uri(), effect.get());
    }
  }
  const auto bind = [&](const Location& location, int held) {
    checkCancelled(token);
    auto parent = std::optional(location);
    for (unsigned depth = 0; parent && depth <= 64; ++depth, parent = parent->parent()) {
      const auto owner = ancestors.find(parent->uri());
      if (owner == ancestors.end()) {
        continue;
      }
      const auto* ancestor = owner->second;
      const auto path = *relativePath(ancestor->m_source, location);
      auto actual = checkedFd(::fcntl(ancestor->m_identity->item.get(), F_DUPFD_CLOEXEC, 0));
      for (const auto& name : path) {
        checkCancelled(token);
        actual = transfer::directory(actual.get(), name);
      }
      if (!sameIdentity(statFd(actual.get()), statFd(held))) {
        throwFileError(FileErrorKind::Changed, "A future Move parent changed.");
      }
      result[location.uri()] = held;
      return;
    }
  };
  for (const auto& effect : m_effects) {
    bind(*effect->m_source.parent(), effect->sourceParent());
    bind(*effect->m_destination.parent(), effect->destinationParent());
    for (const auto& previous : {effect->m_previous, effect->m_retained}) {
      if (previous) {
        bind(*previous->m_destination.parent(), previous->parent());
      }
    }
  }
  for (const auto& [location, held] : extra) {
    bind(*location.parent(), held);
  }
  // Every projected parent derives from a pinned root whose exact tree validates before replay.
  for (const auto& [location, ancestor] : ancestors) {
    (void)location;
    ancestor->validate(ancestor->m_revision, result, false, token);
  }
  return result;
}

std::vector<std::shared_ptr<const luciole::MoveEffect>>
luciole::MoveReceipt::coalesce(const std::vector<std::shared_ptr<const MoveEffect>>& effects) {
  if (std::ranges::any_of(effects, [](const auto& effect) { return effect->m_retained || effect->m_reversed; })) {
    return effects;
  }
  std::vector<std::shared_ptr<const MoveEffect>> result;
  result.reserve(effects.size());
  std::map<std::string, std::size_t> latest;
  for (const auto& effect : effects) {
    if (const auto found = latest.find(effect->m_destination.uri()); found != latest.end()) {
      const auto prior = result[found->second];
      const auto displaced = effect->m_previous;
      if (!displaced || !sameIdentity(statFd(prior->m_identity->item.get()), statFd(displaced->item()))) {
        throwFileError(FileErrorKind::Changed, "A repeated move destination lost its previous version.");
      }
      displaced->validate({});
      prior->m_identity->revisions->validate(prior->m_revision, statFd(displaced->item()));
      result[found->second] = std::shared_ptr<const MoveEffect>(new MoveEffect(
          prior->m_source, prior->m_destination, prior->m_identity, prior->m_revision, false, {}, displaced,
          prior->m_trees
      ));
      found->second = result.size();
      result.push_back(
          std::shared_ptr<const MoveEffect>(new MoveEffect(
              effect->m_source, effect->m_destination, effect->m_identity, effect->m_revision, false, prior->m_previous,
              {}, effect->m_trees
          ))
      );
    } else {
      latest.emplace(effect->m_destination.uri(), result.size());
      result.push_back(effect);
    }
  }
  return result;
}

std::shared_ptr<const luciole::MoveReceipt> luciole::MoveReceipt::capture(
    const std::vector<std::shared_ptr<const MoveEffect>>& effects,
    const std::vector<std::shared_ptr<const MoveFolder>>& folders, bool undone, bool sequence, const CancelToken& token,
    const std::vector<std::shared_ptr<const CopyEffect>>& copies
) {
  if (effects.size() + folders.size() == 0 || effects.size() + folders.size() > 4096) {
    return {};
  }
  auto receipt = std::make_shared<MoveReceipt>();
  receipt->m_revisions =
      !effects.empty() ? effects.front()->m_identity->revisions : folders.front()->m_item->revisions();
  const auto guard = receipt->m_revisions->lock();
  receipt->m_folders = folders;
  receipt->m_undone = undone;
  receipt->m_sequence = sequence || needsSequence(effects, token);
  if (receipt->m_sequence && !copies.empty()) {
    throwFileError(FileErrorKind::Unsupported, "Mixed ancestor replacement history is unavailable.");
  }
  receipt->m_effects = receipt->m_sequence ? (sequence ? effects : prepareSequence(effects, token))
                                           : coalesce(compose(effects, false, token, copies));
  if (receipt->m_sequence) {
    std::ranges::reverse(receipt->m_effects);
    receipt->validateSequence(token);
    receipt->m_generation = receipt->m_revisions->generation();
    return receipt;
  }
  std::stable_sort(receipt->m_effects.begin(), receipt->m_effects.end(), [undone](const auto& a, const auto& b) {
    const auto left = std::ranges::count((undone ? a->m_source : a->m_destination).uri(), '/');
    const auto right = std::ranges::count((undone ? b->m_source : b->m_destination).uri(), '/');
    return undone ? left < right : left > right;
  });
  const auto bindings = receipt->parents();
  for (const auto& folder : folders) {
    if (folder->m_item->revisions() != receipt->m_revisions) {
      throwFileError(FileErrorKind::Changed, "Merge history belongs to different jobs.");
    }
    folder->m_item->validate({});
    (void)currentParent(folder->m_retainingFolder.child("item"), folder->m_target->get());
    (void)currentParent(folder->m_source, folder->m_from->get(), bindings);
  }
  for (auto& effect : receipt->m_effects) {
    if (effect->m_identity->revisions != receipt->m_revisions) {
      throwFileError(FileErrorKind::Changed, "Move history belongs to different jobs.");
    }
    const auto expected =
        effect->m_trees ? statFd(effect->m_identity->item.get()) : receipt->m_revisions->expected(effect->m_revision);
    effect->validate(expected, bindings);
    effect = std::shared_ptr<const MoveEffect>(new MoveEffect(
        effect->m_source, effect->m_destination, effect->m_identity, expected, effect->m_reversed, effect->m_previous,
        effect->m_retained, effect->m_trees
    ));
  }
  receipt->m_generation = receipt->m_revisions->generation();
  return receipt;
}

void luciole::MoveReceipt::validateFolders(const MoveParents& bindings, const CancelToken& token) const {
  for (const auto& folder : m_folders) {
    checkCancelled(token);
    folder->m_item->validate(token);
    (void)currentParent(folder->m_retainingFolder.child("item"), folder->m_target->get());
    const auto parent = currentParent(folder->m_source, folder->m_from->get(), bindings);
    if (!m_undone) {
      struct stat occupied{};
      if (::fstatat(parent.get(), folder->m_source.basename()->c_str(), &occupied, AT_SYMLINK_NOFOLLOW) == 0) {
        throwFileError(FileErrorKind::Exists, "A merged source folder's return name is occupied.");
      }
      if (errno != ENOENT) {
        throwErrno(errno);
      }
    }
  }
}

bool luciole::MoveReceipt::ownsProof() const {
  return m_generation == m_revisions->generation()
      && std::ranges::all_of(
             m_effects,
             [](const auto& effect) {
               return (!effect->m_previous || effect->m_previous->ownsProof())
                   && (!effect->m_retained || effect->m_retained->ownsProof());
             }
      )
      && std::ranges::all_of(m_folders, [](const auto& folder) { return folder->m_item->ownsProof(); });
}

bool luciole::MoveReceipt::restoreFolders(
    std::vector<std::shared_ptr<const MoveFolder>>& folders, const CancelToken& token, MoveReplayResult& reply,
    std::atomic<std::size_t>& completed, bool& applied
) const {
  // Forward capture orders merged containers from children to parents. Restore parents first.
  for (std::size_t n = folders.size(); n > 0; --n) {
    auto& result = reply.results[m_effects.size() + n - 1];
    const auto& folder = folders[n - 1];
    bool changed = false;
    try {
      checkCancelled(token);
      auto restored = std::make_shared<MoveFolder>(*folder);
      result.source = folder->m_item->currentLocation();
      result.destination = folder->m_source;
      restored->m_item = folder->m_item->restoreTo(folder->m_source, folder->m_from->get(), changed);
      applied |= changed;
      folders[n - 1] = std::move(restored);
      result.outcome = TransferOutcome::Moved;
    } catch (const std::exception&) {
      applied |= changed;
      result.error = currentFileError();
      result.outcome = changed                             ? TransferOutcome::Partial
          : result.error->kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                           : TransferOutcome::Failed;
      return false;
    }
    completed.fetch_add(1);
  }
  return true;
}

luciole::MoveReplayResult
luciole::MoveReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  return reverseImpl(token, completed, nullptr);
}
luciole::MoveReplayResult luciole::MoveReceipt::reverseImpl(
    const CancelToken& token, std::atomic<std::size_t>& completed, const ReplayHooks* hooks
) const {
  const auto revisionGuard = m_revisions->lock();
  MoveReplayResult reply;
  reply.results.reserve(size());
  for (const auto& effect : m_effects) {
    reply.results.emplace_back(effect->m_destination, TransferOutcome::Skipped);
  }
  for (const auto& folder : m_folders) {
    reply.results.emplace_back(folder->m_source, TransferOutcome::Skipped);
  }
  std::unique_lock actionLock(*m_mutex, std::try_to_lock);
  std::vector<std::unique_lock<std::mutex>> locks;
  bool ownsProof = false;
  try {
    if (!actionLock.owns_lock()) {
      throwFileError(FileErrorKind::Other, "Another move reversal is using this history.");
    }
    for (const auto& effect : m_effects) {
      for (const auto& retained : {effect->m_previous, effect->m_retained}) {
        if (retained) {
          locks.emplace_back(retained->mutex(), std::try_to_lock);
          if (!locks.back().owns_lock()) {
            throwFileError(FileErrorKind::Other, "Retained move data is in use.");
          }
        }
      }
    }
    for (const auto& folder : m_folders) {
      locks.emplace_back(folder->m_item->mutex(), std::try_to_lock);
      if (!locks.back().owns_lock()) {
        throwFileError(FileErrorKind::Other, "Retained merge data is in use.");
      }
    }
    ownsProof = this->ownsProof();
    if (!ownsProof) {
      throwFileError(FileErrorKind::Changed, "The move history entry is stale.");
    }
    const auto bindings = parents(token);
    if (m_sequence) {
      validateSequence(token);
    } else {
      validateFolders(bindings, token);
      for (const auto& effect : m_effects) {
        checkCancelled(token);
        effect->validate(effect->m_revision, bindings, false, token);
        if (!effect->m_reversed || (!effect->m_previous && !effect->m_retained)) {
          effect->requireVacant(bindings);
        }
      }
    }
    if (hooks) {
      hooks->preflight(bindings);
    }
  } catch (const std::exception&) {
    const auto error = currentFileError();
    reply.retireHistory = invalidates(error);
    for (auto& result : reply.results) {
      result.outcome = error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled : TransferOutcome::Failed;
      result.error = error;
    }
    completed.store(size());
    if (reply.retireHistory && ownsProof) {
      retainIncomplete(reply.results);
    }
    return reply;
  }
  Revisions revisions;
  std::vector<std::shared_ptr<const MoveEffect>> inverse;
  std::vector<std::shared_ptr<const MoveFolder>> folders = m_folders;
  bool stopped = false, applied = false;
  if (!m_undone) {
    stopped = !restoreFolders(folders, token, reply, completed, applied);
  }
  if (!stopped && hooks && hooks->beforeMoves) {
    try {
      hooks->beforeMoves(applied);
    } catch (const std::exception&) {
      reply.retireHistory |= invalidates(currentFileError());
      stopped = true;
    }
  }
  for (std::size_t i = 0; i < m_effects.size() && !stopped; ++i) {
    auto& result = reply.results[i];
    auto effect = m_effects[i];
    bool changed = false;
    try {
      checkCancelled(token);
      effect = replayEffect(effect, token);
      auto expected = effect->m_revision;
      if (const auto found = revisions.find({expected.st_dev, expected.st_ino}); found != revisions.end()) {
        expected = found->second;
      }
      // Merge containers are now restored; native bindings must resolve normally.
      effect->validate(expected, {}, true, token);
      expected = statFd(effect->m_identity->item.get());
      if (effect->m_retained) {
        const auto& retained = effect->m_retained;
        result.source = retained->currentLocation();
        auto next = effect->m_reversed
            ? retained->storeTo(*effect->m_source.parent(), effect->sourceParent(), changed, effect->retentionParent())
            : retained->restoreTo(effect->m_source, effect->sourceParent(), changed);
        applied |= changed;
        result.outcome = TransferOutcome::Moved;
        result.destination = next->currentLocation();
        inverse.push_back(
            std::shared_ptr<const MoveEffect>(new MoveEffect(
                effect->m_destination, effect->m_source, effect->m_identity, statFd(effect->m_identity->item.get()),
                !effect->m_reversed, {}, std::move(next), effect->m_trees
            ))
        );
        completed.fetch_add(1);
        continue;
      }
      auto previous = effect->m_previous;
      if (effect->m_reversed && previous) {
        const auto original = previous->currentLocation();
        previous = previous->storeTo(
            *previous->m_destination.parent(), previous->parent(), changed, effect->retentionParent()
        );
        applied |= changed;
        result.children.emplace_back(original, TransferOutcome::Moved).destination = previous->currentLocation();
      }
      effect->requireVacant();
      const auto from = currentParent(effect->m_destination, effect->destinationParent());
      const auto to = currentParent(effect->m_source, effect->sourceParent());
      // Once a paired predecessor is stored, finish this item's move before observing cancellation again.
      if (!previous || !effect->m_reversed) {
        checkCancelled(token);
      }
      if (::syscall(
              SYS_renameat2, from.get(), effect->m_destination.basename()->c_str(), to.get(),
              effect->m_source.basename()->c_str(), RENAME_NOREPLACE
          )
          != 0) {
        throwErrno(errno);
      }
      changed = applied = true;
      result.outcome = TransferOutcome::Moved;
      result.destination = effect->m_source;
      const auto current = statFd(effect->m_identity->item.get());
      m_revisions->renamed(expected, current);
      if (!sameFileRevision(current, namedStamp(to.get(), effect->m_source))) {
        throwFileError(
            FileErrorKind::Changed, "The item changed during move reversal. Inspect the reported locations."
        );
      }
      if (!effect->m_reversed && previous) {
        const auto retained = previous->currentLocation();
        previous = previous->restoreTo(previous->m_destination, previous->parent(), changed);
        result.children.emplace_back(retained, TransferOutcome::Moved).destination = previous->currentLocation();
      }
      revisions[{current.st_dev, current.st_ino}] = current;
      inverse.push_back(
          std::shared_ptr<const MoveEffect>(new MoveEffect(
              effect->m_destination, effect->m_source, effect->m_identity, current, !effect->m_reversed,
              std::move(previous), {}, effect->m_trees, effect->m_previousTrees
          ))
      );
    } catch (const std::exception&) {
      const auto error = currentFileError();
      applied |= changed;
      reply.retireHistory |= invalidates(error);
      result.outcome = changed                     ? TransferOutcome::Partial
          : error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                   : TransferOutcome::Failed;
      result.error = error;
      stopped = true;
    }
    completed.fetch_add(1);
  }
  // Empty source containers exist now, and must include copied roots in the inverse snapshot.
  if (!stopped && hooks) {
    try {
      hooks->apply(applied);
    } catch (const std::exception&) {
      const auto error = currentFileError();
      reply.retireHistory |= invalidates(error);
      stopped = true;
    }
  }
  std::size_t remaining = 16384;
  if (!stopped) {
    for (std::size_t i = 0; i < folders.size(); ++i) {
      auto& result = reply.results[m_effects.size() + i];
      const auto& folder = folders[i];
      bool changed = false;
      try {
        checkCancelled(token);
        auto next = std::make_shared<MoveFolder>(*folder);
        next->m_item = folder->m_item->refreshFolder(m_undone, remaining, token);
        if (m_undone) {
          next->m_item = next->m_item->storeTo(folder->m_retainingFolder, folder->m_target->get(), changed);
          applied |= changed;
          result.destination = next->m_item->currentLocation();
          result.outcome = TransferOutcome::Moved;
        }
        folders[i] = std::move(next);
      } catch (const std::exception&) {
        applied |= changed;
        result.error = currentFileError();
        if (m_undone) {
          result.outcome = changed                             ? TransferOutcome::Partial
              : result.error->kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                               : TransferOutcome::Failed;
        }
        stopped = true;
        break;
      }
      if (m_undone) {
        completed.fetch_add(1);
      }
    }
  }
  if (applied) {
    m_revisions->advance();
  }
  if (!stopped) {
    try {
      auto captured = capture(inverse, folders, !m_undone, m_sequence);
      if (captured) {
        // Serialize old and inverse records too, while retained generations reject stale replay.
        const_cast<MoveReceipt*>(captured.get())->m_mutex = m_mutex;
      }
      reply.receipt = std::move(captured);
    } catch (const FileErrorException&) {
    }
  }
  reply.retireHistory |= applied && !reply.receipt;
  if (reply.retireHistory) {
    retainIncomplete(reply.results);
  }
  if (stopped) {
    for (auto& result : reply.results) {
      if (result.outcome == TransferOutcome::Skipped && !result.error) {
        result.outcome = token.cancelled() ? TransferOutcome::Cancelled : TransferOutcome::Skipped;
        result.error = FileError{
            .kind = token.cancelled() ? FileErrorKind::Cancelled : FileErrorKind::Other,
            .message = "Not attempted after move reversal stopped."
        };
      }
    }
    completed.store(size());
  }
  return reply;
}

void luciole::MoveReceipt::retainIncomplete(std::vector<TransferResult>& results) const {
  const auto append = [](TransferResult& result, const std::vector<Location>& locations) {
    for (const auto& location : locations) {
      if (!result.error) {
        result.error = FileError{.kind = FileErrorKind::Other, .message = "Move history is unavailable."};
      }
      result.error->message += " Inspect retained item: " + location.uri();
    }
  };
  for (std::size_t i = 0; i < m_effects.size(); ++i) {
    append(results[i], m_effects[i]->retainForRecovery());
  }
  for (std::size_t i = 0; i < m_folders.size(); ++i) {
    append(results[m_effects.size() + i], m_folders[i]->retainForRecovery());
  }
}

std::size_t luciole::transferHistoryBudget(std::size_t descriptorsPerRoot) {
  struct rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return 0;
  }
  std::error_code error;
  std::size_t used = 0;
  std::filesystem::directory_iterator it("/proc/self/fd", error), end;
  while (!error && it != end) {
    ++used;
    it.increment(error);
  }
  if (error || !descriptorsPerRoot || limit.rlim_cur <= used + 128) {
    return 0;
  }
  return static_cast<std::size_t>(std::min<rlim_t>(4096, (limit.rlim_cur - used - 128) / descriptorsPerRoot));
}

std::vector<luciole::TransferResult> luciole::MoveReceipt::unconfirmed(const FileError& error) const {
  std::vector<TransferResult> results;
  for (const auto& effect : m_effects) {
    auto& result = results.emplace_back(effect->m_destination, TransferOutcome::Partial, error);
    result.destination = effect->m_source;
    result.error->message =
        "Move reversal could not be confirmed. Inspect both reported locations. " + result.error->message;
  }
  for (const auto& folder : m_folders) {
    auto& result = results.emplace_back(folder->m_source, TransferOutcome::Partial, error);
    result.error->message = "Merge reversal could not be confirmed. Inspect the source and adjacent retained folders. "
        + result.error->message;
  }
  return results;
}
