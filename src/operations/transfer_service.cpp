#include "operations/transfer_service.h"

#include "fs/background_job.h"
#include "fs/cancel_token.h"
#include "fs/filename.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "fs/system_mounts.h"
#include "operations/conflict_gate.h"
#include "operations/copied_move_receipt.h"
#include "operations/copy_receipt.h"
#include "operations/history_revisions.h"
#include "operations/move_receipt.h"
#include "operations/provider_transfer.h"
#include "operations/transfer_copy.h"
#include "operations/transfer_staging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <glib.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <tuple>
#include <utility>

namespace {
  using namespace luciole::transfer;

  struct Job {
    explicit Job(luciole::TransferRequest transfer) : request(std::move(transfer)) {}
    luciole::TransferRequest request;
    std::vector<std::shared_ptr<Node>> nodes;
    std::unique_ptr<luciole::ProviderTransfer> provider;
    std::map<std::pair<dev_t, ino_t>, struct stat> changedLinks;
    std::size_t mergedItems = 0;
    std::vector<luciole::TransferResult> outcomes;
  };

  struct Answer {
    luciole::TransferConflictChoice choice;
    bool applyToAll = false;
  };
  using Gate = luciole::ConflictGate<luciole::TransferConflict, Answer>;

  struct Reply {
    std::vector<luciole::TransferResult> results;
    std::string report;
    std::shared_ptr<const luciole::MoveReceipt> receipt;
    std::shared_ptr<const luciole::CopyReceipt> copyReceipt;
  };

  std::string reportUri(const luciole::Location& location) {
    const std::unique_ptr<GUri, decltype(&g_uri_unref)> parsed(
        g_uri_parse(
            location.uri().c_str(), static_cast<GUriFlags>(G_URI_FLAGS_ENCODED | G_URI_FLAGS_HAS_PASSWORD), nullptr
        ),
        g_uri_unref
    );
    if (!parsed) {
      return "[invalid location]";
    }
    const luciole::GString safe(g_uri_to_string_partial(parsed.get(), G_URI_HIDE_PASSWORD));
    return safe ? safe.get() : "[invalid location]";
  }

  std::string jobReport(const std::vector<luciole::TransferResult>& results) {
    std::string report = "Transfer results\n";
    constexpr std::array labels{"Moved", "Copied", "Skipped", "Failed", "Cancelled", "Partial", "Linked"};
    std::function<void(const luciole::TransferResult&)> append = [&](const auto& result) {
      if (report.size() > 16 * 1024 * 1024) {
        return;
      }
      report += reportUri(result.source)
          + (result.destination ? " -> " + reportUri(*result.destination) : "")
          + "\n  "
          + labels.at(static_cast<std::size_t>(result.outcome));
      report += result.retryable ? " (retry available)" : "";
      report += result.error ? "\n  " + result.error->message + "\n" : "\n";
      for (const auto& child : result.children) {
        append(child);
      }
    };
    for (const auto& result : results) {
      append(result);
    }
    if (report.size() > 16 * 1024 * 1024) {
      report += "\nReport truncated.\n";
    }
    return report;
  }

  luciole::FileError cancelledError() {
    return {.kind = luciole::FileErrorKind::Cancelled, .message = "Transfer cancelled."};
  }
} // namespace

struct luciole::TransferService::State {
  std::function<void()> changed;
  bool active = false;
  TransferAction action = TransferAction::Move;
  std::size_t total = 0;
  std::atomic<std::size_t> completed{0};
  std::atomic<std::uint64_t> bytes{0};
  CancelToken token;
  std::vector<TransferResult> results;
  Gate gate;
  std::uint64_t jobId = 0;
  std::shared_ptr<Job> job;
  std::size_t retryable = 0;
  std::string report;
  std::shared_ptr<const MoveReceipt> receipt;
  std::shared_ptr<const CopyReceipt> copyReceipt;
  bool historyRetired = false;

  static void notify(const std::shared_ptr<State>& owner) {
    auto* source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          const auto& retained = *static_cast<std::shared_ptr<State>*>(data);
          const auto callback = retained->changed;
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
};

namespace {
  // Worker side of a conflict: remembered policies first, then the gate.
  Work::Choose conflictAsker(
      Gate& gate, std::function<void()> published, luciole::CancelToken token, bool allowMerge,
      std::map<std::pair<mode_t, mode_t>, luciole::TransferConflictChoice>& policies
  ) {
    return [&gate, published = std::move(published), token = std::move(token), allowMerge, &policies](
               const Source& source, const luciole::Location& destination, const std::string& target,
               const struct stat& existing, bool forcePrompt
           ) {
      const auto kind = std::pair{source.metadata.st_mode & S_IFMT, existing.st_mode & S_IFMT};
      if (!forcePrompt) {
        if (const auto found = policies.find(kind); found != policies.end()) {
          return found->second;
        }
      }
      const bool folders = S_ISDIR(source.metadata.st_mode) && S_ISDIR(existing.st_mode);
      luciole::TransferConflict conflict{
          .id = 0,
          .source = source.location,
          .destination = destination.child(target),
          .name = luciole::filenameDisplayText(target),
          .sourceDirectory = S_ISDIR(source.metadata.st_mode),
          .destinationDirectory = S_ISDIR(existing.st_mode),
          .sourceSize = S_ISDIR(source.metadata.st_mode) || source.metadata.st_size < 0
              ? std::nullopt
              : std::optional<std::uint64_t>(source.metadata.st_size),
          .destinationSize = S_ISDIR(existing.st_mode) || existing.st_size < 0
              ? std::nullopt
              : std::optional<std::uint64_t>(existing.st_size),
          .sourceModified = source.metadata.st_mtim.tv_sec < 0
              ? std::nullopt
              : std::optional<std::uint64_t>(source.metadata.st_mtim.tv_sec),
          .destinationModified =
              existing.st_mtim.tv_sec < 0 ? std::nullopt : std::optional<std::uint64_t>(existing.st_mtim.tv_sec),
          .canMerge = allowMerge && folders,
          .canApplyToAll = allowMerge,
          .scopeLabel = folders ? "Apply to remaining folder conflicts in this job"
                                : "Apply to remaining conflicts of these types in this job"
      };
      const auto answer = gate.ask(std::move(conflict), token, published);
      if (!answer) {
        return luciole::TransferConflictChoice::Cancel;
      }
      if (answer->applyToAll && answer->choice != luciole::TransferConflictChoice::Cancel) {
        policies.insert_or_assign(kind, answer->choice);
      }
      return answer->choice;
    };
  }
  // Removes the sources still waiting for a durability flush. Earlier batches may have completed items after their
  // outcome was recorded, so moves refresh every attempted item.
  void finishMoves(Job& job, Work& work, std::vector<luciole::TransferResult>& outcomes) {
    if (job.request.action != luciole::TransferAction::Move) {
      return;
    }
    flushMoves(work);
    for (const auto& node : job.nodes) {
      if (node && node->attempted) {
        outcomes.at(node->source.index) = snapshot(*node);
      }
    }
  }

  std::vector<luciole::TransferResult> retryJob(Job& job, Work& work, std::atomic<std::size_t>& completed) {
    auto outcomes = job.outcomes;
    for (const auto& node : job.nodes) {
      if (node) {
        execute(*node, job.request.action, work, true);
        outcomes.at(node->source.index) = snapshot(*node);
      }
      completed.fetch_add(1);
    }
    finishMoves(job, work, outcomes);
    return outcomes;
  }

  void guardCopiedMoves(
      Work& work, const std::vector<Source>& sources, const std::vector<bool>& included,
      const std::vector<bool>& processed, const luciole::Location& destination
  ) {
    if (!work.prepareCopiedMove) {
      return;
    }
    struct Root {
      luciole::Location source, destination;
      bool folder;
    };
    std::vector<Root> roots;
    for (std::size_t i = 0; i < sources.size(); ++i) {
      if (processed[sources[i].index] || included[i]) {
        continue;
      }
      if (roots.size() == 4096) {
        work.prepareCopiedMove = {};
        return;
      }
      roots.push_back({sources[i].location, destination.child(sources[i].name), S_ISDIR(sources[i].metadata.st_mode)});
    }
    work.prepareCopiedMove = [prepare = std::move(work.prepareCopiedMove), roots = std::move(roots),
                              parent = work.copiedMergeParent, checked = false, independent = true](
                                 const Source& source, const luciole::Location& target, int folder, int stage,
                                 bool replacing
                             ) mutable -> std::shared_ptr<const luciole::CopiedMoveEffect> {
      const auto overlap = [](const auto& a, const auto& b) {
        return luciole::transferLocationsEqual(a, b)
            || luciole::transferLocationInside(a, b)
            || luciole::transferLocationInside(b, a);
      };
      if (!checked) {
        checked = true;
        for (std::size_t i = 0; i < roots.size() && independent; ++i) {
          for (const auto& other : roots) {
            if (overlap(roots[i].source, other.destination)) {
              independent = false;
            }
          }
          for (std::size_t j = 0; j < i && independent; ++j) {
            if (overlap(roots[i].source, roots[j].source)) {
              independent = false;
            }
            if (overlap(roots[i].destination, roots[j].destination)
                && !(
                    roots[i].folder
                    && roots[j].folder
                    && roots[i].destination == roots[j].destination
                    && parent
                    && parent(roots[i].destination)
                )) {
              independent = false;
            }
          }
        }
      }
      return independent ? prepare(source, target, folder, stage, replacing) : nullptr;
    };
  }

  std::vector<luciole::TransferResult> runFirst(Job& job, Work& work, std::atomic<std::size_t>& completed) {
    using luciole::TransferOutcome;
    const auto& request = job.request;
    std::vector<luciole::TransferResult> outcomes;
    outcomes.reserve(request.sources.size());
    for (const auto& source : request.sources) {
      outcomes.emplace_back(source, TransferOutcome::Failed);
    }
    std::vector<bool> processed(outcomes.size(), false);
    job.nodes.resize(outcomes.size());
    try {
      const auto path = request.destination.nativePath();
      if (!path) {
        luciole::throwFileError(
            luciole::FileErrorKind::Unsupported, "Transfers to remote folders are not supported yet."
        );
      }
      auto destination = opened(::open(path->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      auto binding =
          std::make_shared<const Destination>(Destination{request.destination, luciole::statFd(destination.get()), {}});
      std::vector<Source> sources;
      for (std::size_t index = 0; index < request.sources.size(); ++index) {
        const auto& location = request.sources[index];
        try {
          work.check();
          const auto sourcePath = location.nativePath();
          if (!sourcePath) {
            luciole::throwFileError(
                luciole::FileErrorKind::Unsupported, "Transfers from remote locations are not supported yet."
            );
          }
          const std::filesystem::path source(*sourcePath);
          const auto name = source.filename().native();
          if (name.empty() || name == "." || name == "..") {
            luciole::throwFileError(
                luciole::FileErrorKind::InvalidArgument, "The filesystem root cannot be transferred."
            );
          }
          auto parent = opened(::open(source.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
          const auto metadata = info(parent.get(), name);
          sources.push_back(
              {index, location, luciole::Fd{}, name, metadata, source.parent_path().native(),
               luciole::statFd(parent.get())}
          );
        } catch (const TransferCancelled&) {
          outcomes[index] = {location, TransferOutcome::Cancelled, cancelledError()};
          processed[index] = true;
          completed.fetch_add(1);
        } catch (const std::exception&) {
          outcomes[index] = {location, TransferOutcome::Failed, luciole::currentFileError()};
          processed[index] = true;
          completed.fetch_add(1);
        }
      }
      std::set<std::pair<dev_t, ino_t>> selectedDirectories;
      for (const auto& source : sources) {
        if (request.action != luciole::TransferAction::Link && S_ISDIR(source.metadata.st_mode)) {
          selectedDirectories.emplace(source.metadata.st_dev, source.metadata.st_ino);
        }
      }
      std::set<std::tuple<dev_t, ino_t, std::string>> seen;
      std::vector<bool> included(sources.size(), false);
      // Classify overlaps before any selected parent is moved or removed.
      for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& source = sources[i];
        try {
          work.check();
          source.parent = openSourceParent(source);
          const auto parentInfo = source.parentMetadata;
          included[i] = !seen.emplace(parentInfo.st_dev, parentInfo.st_ino, source.name).second;
          auto current = opened(::fcntl(source.parent.get(), F_DUPFD_CLOEXEC, 0));
          for (std::size_t depth = 0; !included[i] && !selectedDirectories.empty(); ++depth) {
            work.check();
            if (depth >= 65536) {
              luciole::throwFileError(luciole::FileErrorKind::Unsupported, "The folder ancestry is too deep.");
            }
            const auto here = luciole::statFd(current.get());
            if (selectedDirectories.contains({here.st_dev, here.st_ino})) {
              included[i] = true;
              break;
            }
            auto parent = directory(current.get(), "..");
            if (luciole::sameIdentity(here, luciole::statFd(parent.get()))) {
              break;
            }
            current = std::move(parent);
          }
        } catch (const TransferCancelled&) {
          outcomes[source.index] = {source.location, TransferOutcome::Cancelled, cancelledError()};
          processed[source.index] = true;
          completed.fetch_add(1);
        } catch (const std::exception&) {
          outcomes[source.index] = {source.location, TransferOutcome::Failed, luciole::currentFileError()};
          processed[source.index] = true;
          completed.fetch_add(1);
        }
        source.parent = luciole::Fd{};
      }

      guardCopiedMoves(work, sources, included, processed, binding->location);

      for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& source = sources[i];
        if (processed[source.index]) {
          continue;
        }
        try {
          if (included[i]) {
            outcomes[source.index].outcome = TransferOutcome::Skipped;
          } else {
            Source captured{source.index,    source.location,   luciole::Fd{},        source.name,
                            source.metadata, source.parentPath, source.parentMetadata};
            auto node = std::make_shared<Node>(std::move(captured), binding, outcomes[source.index]);
            job.nodes[source.index] = node;
            execute(*node, request.action, work, false);
            outcomes[source.index] = snapshot(*node);
          }
        } catch (const std::exception&) {
          outcomes[source.index].error = luciole::currentFileError();
        }
        source.parent = luciole::Fd{};
        processed[source.index] = true;
        completed.fetch_add(1);
      }
    } catch (const std::exception&) {
      const auto error = luciole::currentFileError();
      for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (!processed[i]) {
          outcomes[i].error = error;
        }
      }
    }
    finishMoves(job, work, outcomes);
    completed.store(outcomes.size());
    return outcomes;
  }
} // namespace

luciole::TransferService::TransferService() : m_state(std::make_shared<State>()) {}
luciole::TransferService::~TransferService() {
  m_state->changed = {};
  cancel();
}
void luciole::TransferService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::TransferService::busy() const { return m_state->active; }
std::size_t luciole::TransferService::total() const { return m_state->total; }
std::size_t luciole::TransferService::completed() const { return m_state->completed.load(); }
std::uint64_t luciole::TransferService::bytes() const { return m_state->bytes.load(); }
luciole::TransferAction luciole::TransferService::action() const { return m_state->action; }
const std::vector<luciole::TransferResult>& luciole::TransferService::results() const { return m_state->results; }
void luciole::TransferService::cancel() {
  m_state->token.cancel();
  m_state->gate.wake();
}
std::optional<luciole::TransferConflict> luciole::TransferService::conflict() const { return m_state->gate.pending(); }
bool luciole::TransferService::resolve(std::uint64_t id, TransferConflictChoice choice, bool applyToAll) {
  if (choice < TransferConflictChoice::Replace || choice > TransferConflictChoice::Merge) {
    return false;
  }
  const auto& token = m_state->token;
  return m_state->gate.resolve(id, Answer{.choice = choice, .applyToAll = applyToAll}, [&](const auto& conflict) {
    if ((choice == TransferConflictChoice::Merge && !conflict.canMerge)
        || (applyToAll && (!conflict.canApplyToAll || choice == TransferConflictChoice::Cancel))) {
      return false;
    }
    if (choice == TransferConflictChoice::Cancel) {
      token.cancel();
    }
    return true;
  });
}

std::size_t luciole::TransferService::retryable() const { return busy() ? 0 : m_state->retryable; }
const std::string& luciole::TransferService::report() const { return m_state->report; }
std::uint64_t luciole::TransferService::jobId() const { return m_state->jobId; }
std::shared_ptr<const luciole::MoveReceipt> luciole::TransferService::receipt() const { return m_state->receipt; }
std::shared_ptr<const luciole::MoveReceipt> luciole::TransferService::takeReceipt() {
  return std::move(m_state->receipt);
}
bool luciole::TransferService::historyRetired() const { return m_state->historyRetired; }
std::shared_ptr<const luciole::CopyReceipt> luciole::TransferService::copyReceipt() const {
  return m_state->copyReceipt;
}
std::shared_ptr<const luciole::CopyReceipt> luciole::TransferService::takeCopyReceipt() {
  return std::move(m_state->copyReceipt);
}
bool luciole::TransferService::start(TransferRequest request) { return launch(std::move(request), false); }
bool luciole::TransferService::retryFailed() {
  return m_state->job && retryable() > 0 ? launch(m_state->job->request, true) : false;
}
namespace {
  using namespace luciole;
  std::optional<std::size_t> completedCopies(const Job& job, const std::vector<TransferResult>& outcomes) {
    if (job.provider) {
      return job.provider->completedCopies();
    }
    std::size_t copied = 0;
    const auto completeCopy = [&](auto&& self, const Node& node) -> bool {
      if (node.result.error
          || (node.result.outcome != TransferOutcome::Copied
              && node.result.outcome != TransferOutcome::Linked
              && node.result.outcome != TransferOutcome::Skipped)) {
        return false;
      }
      if (!node.merging) {
        copied +=
            node.result.outcome == TransferOutcome::Copied || node.result.outcome == TransferOutcome::Linked ? 1 : 0;
      }
      return std::ranges::all_of(node.children, [&](const auto& child) { return self(self, *child); });
    };
    const bool copiesComplete = std::ranges::all_of(
                                    outcomes,
                                    [](const auto& result) {
                                      return !result.error
                                          && (result.outcome == TransferOutcome::Copied
                                              || result.outcome == TransferOutcome::Linked
                                              || result.outcome == TransferOutcome::Skipped);
                                    }
                                )
        && std::ranges::all_of(job.nodes, [&](const auto& node) { return !node || completeCopy(completeCopy, *node); });
    return copiesComplete ? std::optional(copied) : std::nullopt;
  }
  bool
  completedMoves(const Job& job, const std::vector<TransferResult>& outcomes, std::size_t& moved, std::size_t& merged) {
    const auto countMove = [&](auto&& self, const Node& node) -> bool {
      if (node.result.error
          || (node.result.outcome != TransferOutcome::Moved && node.result.outcome != TransferOutcome::Skipped)) {
        return false;
      }
      if (node.result.outcome == TransferOutcome::Moved) {
        node.merging ? ++merged : ++moved;
      }
      return std::ranges::all_of(node.children, [&](const auto& child) { return self(self, *child); });
    };
    return std::ranges::all_of(
               outcomes,
               [](const auto& result) {
                 return !result.error
                     && (result.outcome == TransferOutcome::Moved || result.outcome == TransferOutcome::Skipped);
               }
           )
        && std::ranges::all_of(job.nodes, [&](const auto& node) { return !node || countMove(countMove, *node); });
  }
  Fd copyHistoryParent(const Location& destination) {
    const auto root = destination.nativePath();
    if (!root) {
      throwFileError(FileErrorKind::Unsupported, "Copy history requires a native destination.");
    }
    return checkedFd(::open(root->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  }
  struct CopiedMoveCapture {
    struct Prepared {
      std::shared_ptr<const luciole::CopiedMoveEffect> effect;
      std::size_t source;
    };
    std::vector<Prepared> prepared;
    bool captureFailed = false;
    void attach(
        Work& work, std::size_t& remaining, const std::function<bool()>& room,
        const std::function<std::shared_ptr<const MoveHistoryRoot>(const Location&, int)>& retention
    ) {
      work.prepareCopiedMove = [&, room, retention](
                                   const Source& source, const luciole::Location& destination, int target, int stage,
                                   bool replacing
                               ) -> std::shared_ptr<const luciole::CopiedMoveEffect> {
        if (room()) {
          try {
            auto effect = luciole::CopiedMoveEffect::prepare(
                source.location, source.parent.get(), destination, target, stage, replacing, remaining, work.token,
                work.history, retention(destination, target)
            );
            prepared.push_back({effect, source.index});
            return effect;
          } catch (const luciole::FileErrorException&) {
          }
        }
        return {};
      };
    }
    std::shared_ptr<const luciole::MoveReceipt> capture(
        std::size_t moved, const luciole::CancelToken& token, std::shared_ptr<const luciole::MoveReceipt> native = {}
    ) {
      if (moved != prepared.size()) {
        return {};
      }
      std::vector<std::shared_ptr<const luciole::CopiedMoveEffect>> effects;
      effects.reserve(prepared.size());
      for (const auto& item : prepared) {
        effects.push_back(item.effect);
      }
      try {
        return luciole::CopiedMoveReceipt::capture(effects, token, std::move(native));
      } catch (const luciole::FileErrorException&) {
        captureFailed = true;
        return {};
      }
    }
    std::vector<std::shared_ptr<const CopyEffect>> destinations() const {
      std::vector<std::shared_ptr<const CopyEffect>> effects;
      effects.reserve(prepared.size());
      for (const auto& item : prepared) {
        effects.push_back(item.effect->destinationProof());
      }
      return effects;
    }
    void preserve(std::vector<luciole::TransferResult>& outcomes) const {
      for (const auto& item : prepared) {
        for (const auto& path : item.effect->retainForRecovery()) {
          auto& result = outcomes.at(item.source);
          if (!result.error) {
            result.error = luciole::FileError{luciole::FileErrorKind::Other, "Move history is unavailable."};
          }
          result.error->message += " Inspect retained item: " + path.uri();
        }
      }
    }
  };
  // Worker-local optional history collection; prepared proofs outlive every source.
  struct MoveCapture {
    struct PreparedMove {
      std::shared_ptr<const MoveEffect> effect;
      std::size_t source;
    };
    struct RetainedFolder {
      std::shared_ptr<const MoveFolder> effect;
      std::size_t source;
    };
    std::size_t budget, remaining = 16384;
    bool collecting;
    CancelToken token;
    std::shared_ptr<HistoryRevisions> revisions;
    Location destination;
    std::shared_ptr<const MoveHistoryRoot> root;
    const std::vector<Location>& sources;
    std::map<std::size_t, std::shared_ptr<const MoveHistoryRoot>> sourceRoots;
    std::function<std::size_t()> copiedRoots = [] { return 0; };
    std::vector<std::shared_ptr<const MoveEffect>> moves;
    std::vector<PreparedMove> preparedMoves;
    std::vector<RetainedFolder> retainedFolders;
    MoveCapture(
        std::size_t limit, CancelToken cancel, std::shared_ptr<HistoryRevisions> history, Location target,
        const std::vector<Location>& selected
    )
        : budget(limit), collecting(limit != 0), token(std::move(cancel)), revisions(std::move(history)),
          destination(std::move(target)), sources(selected) {}
    std::shared_ptr<const MoveHistoryRoot> sourceRetention(const Source& source) {
      if (const auto found = sourceRoots.find(source.index); found != sourceRoots.end()) {
        return found->second;
      }
      // Only empty containers are retained, outside the original selected tree on its own filesystem.
      const auto parent = sources.at(source.index).parent();
      if (!parent || !parent->isNative()) {
        throwFileError(FileErrorKind::Unsupported, "Merge-container history requires a native source parent.");
      }
      auto held = checkedFd(::open(parent->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      auto retained = std::make_shared<MoveHistoryRoot>(*parent, std::move(held));
      sourceRoots.emplace(source.index, retained);
      return retained;
    }
    std::shared_ptr<const MoveHistoryRoot> retention(const Location& folder, int parent) {
      if (!root) {
        const auto path = destination.nativePath();
        if (!path) {
          throwFileError(FileErrorKind::Unsupported, "Move history requires a native destination.");
        }
        root = std::make_shared<MoveHistoryRoot>(
            destination, checkedFd(::open(path->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))
        );
      }
      if (sameMount(root->parent.get(), parent)) {
        return root;
      }
      for (const auto& effect : moves) {
        if (effect->encloses(folder)
            && effect->retentionRoot()
            && sameMount(effect->retentionRoot()->parent.get(), parent)) {
          return effect->retentionRoot();
        }
      }
      // A mounted branch needs retention on its own filesystem, outside its first moved ancestor.
      return std::make_shared<MoveHistoryRoot>(folder, checkedFd(::fcntl(parent, F_DUPFD_CLOEXEC, 0)));
    }
    std::shared_ptr<const MoveEffect> contents(std::shared_ptr<const MoveEffect> effect) {
      if (std::ranges::any_of(moves, [&](const auto& parent) {
            return (parent->hasContents() && parent->encloses(effect->destination()))
                || effect->encloses(parent->destination());
          })) {
        return effect->captureContents(remaining, token);
      }
      return effect;
    }
    bool room() const { return moves.size() + preparedMoves.size() + retainedFolders.size() + copiedRoots() < budget; }
    std::shared_ptr<const MoveReceipt>
    capture(std::size_t moved, std::size_t merged, CopiedMoveCapture& copied, bool& changed) const {
      if (!collecting || moved != moves.size() + copied.prepared.size() || merged != retainedFolders.size()) {
        return {};
      }
      try {
        std::vector<std::shared_ptr<const MoveFolder>> folders;
        folders.reserve(retainedFolders.size());
        for (const auto& retained : retainedFolders) {
          folders.push_back(retained.effect);
        }
        auto native = MoveReceipt::capture(moves, folders, false, false, token, copied.destinations());
        if (copied.prepared.empty()) {
          return native;
        }
        if (!native && (!moves.empty() || !folders.empty())) {
          return {};
        }
        auto receipt = copied.capture(copied.prepared.size(), token, std::move(native));
        changed |= copied.captureFailed;
        return receipt;
      } catch (const FileErrorException&) {
        changed = true;
        return {};
      }
    }
    void attach(Work& work) {
      if (collecting) {
        work.copiedMergeParent = [&](const Location& folder) {
          return collecting && std::ranges::any_of(moves, [&](const auto& effect) {
                   return effect->destination() == folder && effect->hasContents();
                 });
        };
        work.prepareMerge = [&](const Location& folder) {
          if (!collecting) {
            return;
          }
          try {
            for (auto& effect : moves) {
              if (effect->encloses(folder) && !effect->hasContents()) {
                effect = effect->captureContents(remaining, token);
              }
            }
          } catch (const FileErrorException&) {
            collecting = false;
          }
        };
        work.recordMove = [&](const Source& source, const Location& destination, int target, int item) {
          try {
            if (!collecting || !room() || item < 0 || !sameIdentity(source.metadata, statFd(item))) {
              collecting = false;
              return false;
            }
            revisions->renamed(source.metadata, statFd(item));
            moves.push_back(contents(
                MoveEffect::capture(
                    source.location, destination, source.parent.get(), target, item, revisions,
                    retention(*destination.parent(), target)
                )
            ));
            return true;
          } catch (const FileErrorException&) {
            collecting = false;
            return false;
          }
        };
        work.prepareMove = [&](const Source& source, const Location& destination,
                               int target) -> std::shared_ptr<const MoveEffect> {
          if (source.metadata.st_dev != statFd(target).st_dev) {
            return {}; // Expected staged copy/remove, not a failed native proof.
          }
          try {
            if (!sameMount(source.parent.get(), target)) {
              return {};
            }
          } catch (const FileErrorException&) {
            // An unavailable optional mount query must not fail the forward transfer.
          }
          if (collecting && room()) {
            try {
              std::vector<std::shared_ptr<const MoveFolder>> folders;
              folders.reserve(retainedFolders.size());
              for (const auto& folder : retainedFolders) {
                folders.push_back(folder.effect);
              }
              moves = MoveReceipt::checkpoint(moves, destination, remaining, token, folders);
              auto proof = MoveEffect::prepareReplacement(
                  source.location, destination, source.parent.get(), target, remaining, token, revisions,
                  retention(*destination.parent(), target)
              );
              if (proof) {
                preparedMoves.push_back({proof, source.index});
                return proof;
              }
            } catch (const FileErrorException&) {
            }
          }
          collecting = false;
          return {};
        };
        work.discardPreparedMove = [&](const std::shared_ptr<const MoveEffect>& proof) {
          std::erase_if(preparedMoves, [&](const auto& prepared) { return prepared.effect == proof; });
        };
        work.recordPreparedMove = [&](const std::shared_ptr<const MoveEffect>& proof) {
          try {
            moves.push_back(contents(proof->publishedReplacement()));
          } catch (const FileErrorException&) {
            collecting = false;
            throw;
          }
        };
        work.recordMerge = [&](const Source& source, const Location& folder, int target) {
          if (collecting && room() && remaining) {
            try {
              auto proof = MoveFolder::retain(
                  source.location, source.parent.get(), folder, target, token, revisions,
                  sameMount(source.parent.get(), target) ? retention(folder, target) : sourceRetention(source)
              );
              if (proof) {
                --remaining;
                retainedFolders.push_back({std::move(proof), source.index});
                return true;
              }
            } catch (const FileErrorException&) {
              collecting = false;
              // Once source retention committed, its reported recovery path must survive.
              if (::faccessat(source.parent.get(), source.name.c_str(), F_OK, AT_SYMLINK_NOFOLLOW) != 0
                  && errno == ENOENT) {
                throw;
              }
            }
          }
          collecting = false;
          return false;
        };
      }
    }
  };
  bool sourcePendingRetry(const Job& job, std::size_t index) {
    if (job.provider) {
      if (index >= job.outcomes.size()) {
        return true;
      }
      const auto value = job.outcomes[index].outcome;
      return value != luciole::TransferOutcome::Moved
          && value != luciole::TransferOutcome::Copied
          && value != luciole::TransferOutcome::Linked
          && value != luciole::TransferOutcome::Skipped;
    }
    return index < job.nodes.size() && job.nodes[index] && retryCount(*job.nodes[index]) > 0;
  }

  std::vector<luciole::Location> guardSourcesFor(const Job& job, bool retry) {
    if (!retry) {
      return job.request.sources;
    }
    std::vector<luciole::Location> sources;
    for (std::size_t i = 0; i < job.request.sources.size(); ++i) {
      if (sourcePendingRetry(job, i)) {
        sources.push_back(job.request.sources[i]);
      }
    }
    return sources;
  }

  std::vector<luciole::Fd>
  openTransferMounts(const Job& job, bool retry, bool allowMerge, const luciole::CancelToken& token) {
    const auto pendingSources = guardSourcesFor(job, retry);
    auto mounts = luciole::openSystemMounts(pendingSources, token, false);
    if (auto destination = luciole::openSystemMount(job.request.destination, token, true); destination.valid()) {
      mounts.push_back(std::move(destination));
    }
    if (job.request.action == luciole::TransferAction::Link) {
      return mounts;
    }
    const auto append = [&](std::vector<luciole::Fd> opened) {
      for (auto& fd : opened) {
        mounts.push_back(std::move(fd));
      }
    };
    append(luciole::openSystemMountsInside(pendingSources, token));
    if (!job.request.destination.isNative()) {
      return mounts;
    }
    std::vector<luciole::Location> destinationChildren;
    std::vector<luciole::Location> mergeDirectories;
    destinationChildren.reserve(pendingSources.size());
    for (const auto& source : pendingSources) {
      if (const auto base = source.basename(); base && !base->empty()) {
        auto target = job.request.destination.child(*base);
        if (allowMerge) {
          if (const auto targetPath = target.nativePath()) {
            struct stat entry{};
            if (::lstat(targetPath->c_str(), &entry) == 0 && S_ISDIR(entry.st_mode)) {
              mergeDirectories.push_back(target);
            }
          }
        }
        destinationChildren.push_back(std::move(target));
      }
    }
    append(luciole::openSystemMounts(destinationChildren, token, false));
    if (!mergeDirectories.empty()) {
      append(luciole::openSystemMountsInside(mergeDirectories, token));
    }
    return mounts;
  }

  void updatePendingResult(
      luciole::TransferResult& result, luciole::TransferOutcome outcome, const luciole::FileError& error
  ) {
    if (result.outcome != luciole::TransferOutcome::Partial) {
      result.outcome = outcome;
    }
    result.error = error;
    for (auto& child : result.children) {
      if (child.retryable || child.outcome == luciole::TransferOutcome::Partial) {
        updatePendingResult(child, outcome, error);
      }
    }
  }

  std::vector<luciole::TransferResult> failedResults(Job& job, bool retry, const luciole::FileError& failure) {
    const bool cancelled = failure.kind == luciole::FileErrorKind::Cancelled;
    const auto outcome = cancelled ? luciole::TransferOutcome::Cancelled : luciole::TransferOutcome::Failed;
    const auto error = cancelled ? cancelledError() : failure;
    if (retry && job.outcomes.size() == job.request.sources.size()) {
      auto results = job.outcomes;
      for (std::size_t i = 0; i < results.size(); ++i) {
        if (sourcePendingRetry(job, i)) {
          updatePendingResult(results[i], outcome, error);
        }
      }
      job.outcomes = results;
      return results;
    }
    std::vector<luciole::TransferResult> results;
    results.reserve(job.request.sources.size());
    for (const auto& source : job.request.sources) {
      results.emplace_back(source, outcome, error);
    }
    return results;
  }
} // namespace

bool luciole::TransferService::launch(TransferRequest request, bool retry) {
  if (busy() || request.sources.empty()) {
    return false;
  }
  m_state->active = true;
  m_state->action = request.action;
  m_state->total = request.sources.size();
  m_state->completed.store(0);
  m_state->bytes.store(0);
  m_state->token = CancelToken{};
  m_state->receipt.reset();
  m_state->copyReceipt.reset();
  m_state->historyRetired = false;
  if (!retry) {
    ++m_state->jobId;
    m_state->results.clear();
    m_state->report.clear();
    m_state->retryable = 0;
    m_state->job.reset();
  }
  auto job = retry ? m_state->job : std::make_shared<Job>(std::move(request));
  runJob<Reply>(
      [owner = m_state, job, token = m_state->token, retry] {
        const bool allowMerge = job->request.allowMerge && job->request.action != TransferAction::Link;
        const auto mounts = openTransferMounts(*job, retry, allowMerge, token);
        std::map<std::pair<mode_t, mode_t>, TransferConflictChoice> policies;
        Work work{
            token, owner->bytes,
            conflictAsker(owner->gate, [owner] { State::notify(owner); }, token, allowMerge, policies)
        };
        work.allowMerge = allowMerge;
        auto revisions = std::make_shared<HistoryRevisions>();
        work.history = revisions;
        std::vector<std::shared_ptr<const CopyEffect>> copies;
        std::size_t publishedCopies = 0;
        bool checkpointFailed = false;
        struct PreparedCopy {
          std::shared_ptr<const CopyEffect> effect;
          std::size_t source;
        };
        // Keep displaced data alive until every source has finished, even if optional history is disqualified.
        std::vector<PreparedCopy> preparedCopies;
        const auto budget = !retry ? transferHistoryBudget(job->request.action == TransferAction::Move ? 14 : 7) : 0;
        std::size_t remaining = 16384;
        MoveCapture moveHistory(
            job->request.action == TransferAction::Move ? budget : 0, token, revisions, job->request.destination,
            job->request.sources
        );
        moveHistory.attach(work);
        CopiedMoveCapture copiedMoves;
        moveHistory.copiedRoots = [&] { return copiedMoves.prepared.size(); };
        if (!retry
            && job->request.action == TransferAction::Move
            && job->request.destination.isNative()
            && std::ranges::all_of(job->request.sources, [](const auto& source) { return source.isNative(); })) {
          copiedMoves.attach(
              work, moveHistory.remaining, [&] { return moveHistory.collecting && moveHistory.room(); },
              [&](const Location& destination, int parent) {
                return moveHistory.retention(*destination.parent(), parent);
              }
          );
        }
        Fd copyRetentionParent;
        bool collectingCopies =
            budget && job->request.action != TransferAction::Move && job->request.destination.isNative();
        if (collectingCopies) {
          work.checkpointCopy = [&](const Location& destination, bool replacing) {
            if (!collectingCopies || !replacing) {
              return;
            }
            try {
              copies = CopyReceipt::checkpoint(copies, destination, job->request.action, token);
            } catch (const FileErrorException&) {
              checkpointFailed = true;
              collectingCopies = false;
              copies.clear();
            }
          };
          work.prepareCopy = [&](std::size_t source, const Location& destination, int parent, int stage,
                                 bool replacing) -> std::shared_ptr<const CopyEffect> {
            if (collectingCopies) {
              try {
                if (preparedCopies.size() < budget) {
                  if (!copyRetentionParent.valid()) {
                    copyRetentionParent = copyHistoryParent(job->request.destination);
                  }
                  auto proof = CopyEffect::prepare(destination, parent, stage, replacing, remaining, token, revisions);
                  preparedCopies.push_back({proof, source});
                  return proof;
                }
              } catch (const FileErrorException&) {
                // Optional history must not fail a completed copy or link transfer.
              }
              collectingCopies = false;
              copies.clear();
            }
            return {};
          };
          work.recordCopy = [&](const std::shared_ptr<const CopyEffect>& prepared, int stage) {
            try {
              copies.push_back(prepared->published(stage, copyRetentionParent.get()));
              ++publishedCopies;
            } catch (const FileErrorException&) {
              collectingCopies = false;
              copies.clear();
              if (prepared->replacing()) {
                // Publication already displaced an item; failure to retain it must remain actionable.
                throw;
              }
            }
          };
        }
        work.changedLinks = job->changedLinks;
        work.mergedItems = job->mergedItems;
        const bool remote = !job->request.destination.isNative()
            || std::ranges::any_of(job->request.sources, [](const auto& source) { return !source.isNative(); });
        if (remote && !job->provider) {
          job->provider = std::make_unique<ProviderTransfer>(job->request);
        }
        auto outcomes = remote
            ? job->provider->run(
                  token, owner->bytes, owner->completed,
                  [&](TransferConflict conflict) {
                    const auto answer = owner->gate.ask(std::move(conflict), token, [owner] { State::notify(owner); });
                    return answer ? answer->choice : TransferConflictChoice::Cancel;
                  },
                  &work, !retry && !job->request.destination.isNative()
              )
            : retry ? retryJob(*job, work, owner->completed)
                    : runFirst(*job, work, owner->completed);
        job->changedLinks = std::move(work.changedLinks);
        job->mergedItems = work.mergedItems;
        std::shared_ptr<const MoveReceipt> receipt;
        std::shared_ptr<const CopyReceipt> copyReceipt;
        std::size_t moved = 0, merged = 0;
        const bool movesComplete = completedMoves(*job, outcomes, moved, merged);
        bool moveCaptureChanged = false;
        if (!retry && !remote && movesComplete) {
          receipt = moveHistory.capture(moved, merged, copiedMoves, moveCaptureChanged);
        }
        if (!receipt && (!movesComplete || moveCaptureChanged || copiedMoves.captureFailed)) {
          copiedMoves.preserve(outcomes);
        }
        if (!receipt && !retry && job->provider) {
          receipt = job->provider->takeMoveReceipt(token);
        }
        if (!receipt && (moveCaptureChanged || !movesComplete)) {
          const auto retain = [&](const auto& prepared) {
            for (const auto& location : prepared.effect->retainForRecovery()) {
              if (prepared.source < outcomes.size()) {
                auto& result = outcomes[prepared.source];
                if (!result.error) {
                  result.error = FileError{
                      .kind = FileErrorKind::Other, .message = "Undo is unavailable for this incomplete move."
                  };
                }
                result.error->message += " Retained item for inspection: " + location.uri();
              }
            }
          };
          for (const auto& prepared : moveHistory.preparedMoves) {
            retain(prepared);
          }
          for (const auto& retained : moveHistory.retainedFolders) {
            retain(retained);
          }
        }
        const auto copied = completedCopies(*job, outcomes);
        bool captureChanged = checkpointFailed;
        if (!retry && collectingCopies && copied && *copied > 0 && *copied == publishedCopies) {
          try {
            copyReceipt = CopyReceipt::capture(copies, job->request.action, token);
          } catch (const FileErrorException&) {
            // Successful effects remain successful if optional history cannot be captured.
            captureChanged = true;
          }
        }
        if (!copyReceipt && !retry && job->provider) {
          copyReceipt = job->provider->takeCopyReceipt(token);
        }
        if (!copyReceipt && (captureChanged || !copied)) {
          for (const auto& prepared : preparedCopies) {
            for (const auto& retained : prepared.effect->retainForRecovery()) {
              if (prepared.source < outcomes.size()) {
                auto& result = outcomes[prepared.source];
                if (!result.error) {
                  result.error = FileError{
                      .kind = FileErrorKind::Other, .message = "Undo is unavailable for this incomplete operation."
                  };
                }
                result.error->message += " Previous destination retained for inspection: " + retained.uri();
              }
            }
          }
        }
        job->outcomes = outcomes;
        auto report = jobReport(outcomes);
        return Reply{
            .results = std::move(outcomes),
            .report = std::move(report),
            .receipt = std::move(receipt),
            .copyReceipt = std::move(copyReceipt)
        };
      },
      [owner = m_state, job, retry](JobResult<Reply> result) {
        if (result) {
          owner->results = std::move(result->results);
          owner->report = std::move(result->report);
          owner->receipt = std::move(result->receipt);
          owner->copyReceipt = std::move(result->copyReceipt);
        } else {
          owner->results = failedResults(*job, retry, result.error());
          owner->report = jobReport(owner->results);
        }
        owner->job = job;
        owner->retryable = job->provider ? job->provider->retryable() : 0;
        for (const auto& node : job->nodes) {
          if (node) {
            owner->retryable += retryCount(*node);
          }
        }
        if (owner->retryable == 0 || (!result && !retry)) {
          owner->retryable = 0;
          owner->job.reset();
        }
        owner->active = false;
        const auto callback = owner->changed;
        if (callback) {
          callback();
        }
      }
  );
  const auto callback = m_state->changed;
  if (callback) {
    callback();
  }
  return true;
}

bool luciole::TransferService::replay(const std::shared_ptr<const MoveReceipt>& receipt) {
  if (busy() || !receipt || receipt->size() == 0) {
    return false;
  }
  m_state->active = true;
  m_state->action = TransferAction::Move;
  m_state->total = receipt->size();
  m_state->completed.store(0);
  m_state->bytes.store(0);
  m_state->token = CancelToken{};
  m_state->results.clear();
  m_state->report.clear();
  m_state->receipt.reset();
  m_state->copyReceipt.reset();
  m_state->historyRetired = false;
  m_state->retryable = 0;
  m_state->job.reset();
  ++m_state->jobId;
  runJob<MoveReplayResult>(
      [owner = m_state, receipt, token = m_state->token] { return receipt->reverse(token, owner->completed); },
      [owner = m_state, receipt](JobResult<MoveReplayResult>&& result) {
        if (result) {
          owner->results = std::move(result->results);
          owner->receipt = std::move(result->receipt);
          owner->historyRetired = result->retireHistory;
        } else {
          owner->historyRetired = true;
          owner->results = receipt->unconfirmed(result.error());
        }
        owner->report = jobReport(owner->results);
        owner->active = false;
        const auto callback = owner->changed;
        if (callback) {
          callback();
        }
      }
  );
  const auto callback = m_state->changed;
  if (callback) {
    callback();
  }
  return true;
}

bool luciole::TransferService::replayCopy(const std::shared_ptr<const CopyReceipt>& receipt) {
  if (busy() || !receipt || receipt->size() == 0) {
    return false;
  }
  m_state->active = true;
  m_state->action = receipt->action();
  m_state->total = receipt->size();
  m_state->completed.store(0);
  m_state->bytes.store(0);
  m_state->token = CancelToken{};
  m_state->results.clear();
  m_state->report.clear();
  m_state->receipt.reset();
  m_state->copyReceipt.reset();
  m_state->historyRetired = false;
  m_state->retryable = 0;
  m_state->job.reset();
  ++m_state->jobId;
  runJob<CopyReplayResult>(
      [owner = m_state, receipt, token = m_state->token] { return receipt->reverse(token, owner->completed); },
      [owner = m_state, receipt](JobResult<CopyReplayResult>&& result) {
        if (result) {
          owner->results = std::move(result->results);
          owner->copyReceipt = std::move(result->receipt);
          owner->historyRetired = result->retireHistory;
        } else {
          owner->historyRetired = true;
          owner->results = receipt->unconfirmed(result.error());
        }
        owner->report = jobReport(owner->results);
        owner->active = false;
        const auto callback = owner->changed;
        if (callback) {
          callback();
        }
      }
  );
  const auto callback = m_state->changed;
  if (callback) {
    callback();
  }
  return true;
}
