#include "operations/copied_move_receipt.h"

#include "operations/history_mount.h"
#include "operations/history_revisions.h"
#include "operations/transfer_staging.h"

#include <algorithm>
#include <fcntl.h>
#include <linux/fs.h>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
  using namespace luciole;
  void check(const CancelToken& token) {
    if (token.cancelled()) {
      throwFileError(FileErrorKind::Cancelled, "Move reversal cancelled.");
    }
  }
  bool invalidates(const FileError& error) {
    return error.kind == FileErrorKind::Changed
        || error.kind == FileErrorKind::Missing
        || error.kind == FileErrorKind::NotDirectory;
  }
  void parentAt(const Location& source, int held, const MoveParents& parents = {}) {
    if (const auto found = parents.find(source.parent()->uri()); found != parents.end()) {
      if (!sameIdentity(statFd(found->second), statFd(held))) {
        throwFileError(FileErrorKind::Changed, "A retained copied Move parent changed.");
      }
      return;
    }
    const auto current = checkedFd(::open(source.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!sameIdentity(statFd(current.get()), statFd(held))) {
      throwFileError(FileErrorKind::Changed, "A copied Move containing folder changed.");
    }
  }
  void vacant(const Location& source, int parent) {
    struct stat value{};
    if (::fstatat(parent, source.basename()->c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0) {
      throwFileError(FileErrorKind::Exists, "A copied Move return name is occupied.");
    }
    if (errno != ENOENT) {
      throwErrno(errno);
    }
  }
  struct Origin {
    Location location;
    std::shared_ptr<const Fd> parent;
  };
} // namespace
struct luciole::CopiedMoveEffect::State {
  explicit State(Origin original) : origin(std::move(original)) {}
  std::shared_ptr<const CopyEffect> source, destination;
  Origin origin;
  std::shared_ptr<const MoveHistoryRoot> root;
  bool published = false, retired = false;
};
std::shared_ptr<const luciole::CopiedMoveEffect> luciole::CopiedMoveEffect::prepare(
    const Location& source, int from, const Location& destination, int target, int stage, bool replacing,
    std::size_t& remaining, const CancelToken& token, const std::shared_ptr<HistoryRevisions>& revisions,
    std::shared_ptr<const MoveHistoryRoot> root
) {
  // This proof follows an actual staged copy/remove path, including EXDEV on one device's bind mounts.
  auto state =
      std::make_shared<State>(Origin{source, std::make_shared<const Fd>(checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)))});
  state->root = std::move(root);
  state->source = CopyEffect::prepareAt(source, from, from, *source.basename(), remaining, token, revisions);
  state->destination = CopyEffect::prepare(destination, target, stage, replacing, remaining, token, revisions);
  return std::shared_ptr<const CopiedMoveEffect>(new CopiedMoveEffect(std::move(state)));
}
std::shared_ptr<const luciole::CopyEffect> luciole::CopiedMoveEffect::destinationProof() const {
  return m_state->destination;
}
void luciole::CopiedMoveEffect::published(int stage) const {
  m_state->destination = m_state->destination->published(stage, m_state->root ? m_state->root->parent.get() : -1);
  m_state->published = true;
}
void luciole::CopiedMoveEffect::beforeSourceRemoval(const CancelToken& token) const {
  if (!m_state->published || m_state->retired) {
    throwFileError(FileErrorKind::Changed, "Copied Move publication is incomplete.");
  }
  m_state->destination->validate(token);
  m_state->source->validate(token);
  check(token);
}
void luciole::CopiedMoveEffect::sourceRemoved(const CancelToken& token) const {
  // Release the original payload descriptor so a successful Move frees source space.
  m_state->source.reset();
  parentAt(m_state->origin.location, m_state->origin.parent->get());
  vacant(m_state->origin.location, m_state->origin.parent->get());
  m_state->destination->validate(token);
  m_state->retired = true;
}
std::vector<luciole::Location> luciole::CopiedMoveEffect::retainForRecovery() const {
  auto paths = m_state->source ? m_state->source->retainForRecovery() : std::vector<Location>{};
  auto target = m_state->destination->retainForRecovery();
  paths.insert(paths.end(), target.begin(), target.end());
  return paths;
}
struct luciole::CopiedMoveReceipt::State {
  std::mutex mutex;
  std::shared_ptr<const CopyReceipt> sources, destinations;
  std::shared_ptr<const MoveReceipt> native;
  std::shared_ptr<const MoveHistoryRoot> root;
  bool dependent = false;
  std::vector<Location> nativeLocations;
  std::vector<std::pair<Location, Location>> locations;
  std::vector<Origin> origins;
  std::vector<std::shared_ptr<const CopyEffect>> restored;
  std::uint64_t generation = 0;
};

bool luciole::CopiedMoveReceipt::usesMount(const HistoryMount& mount) const {
  return (m_state->sources && m_state->sources->usesMount(mount))
      || (m_state->destinations && m_state->destinations->usesMount(mount))
      || (m_state->native && m_state->native->usesMount(mount))
      || (m_state->root && mount.contains(m_state->root->parent.get()))
      || std::ranges::any_of(m_state->origins, [&](const auto& origin) { return mount.contains(origin.parent->get()); })
      || std::ranges::any_of(m_state->restored, [&](const auto& effect) { return effect->usesMount(mount); });
}
luciole::CopiedMoveReceipt::CopiedMoveReceipt(std::shared_ptr<State> state, std::uint64_t generation, bool undone)
    : m_state(std::move(state)), m_generation(generation), m_undone(undone) {}
std::shared_ptr<const luciole::MoveReceipt> luciole::CopiedMoveReceipt::capture(
    const std::vector<std::shared_ptr<const CopiedMoveEffect>>& effects, const CancelToken& token,
    std::shared_ptr<const MoveReceipt> native
) {
  if (effects.empty() || effects.size() + (native ? native->size() : 0) > 4096) {
    return {};
  }
  std::vector<std::shared_ptr<const CopyEffect>> destinations;
  destinations.reserve(effects.size());
  auto state = std::make_shared<State>();
  if (native && native->m_sequence) {
    throwFileError(FileErrorKind::Unsupported, "Mixed Move history requires independent selected roots.");
  }
  if (native) {
    for (const auto& effect : native->m_effects) {
      state->nativeLocations.push_back(effect->destination());
    }
    for (const auto& folder : native->m_folders) {
      state->nativeLocations.push_back(folder->m_source);
    }
  }
  state->native = std::move(native);
  state->locations.reserve(effects.size());
  for (const auto& effect : effects) {
    const auto& item = *effect->m_state;
    if (!item.retired || !item.published) {
      throwFileError(FileErrorKind::Changed, "The copied Move proof is incomplete.");
    }
    state->origins.push_back(item.origin);
    destinations.push_back(item.destination);
    state->locations.emplace_back(item.origin.location, item.destination->m_destination);
  }
  if (state->native) {
    for (const auto& moved : state->native->m_effects) {
      for (const auto& copied : destinations) {
        const auto& a = moved->destination();
        const auto& b = copied->m_destination;
        if (transferLocationInside(b, a) && moved->hasContents()) {
          state->dependent = true;
          state->root = moved->retentionRoot();
        } else if (transferLocationsEqual(a, b) || transferLocationInside(a, b) || transferLocationInside(b, a)) {
          throwFileError(FileErrorKind::Unsupported, "This mixed Move destination dependency has no proof.");
        }
      }
    }
  }
  if (state->dependent) {
    if (!state->root) {
      throwFileError(FileErrorKind::Unsupported, "Dependent Move history has no retention root.");
    }
    for (const auto& copied : destinations) {
      if (!sameMount(copied->parent(), state->root->parent.get())) {
        throwFileError(FileErrorKind::Unsupported, "Dependent Move history requires a common retention mount.");
      }
    }
    for (const auto& moved : state->native->m_effects) {
      if (moved->encloses(state->root->location)) {
        throwFileError(FileErrorKind::Unsupported, "Move retention cannot travel inside a moved ancestor.");
      }
    }
  }
  state->destinations = CopyReceipt::capture(destinations, TransferAction::Copy, token);
  if (!state->destinations || state->destinations->size() != effects.size()) {
    throwFileError(
        FileErrorKind::Unsupported, "Copied Move history requires independent source and destination roots."
    );
  }
  preflight(*state->destinations, token);
  const auto parents = state->native ? state->native->parents(token) : MoveParents{};
  for (const auto& origin : state->origins) {
    parentAt(origin.location, origin.parent->get(), parents);
    vacant(origin.location, origin.parent->get());
  }
  return std::shared_ptr<const MoveReceipt>(new CopiedMoveReceipt(std::move(state), 0, false));
}
std::size_t luciole::CopiedMoveReceipt::size() const {
  return m_state->locations.size() + m_state->nativeLocations.size();
}
void luciole::CopiedMoveReceipt::preflight(
    const CopyReceipt& receipt, const CancelToken& token, const MoveParents& parents
) {
  if (receipt.m_generation != receipt.m_revisions->generation()) {
    throwFileError(FileErrorKind::Changed, "Copied Move history is stale.");
  }
  for (const auto& effect : receipt.m_effects) {
    check(token);
    effect->validate(token, parents);
    if (effect->m_hidden && !effect->m_previous) {
      effect->vacant();
    }
  }
}
std::vector<luciole::TransferResult>
luciole::CopiedMoveReceipt::results(TransferOutcome outcome, const std::optional<FileError>& error) const {
  std::vector<TransferResult> values;
  values.reserve(size());
  for (const auto& [source, destination] : m_state->locations) {
    values.emplace_back(m_undone ? source : destination, outcome, error);
  }
  return values;
}
void luciole::CopiedMoveReceipt::retain(std::vector<TransferResult>& values) const {
  for (std::size_t i = 0; i < m_state->locations.size(); ++i) {
    for (const auto& path : m_state->destinations->m_effects[i]->retainForRecovery()) {
      if (!values[i].error) {
        values[i].error = FileError{FileErrorKind::Other, "Move reversal is incomplete."};
      }
      values[i].error->message += " Inspect retained item: " + path.uri();
    }
    if (!values[i].error) {
      values[i].error = FileError{FileErrorKind::Other, "Move history is unavailable after incomplete reversal."};
    }
    values[i].error->message += " Inspect source "
        + m_state->locations[i].first.uri()
        + " and destination "
        + m_state->locations[i].second.uri();
  }
}
void luciole::CopiedMoveReceipt::preflight(const CancelToken& token, const MoveParents& parents) const {
  preflight(*m_state->destinations, token, parents);
  if (m_undone) {
    if (!m_state->sources || m_state->restored.size() != m_state->locations.size()) {
      throwFileError(FileErrorKind::Changed, "The restored Move source proof is incomplete.");
    }
    preflight(*m_state->sources, token);
  }
  for (const auto& origin : m_state->origins) {
    check(token);
    parentAt(origin.location, origin.parent->get(), parents);
    if (!m_undone) {
      vacant(origin.location, origin.parent->get());
    }
  }
}
luciole::MoveParents luciole::CopiedMoveReceipt::destinationParents(const CancelToken& token) const {
  std::vector<std::pair<Location, int>> anchors;
  for (const auto& effect : m_state->destinations->m_effects) {
    anchors.emplace_back(effect->m_destination, effect->parent());
  }
  return m_state->native->parents(token, anchors);
}
void luciole::CopiedMoveReceipt::refreshDestinations(const CancelToken& token) const {
  // These exclusive component roots stay independent across replay; refresh only the job generation.
  auto next = std::make_shared<CopyReceipt>();
  next->m_effects = m_state->destinations->m_effects;
  next->m_revisions = m_state->destinations->m_revisions;
  next->m_generation = next->m_revisions->generation();
  preflight(*next, token, m_state->dependent ? destinationParents(token) : MoveParents{});
  m_state->destinations = std::move(next);
}
void luciole::CopiedMoveReceipt::restoreSources(
    const CancelToken& token, MoveReplayResult& reply, bool& applied
) const {
  std::atomic<std::uint64_t> bytes{0};
  transfer::Work work(token, bytes, {});
  work.history = std::make_shared<HistoryRevisions>();
  std::size_t remaining = 16384;
  m_state->restored.assign(m_state->locations.size(), {});
  for (std::size_t i = 0; i < m_state->locations.size(); ++i) {
    const auto& origin = m_state->origins[i];
    const auto& incoming = m_state->destinations->m_effects[i];
    reply.results[i].outcome = TransferOutcome::Failed;
    check(token);
    incoming->validate(token);
    parentAt(origin.location, origin.parent->get());
    vacant(origin.location, origin.parent->get());
    transfer::Stage stage(origin.parent->get(), work);
    (void)transfer::copyTree(incoming->parent(), *incoming->m_destination.basename(), stage.fd(), "item", work, 0);
    incoming->validate(token);
    if (work.metadataIncomplete) {
      throwFileError(FileErrorKind::Unsupported, "The original filesystem cannot preserve some copied metadata.");
    }
    auto proof =
        CopyEffect::prepare(origin.location, origin.parent->get(), stage.fd(), false, remaining, token, work.history);
    parentAt(origin.location, origin.parent->get());
    vacant(origin.location, origin.parent->get());
    check(token);
    if (::syscall(
            SYS_renameat2, stage.fd(), "item", origin.parent->get(), origin.location.basename()->c_str(),
            RENAME_NOREPLACE
        )
        != 0) {
      throwErrno(errno);
    }
    applied = true;
    reply.results[i].children.emplace_back(incoming->m_destination, TransferOutcome::Copied).destination =
        origin.location;
    m_state->restored[i] = proof;
    m_state->restored[i] = proof->published(stage.fd());
    if (!stage.clear()) {
      throwFileError(FileErrorKind::Other, "The source was restored; inspect its private staging folder.");
    }
  }
  // Source copies must be durable before any destination is removed or exchanged.
  std::set<dev_t> synced;
  for (const auto& origin : m_state->origins) {
    check(token);
    if (synced.insert(statFd(origin.parent->get()).st_dev).second) {
      transfer::synchronize(origin.parent->get());
    }
  }
  m_state->sources = CopyReceipt::capture(m_state->restored, TransferAction::Copy, token);
  if (!m_state->sources) {
    throwFileError(FileErrorKind::Changed, "The restored Move sources could not be verified.");
  }
}
void luciole::CopiedMoveReceipt::removeSources(const CancelToken& token, MoveReplayResult& reply, bool& applied) const {
  std::atomic<std::uint64_t> bytes{0};
  transfer::Work work(token, bytes, {});
  work.history = m_state->restored.front()->revisions();
  std::set<dev_t> synced;
  for (const auto& effect : m_state->destinations->m_effects) {
    check(token);
    effect->validate(token);
    if (synced.insert(statFd(effect->parent()).st_dev).second) {
      transfer::synchronize(effect->parent());
    }
  }
  m_state->sources.reset();
  for (std::size_t i = 0; i < m_state->locations.size(); ++i) {
    const auto& origin = m_state->origins[i];
    const auto& proof = m_state->restored[i];
    check(token);
    parentAt(origin.location, origin.parent->get());
    m_state->destinations->m_effects[i]->validate(token);
    proof->validate(token);
    auto tree = *proof->m_tree;
    tree.name = *origin.location.basename();
    // Recursive removal can have partial effects even when it throws.
    applied = true;
    transfer::removeSource(origin.parent->get(), tree, work);
    m_state->restored[i].reset();
    reply.results[i].children.emplace_back(origin.location, TransferOutcome::Moved).destination =
        m_state->locations[i].second;
  }
  m_state->sources.reset();
  m_state->restored.clear();
}
void luciole::CopiedMoveReceipt::reverseDestinations(
    const CancelToken& token, MoveReplayResult& reply, bool& applied
) const {
  const auto before = m_state->destinations->m_revisions->generation();
  std::atomic<std::size_t> completed{0};
  auto phase =
      m_state->destinations->reverseImpl(token, completed, {}, m_state->root ? m_state->root->parent.get() : -1, true);
  applied |= before != m_state->destinations->m_revisions->generation();
  for (std::size_t i = 0; i < m_state->locations.size(); ++i) {
    reply.results[i].children.push_back(phase.results[i]);
    if (phase.results[i].error) {
      reply.results[i].error = phase.results[i].error;
      reply.results[i].outcome = phase.results[i].outcome;
    }
  }
  reply.retireHistory |= phase.retireHistory;
  if (!phase.receipt) {
    for (const auto& result : phase.results) {
      if (result.error) {
        throw FileErrorException(*result.error);
      }
    }
    throwFileError(FileErrorKind::Other, "The destination reversal could not be confirmed.");
  }
  m_state->destinations = std::move(phase.receipt);
}
std::vector<luciole::TransferResult>
luciole::CopiedMoveReceipt::nativeResults(TransferOutcome outcome, const std::optional<FileError>& error) const {
  std::vector<TransferResult> values;
  for (const auto& location : m_state->nativeLocations) {
    values.emplace_back(location, outcome, error);
  }
  return values;
}
void luciole::CopiedMoveReceipt::retainAll(std::vector<TransferResult>& values) const {
  const auto firstCopied = values.begin() + static_cast<std::ptrdiff_t>(m_state->nativeLocations.size());
  if (m_state->native) {
    std::vector<TransferResult> native(values.begin(), firstCopied);
    m_state->native->retainIncomplete(native);
    std::ranges::move(native, values.begin());
  }
  std::vector<TransferResult> copied(firstCopied, values.end());
  retain(copied);
  std::ranges::move(copied, firstCopied);
}
std::vector<luciole::TransferResult> luciole::CopiedMoveReceipt::unconfirmed(const FileError& error) const {
  auto values = nativeResults(TransferOutcome::Partial, error);
  auto copied = results(TransferOutcome::Partial, error);
  values.insert(values.end(), copied.begin(), copied.end());
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  if (lock.owns_lock() && m_generation == m_state->generation) {
    ++m_state->generation;
    retainAll(values);
  }
  return values;
}
luciole::MoveReplayResult
luciole::CopiedMoveReceipt::reverseCopied(const CancelToken& token, bool& applied, const MoveParents& parents) const {
  MoveReplayResult reply;
  reply.results = results(TransferOutcome::Skipped);
  bool began = false;
  try {
    preflight(token, parents);
    began = true;
    if (!m_undone) {
      restoreSources(token, reply, applied);
      reverseDestinations(token, reply, applied);
    } else {
      reverseDestinations(token, reply, applied);
      removeSources(token, reply, applied);
    }
    for (std::size_t i = 0; i < m_state->locations.size(); ++i) {
      reply.results[i].outcome = TransferOutcome::Moved;
      reply.results[i].destination = m_undone ? m_state->locations[i].second : m_state->locations[i].first;
    }
    // The outer owner publishes the inverse only after both components have completed.
  } catch (const FileErrorException& failure) {
    const auto& error = failure.error();
    reply.retireHistory |= applied || invalidates(error);
    for (auto& result : reply.results) {
      const bool changed = std::ranges::any_of(result.children, [](const auto& child) {
        return child.outcome == TransferOutcome::Moved
            || child.outcome == TransferOutcome::Copied
            || child.outcome == TransferOutcome::Partial;
      });
      result.outcome = changed                                  ? TransferOutcome::Partial
          : error.kind == FileErrorKind::Cancelled              ? TransferOutcome::Cancelled
          : began && result.outcome == TransferOutcome::Skipped ? TransferOutcome::Skipped
                                                                : TransferOutcome::Failed;
      if (!result.error) {
        result.error = error;
      }
    }
  }
  return reply;
}
luciole::MoveReplayResult
luciole::CopiedMoveReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  MoveReplayResult reply;
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  bool applied = false;
  try {
    if (!lock.owns_lock()) {
      throwFileError(FileErrorKind::Other, "Another reversal is using this Move history.");
    }
    if (m_generation != m_state->generation) {
      throwFileError(FileErrorKind::Changed, "Copied Move history is stale.");
    }
    if (m_state->native) {
      MoveReplayResult copied;
      bool attempted = false;
      const auto applyCopies = [&](bool& changed) {
        attempted = true;
        copied = reverseCopied(token, changed);
        const auto failure =
            std::ranges::find_if(copied.results, [](const auto& item) { return item.error.has_value(); });
        if (failure != copied.results.end()) {
          throw FileErrorException(*failure->error);
        }
      };
      const MoveReceipt::ReplayHooks hooks{
          [&](const MoveParents& parents) {
            preflight(token, m_state->dependent ? destinationParents(token) : parents);
          },
          [&](bool& changed) {
            if (m_state->dependent && !m_undone) {
              applyCopies(changed);
            }
          },
          [&](bool& changed) {
            if (!m_state->dependent || m_undone) {
              applyCopies(changed);
            }
          }
      };
      std::atomic<std::size_t> nativeCompleted{0};
      const auto before = m_state->native->m_revisions->generation();
      reply = m_state->native->reverseImpl(token, nativeCompleted, &hooks);
      applied = before != m_state->native->m_revisions->generation();
      if (!attempted || copied.results.empty()) {
        const auto failure =
            std::ranges::find_if(reply.results, [](const auto& item) { return item.error.has_value(); });
        copied.results = results(
            TransferOutcome::Skipped,
            failure != reply.results.end() ? failure->error
                                           : std::optional<FileError>{FileError{
                                                 FileErrorKind::Other, "Move reversal stopped before copied items."
                                             }}
        );
      }
      reply.results.insert(reply.results.end(), copied.results.begin(), copied.results.end());
      reply.retireHistory |= copied.retireHistory;
      if (!reply.receipt) {
        if (reply.retireHistory) {
          ++m_state->generation;
          retainAll(reply.results);
        }
        completed.store(size());
        return reply;
      }
      m_state->native = reply.receipt;
      reply.receipt.reset();
      // Native replay advances the shared job generation after copied destination reversal.
      refreshDestinations(token);
    } else {
      reply = reverseCopied(token, applied);
      const auto failure = std::ranges::find_if(reply.results, [](const auto& item) { return item.error.has_value(); });
      if (failure != reply.results.end()) {
        throw FileErrorException(*failure->error);
      }
    }
    ++m_state->generation;
    reply.receipt = std::shared_ptr<const MoveReceipt>(new CopiedMoveReceipt(m_state, m_state->generation, !m_undone));
  } catch (const FileErrorException& failure) {
    reply.retireHistory |= applied || invalidates(failure.error());
    if (reply.results.empty()) {
      const auto outcome =
          failure.error().kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled : TransferOutcome::Failed;
      reply.results = nativeResults(outcome, failure.error());
      auto copied = results(outcome, failure.error());
      reply.results.insert(reply.results.end(), copied.begin(), copied.end());
    }
    if (applied) {
      for (auto& result : reply.results) {
        result.outcome = TransferOutcome::Partial;
        if (!result.error) {
          result.error = failure.error();
        }
      }
    }
    if (reply.retireHistory && lock.owns_lock() && m_generation == m_state->generation) {
      ++m_state->generation;
      retainAll(reply.results);
    }
  }
  completed.store(size());
  return reply;
}
