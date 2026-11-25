#include "operations/transfer_staging.h"

#include "fs/gio_ptr.h"
#include "operations/copied_move_receipt.h"
#include "operations/move_receipt.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <format>
#include <glib.h>
#include <optional>
#include <set>
#include <unistd.h>
#include <utility>

namespace luciole::transfer {
  namespace {
    // Appends text to an item's error, creating one of kind when none is set.
    void appendError(std::optional<FileError>& error, FileErrorKind kind, const std::string& text) {
      if (!error) {
        error = FileError{.kind = kind, .message = {}};
      }
      error->message += text;
    }

    std::string numberedName(const Source& source, int destination, std::uint64_t number) {
      const auto dot = source.name.find_last_of('.');
      const bool extension = !S_ISDIR(source.metadata.st_mode) && dot != std::string::npos && dot != 0;
      const auto suffix = extension ? source.name.substr(dot) : std::string{};
      auto stem = extension ? source.name.substr(0, dot) : source.name;
      const auto counter = " (" + std::to_string(number) + ")";
      const auto limit = ::fpathconf(destination, _PC_NAME_MAX);
      if (limit > 0 && stem.size() + counter.size() + suffix.size() > static_cast<std::size_t>(limit)) {
        if (counter.size() + suffix.size() >= static_cast<std::size_t>(limit)) {
          throwFileError(FileErrorKind::NameTooLong, "The numbered filename would be too long.");
        }
        auto bytes = static_cast<std::size_t>(limit) - counter.size() - suffix.size();
        if (g_utf8_validate(stem.data(), static_cast<gssize>(stem.size()), nullptr)) {
          while (bytes > 0 && (static_cast<unsigned char>(stem[bytes]) & 0xc0U) == 0x80U) {
            --bytes;
          }
        }
        stem.resize(bytes);
      }
      return stem + counter + suffix;
    }

    void validateDestination(const Destination& destination) {
      for (const auto* current = &destination; current; current = current->ancestor.get()) {
        struct stat value{};
        const auto path = *current->location.nativePath();
        const int result = current->ancestor ? ::lstat(path.c_str(), &value) : ::stat(path.c_str(), &value);
        if (result != 0 || !sameIdentity(value, current->metadata)) {
          throwFileError(FileErrorKind::Changed, "The destination folder moved or was replaced. Start a new transfer.");
        }
      }
    }

    void validateBindings(const Source& source, const Destination& destination) {
      const auto parent = openSourceParent(source);
      if (!sameIdentity(statFd(parent.get()), statFd(source.parent.get()))) {
        throwFileError(FileErrorKind::Changed, "The source parent moved or was replaced.");
      }
      validateDestination(destination);
    }

    TransferResult merge(Node& node, int destination, TransferAction action, Work& work, const struct stat& existing);

    bool chooseTarget(
        const Source& source, int destination, Work& work, std::string& target, bool& keepBoth, std::uint64_t& number,
        std::optional<struct stat>& replace, bool& merging, const Destination& binding, bool forcePrompt = false
    ) {
      for (;;) {
        work.check();
        if (keepBoth) {
          target = numberedName(source, destination, ++number);
        }
        struct stat existing{};
        if (::fstatat(destination, target.c_str(), &existing, AT_SYMLINK_NOFOLLOW) != 0) {
          if (errno == ENOENT) {
            replace.reset();
            return true;
          }
          fail("Could not inspect destination");
        }
        if (keepBoth) {
          continue;
        }
        if (replace && unchanged(*replace, existing)) {
          return true;
        }
        replace.reset();
        validateBindings(source, binding);
        const auto choice = work.choose(source, binding.location, target, existing, forcePrompt);
        validateBindings(source, binding);
        requireUnchanged(work.expected(source.metadata), info(source.parent.get(), source.name));
        switch (choice) {
        case TransferConflictChoice::Merge:
          if (!work.allowMerge || !S_ISDIR(source.metadata.st_mode) || !S_ISDIR(existing.st_mode)) {
            throwFileError(FileErrorKind::InvalidArgument, "Only two actual folders can be merged.");
          }
          replace = existing;
          merging = true;
          return true;
        case TransferConflictChoice::Skip:
          return false;
        case TransferConflictChoice::Cancel:
          work.token.cancel();
          throw TransferCancelled();
        case TransferConflictChoice::KeepBoth:
          keepBoth = true;
          break;
        case TransferConflictChoice::Replace:
          replace = existing;
          return true;
        }
      }
    }

    void recordPublication(
        const std::shared_ptr<const CopyEffect>& copyProof, const std::shared_ptr<const CopiedMoveEffect>& moveProof,
        bool replacing, Stage& stage, Work& work
    ) {
      if (replacing) {
        if (copyProof || moveProof) {
          stage.keepContents();
        }
        work.note(info(stage.fd(), "item"));
      }
      if (copyProof && work.recordCopy) {
        work.recordCopy(copyProof, stage.fd());
      }
      if (moveProof) {
        moveProof->published(stage.fd());
      }
    }

    void queueSourceRemoval(
        Node& node, const Tree& tree, int destination, std::shared_ptr<const CopiedMoveEffect> proof, Work& work
    ) {
      work.pendingMoves.push_back(
          {.node = &node,
           .tree = tree,
           .destination = opened(::fcntl(destination, F_DUPFD_CLOEXEC, 0)),
           .outcome = TransferOutcome::Moved,
           .proof = std::move(proof)}
      );
    }

    void
    checkReplacementSource(const Source& source, TransferAction action, const std::optional<struct stat>& replace) {
      if (action != TransferAction::Move
          && replace
          && S_ISDIR(replace->st_mode)
          && (sameIdentity(source.metadata, *replace) || hasAncestor(source.parent.get(), *replace))) {
        throwFileError(FileErrorKind::WouldRecurse, "Replacement would remove the transfer source.");
      }
    }

    TransferResult transferItem(Node& node, int destination, TransferAction action, Work& work) {
      auto& source = node.source;
      const auto& binding = *node.destination;
      using enum TransferOutcome;
      TransferResult result{source.location};
      try {
        work.check();
        source.metadata = work.expected(source.metadata);
        requireUnchanged(source.metadata, info(source.parent.get(), source.name));
        const bool sameFolder = sameIdentity(statFd(source.parent.get()), statFd(destination));
        if (sameFolder && action == TransferAction::Move) {
          result.outcome = Skipped;
          return result;
        }
        if (S_ISDIR(source.metadata.st_mode) && hasAncestor(destination, source.metadata)) {
          throwFileError(FileErrorKind::WouldRecurse, "A folder cannot be placed inside itself.");
        }
        if (!S_ISDIR(source.metadata.st_mode)
            && !S_ISREG(source.metadata.st_mode)
            && !S_ISLNK(source.metadata.st_mode)) {
          throwFileError(FileErrorKind::Unsupported, "Special files cannot be transferred.");
        }
        std::string target = source.name;
        bool keepBoth = sameFolder, merging = false, forcePrompt = false;
        std::uint64_t number = 0;
        std::optional<struct stat> replace;
        for (;;) {
          if (!chooseTarget(
                  source, destination, work, target, keepBoth, number, replace, merging, binding, forcePrompt
              )) {
            result.outcome = Skipped;
            return result;
          }
          if (merging) {
            if (!unchanged(*replace, info(destination, target))) {
              merging = false;
              replace.reset();
              forcePrompt = true;
              continue;
            }
            return merge(node, destination, action, work, *replace);
          }
          validateBindings(source, binding);
          if (action == TransferAction::Move && replace && work.prepareMove) {
            auto proof = work.prepareMove(source, binding.location.child(target), destination);
            if (proof) {
              work.check();
              requireUnchanged(source.metadata, info(source.parent.get(), source.name));
              requireUnchanged(*replace, info(destination, target));
              if (renameExchange(source.parent.get(), source.name.c_str(), destination, target.c_str()) != 0) {
                if (errno != EXDEV) {
                  fail("Could not exchange moved item");
                }
                if (work.discardPreparedMove) {
                  work.discardPreparedMove(proof);
                }
                break; // No exchange occurred; use the verified copy/remove path instead.
              }
              result.outcome = Partial;
              result.destination = binding.location.child(target);
              try {
                work.note(info(destination, target));
                work.note(info(source.parent.get(), source.name));
                work.recordPreparedMove(proof);
                result.outcome = Moved;
              } catch (const std::exception&) {
                result.error = currentFileError();
                result.error->message +=
                    " Inspect both the destination and original source name for the exchanged items.";
              }
              return result;
            }
          }
          if (action != TransferAction::Move || replace) {
            break;
          }
          work.check();
          requireUnchanged(source.metadata, info(source.parent.get(), source.name));
          Fd historyItem;
          if (work.recordMove) {
            historyItem = Fd(::openat(source.parent.get(), source.name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
          }
          if (renameNoReplace(source.parent.get(), source.name.c_str(), destination, target.c_str()) == 0) {
            work.note(info(destination, target));
            result.outcome = Moved;
            result.destination = binding.location.child(target);
            if (work.recordMove && !work.recordMove(source, *result.destination, destination, historyItem.get())) {
              work.recordMove = {};
            }
            return result;
          }
          if (errno == EXDEV) {
            break;
          }
          if (errno != EEXIST) {
            fail("Could not move item");
          }
        }
        if (action != TransferAction::Move && work.checkpointCopy) {
          work.checkpointCopy(binding.location.child(target), replace.has_value());
        }
        Stage stage(destination, work);
        work.metadataIncomplete = false;
        bool published = false;
        bool deferred = false;
        try {
          std::optional<Tree> tree;
          if (action == TransferAction::Link) {
            if (::symlinkat(source.location.nativePath()->c_str(), stage.fd(), "item") != 0) {
              fail("Could not create symbolic link");
            }
          } else {
            tree = copyTree(source.parent.get(), source.name, stage.fd(), "item", work, 0);
            requireUnchanged(source.metadata, tree->metadata);
          }
          const auto verifySource = [&] {
            if (tree) {
              verifyTree(source.parent.get(), *tree, work);
            } else {
              requireUnchanged(source.metadata, info(source.parent.get(), source.name));
            }
          };
          // Without a pending conflict the loop below verifies again before anything else can happen.
          if (replace || merging) {
            verifySource();
          }
          for (;;) {
            work.check();
            if (replace) {
              struct stat current{};
              if (::fstatat(destination, target.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
                  || !unchanged(*replace, current)) {
                replace.reset();
                if (!chooseTarget(
                        source, destination, work, target, keepBoth, number, replace, merging, binding, true
                    )) {
                  result.outcome = Skipped;
                  break;
                }
                continue;
              }
            }
            if (merging) {
              // The conflicting folder appeared after staging. No staged item has been published.
              if (!stage.clear()) {
                throwFileError(FileErrorKind::Other, "Could not remove staging before merging: " + stage.name());
              }
              return merge(node, destination, action, work, *replace);
            }
            validateBindings(source, binding);
            verifySource();
            if (replace) {
              // The exchange retires the old destination: make the copy durable first. Plain copies skip this,
              // like cp and GIO.
              synchronize(stage.fd());
            }
            auto copyProof = action != TransferAction::Move && work.prepareCopy
                ? work.prepareCopy(
                      source.index, binding.location.child(target), destination, stage.fd(), replace.has_value()
                  )
                : nullptr;
            auto moveProof = action == TransferAction::Move && work.prepareCopiedMove
                ? work.prepareCopiedMove(
                      source, binding.location.child(target), destination, stage.fd(), replace.has_value()
                  )
                : nullptr;
            work.check();
            checkReplacementSource(source, action, replace);
            const auto placed = replace ? renameExchange(stage.fd(), "item", destination, target.c_str())
                                        : renameNoReplace(stage.fd(), "item", destination, target.c_str());
            if (placed != 0) {
              if (errno == EEXIST || (replace && errno == ENOENT)) {
                replace.reset();
                if (!chooseTarget(
                        source, destination, work, target, keepBoth, number, replace, merging, binding, true
                    )) {
                  result.outcome = Skipped;
                  break;
                }
                continue;
              }
              fail("Could not place item in destination");
            }
            published = true;
            result.destination = binding.location.child(target);
            if (replace && !sameRenamedRevision(*replace, info(stage.fd(), "item"), true)) {
              stage.retain();
              throwFileError(
                  FileErrorKind::Changed, "The previous destination changed and was retained in " + stage.name()
              );
            }
            result.outcome = action == TransferAction::Link ? Linked : Copied;
            recordPublication(copyProof, moveProof, replace.has_value(), stage, work);
            if (action == TransferAction::Move) {
              // The sources go only after the published copy is durable; flushMoves() syncs a batch at once.
              queueSourceRemoval(node, *tree, destination, std::move(moveProof), work);
              deferred = true;
              result.outcome = Moved;
            }
            break;
          }
        } catch (const TransferCancelled&) {
          result.outcome = published ? Partial : Cancelled;
          result.error = FileError{
              .kind = FileErrorKind::Cancelled,
              .message =
                  published ? "The copy completed; cancellation left source items in place." : "Transfer cancelled.",
          };
        } catch (const std::exception&) {
          result.outcome = published ? Partial : Failed;
          result.error = currentFileError();
          if (published) {
            result.error->message =
                "The destination was created, but transfer cleanup is incomplete: " + result.error->message;
          }
        }
        if (!stage.clear()) {
          result.outcome = Partial;
          appendError(
              result.error, FileErrorKind::Other,
              " Previous or temporary items remain in " + stage.name() + " in the destination."
          );
        }
        if (work.metadataIncomplete && published) {
          appendError(
              result.error, FileErrorKind::Unsupported, " The destination does not support some extended attributes."
          );
        }
        if (deferred) {
          // Partial until flushMoves() removes the source; it then applies the outcome recorded here.
          work.pendingMoves.back().outcome = result.outcome;
          result.outcome = Partial;
        }
      } catch (const TransferCancelled&) {
        result.outcome = Cancelled;
        result.error = FileError{.kind = FileErrorKind::Cancelled, .message = "Transfer cancelled."};
      } catch (const std::exception&) {
        result.error = currentFileError();
      }
      result.retryable = result.outcome == Failed;
      return result;
    }

    TransferResult merge(Node& node, int destination, TransferAction action, Work& work, const struct stat& existing) {
      // Native move history captures changed leaves and retains emptied source containers.
      if (action != TransferAction::Copy) {
        work.prepareCopy = {};
        work.recordCopy = {};
      }
      using enum TransferOutcome;
      auto& source = node.source;
      TransferResult result{source.location};
      result.destination = node.destination->location.child(source.name);
      bool finishedChildren = false;
      try {
        work.check();
        validateBindings(source, *node.destination);
        auto input = directory(source.parent.get(), source.name);
        if (!sameIdentity(statFd(input.get()), source.metadata)
            || !sameIdentity(info(source.parent.get(), source.name), source.metadata)) {
          throwFileError(FileErrorKind::Changed, "The source folder changed.");
        }
        auto output = directory(destination, source.name);
        if (!sameIdentity(existing, statFd(output.get()))) {
          throwFileError(FileErrorKind::Changed, "The destination folder changed.");
        }
        if (hasAncestor(output.get(), source.metadata) || hasAncestor(input.get(), existing)) {
          throwFileError(FileErrorKind::WouldRecurse, "Related or identical folders cannot be merged.");
        }
        if (!node.merging) {
          if (work.prepareMerge) {
            work.prepareMerge(*result.destination);
          }
          requireUnchanged(source.metadata, statFd(input.get()));
          requireUnchanged(existing, info(destination, source.name));
          auto names = children(input.get(), 16384 - work.mergedItems);
          if (node.depth >= 256 || names.size() > 16384 - work.mergedItems) {
            throwFileError(
                FileErrorKind::Unsupported,
                "The merge exceeds 16384 items or 256 folder levels. Select smaller folders."
            );
          }
          work.mergedItems += names.size();
          std::ranges::sort(names);
          const auto childPath = *node.destination->location.nativePath() + "/" + source.name;
          auto target = std::make_shared<const Destination>(
              Destination{Location::fromPath(childPath), existing, node.destination}
          );
          std::vector<std::shared_ptr<Node>> planned;
          for (const auto& name : names) {
            work.check();
            const auto metadata = info(input.get(), name);
            auto location = Location::fromPath(*source.location.nativePath() + "/" + name);
            Source child{source.index,       location, Fd{}, name, metadata, *source.location.nativePath(),
                         statFd(input.get())};
            planned.push_back(
                std::make_shared<Node>(
                    std::move(child), target,
                    TransferResult{
                        location, Cancelled, FileError{.kind = FileErrorKind::Cancelled, .message = "Not attempted."}
                    },
                    node.depth + 1
                )
            );
          }
          requireUnchanged(source.metadata, statFd(input.get()));
          node.children = std::move(planned);
          node.mergeTarget = target;
          node.merging = true;
        }
        // Each child has its own commit boundary. Existing destination directory
        // metadata and destination-only entries are preserved.
        for (const auto& child : node.children) {
          validateBindings(source, *node.destination);
          validateDestination(*node.mergeTarget);
          if (!sameIdentity(source.metadata, info(source.parent.get(), source.name))) {
            throwFileError(FileErrorKind::Changed, "The source folder moved or was replaced.");
          }
          execute(*child, action, work, false);
        }
        finishedChildren = true;
        flushMoves(work);
        const bool complete = std::ranges::all_of(node.children, [action](const auto& child) {
          return child->result.outcome == (action == TransferAction::Copy ? Copied : Moved);
        });
        if (complete) {
          validateBindings(source, *node.destination);
          validateDestination(*node.mergeTarget);
          if (!sameIdentity(source.metadata, info(source.parent.get(), source.name))) {
            throwFileError(FileErrorKind::Changed, "The source folder moved or was replaced.");
          }
          if (action == TransferAction::Move) {
            work.check();
            if (!children(input.get()).empty()) {
              throwFileError(
                  FileErrorKind::Changed, "New source entries remain; they were not included in this merge."
              );
            }
            if ((!work.recordMerge || !work.recordMerge(source, *result.destination, output.get()))
                && ::unlinkat(source.parent.get(), source.name.c_str(), AT_REMOVEDIR) != 0) {
              fail("Could not remove empty merged source folder");
            }
            result.outcome = Moved;
          } else {
            requireUnchanged(source.metadata, statFd(input.get()));
            result.outcome = Copied;
          }
          return result;
        }
      } catch (const TransferCancelled&) {
        result.error = FileError{
            .kind = FileErrorKind::Cancelled, .message = "Merge cancelled. Completed child transfers remain applied."
        };
      } catch (const std::exception&) {
        result.error = currentFileError();
        // Only an empty source-folder cleanup can be retried here. Publication or
        // source-removal partials of individual files are never replayed.
        result.retryable = finishedChildren
            && action == TransferAction::Move
            && std::ranges::all_of(node.children, [](const auto& child) { return child->result.outcome == Moved; });
      }
      std::size_t done = 0, failed = 0, skipped = 0, cancelled = 0;
      for (const auto& child : node.children) {
        const auto value = child->result.outcome;
        done += value == Moved || value == Copied || value == Partial ? 1 : 0;
        failed += value == Failed || value == Partial ? 1 : 0;
        skipped += value == Skipped ? 1 : 0;
        cancelled += value == Cancelled ? 1 : 0;
      }
      result.outcome = done > 0                    ? Partial
          : failed > 0 || result.error.has_value() ? (work.token.cancelled() ? Cancelled : Failed)
          : cancelled > 0                          ? Cancelled
                                                   : Skipped;
      result.retryable = result.retryable || (!node.merging && result.outcome == Failed);
      if (!result.error) {
        result.error = FileError{
            .kind = FileErrorKind::Other,
            .message = std::format(
                "Merge: {} completed, {} failed/partial, {} skipped, {} cancelled.", done, failed, skipped, cancelled
            ),
        };
        const auto problem =
            std::ranges::find_if(node.children, [](const auto& child) { return child->result.error.has_value(); });
        if (problem != node.children.end()) {
          const auto& error = *(*problem)->result.error;
          result.error->kind = error.kind;
          result.error->message += " " + (*problem)->source.location.uri() + ": " + error.message;
        }
      }
      return result;
    }
  } // namespace

  Stage::Stage(int parent, CopyWork& work) : m_parent(parent), m_work(work) {
    const GString id(g_uuid_string_random());
    m_name = ".luciole-transfer-" + std::string(id.get());
    if (::mkdirat(parent, m_name.c_str(), 0700) != 0) {
      fail("Could not prepare destination");
    }
    try {
      m_fd = directory(parent, m_name);
    } catch (...) {
      ::unlinkat(parent, m_name.c_str(), AT_REMOVEDIR);
      throw;
    }
  }

  Stage::~Stage() {
    if (!m_cleaned) {
      (void)clear();
    }
  }

  bool Stage::clear() noexcept {
    if (m_cleaned) {
      return true;
    }
    m_cleaned = true;
    try {
      const auto names = children(m_fd.get());
      if (m_keepContents && !names.empty()) {
        return false;
      }
      for (const auto& name : names) {
        removeTemporary(m_fd.get(), name, m_work);
      }
      return ::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR) == 0;
    } catch (...) {
      return false;
    }
  }

  Fd openSourceParent(const Source& source) {
    auto fd = opened(::open(source.parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!sameIdentity(source.parentMetadata, statFd(fd.get()))) {
      throwFileError(FileErrorKind::Changed, "The source folder changed.");
    }
    return fd;
  }

  std::size_t retryCount(const Node& node) {
    std::size_t count = node.result.retryable ? 1 : 0;
    for (const auto& child : node.children) {
      count += retryCount(*child);
    }
    return count;
  }

  TransferResult snapshot(const Node& node) {
    auto result = node.result;
    result.children.clear();
    for (const auto& child : node.children) {
      result.children.push_back(snapshot(*child));
    }
    return result;
  }

  void execute(Node& node, TransferAction action, Work& work, bool retry) {
    // Completed/skipped/partial leaf operations are terminal. A retry visits only
    // captured failures, or an existing merge container with failed descendants.
    if (retry && retryCount(node) == 0) {
      return;
    }
    if (!retry && node.attempted) {
      return;
    }
    if (node.merging && retry) {
      for (auto& child : node.children) {
        execute(*child, action, work, true);
      }
    }
    try {
      work.check();
      node.source.parent = openSourceParent(node.source);
      validateBindings(node.source, *node.destination);
      auto destination =
          opened(::open(node.destination->location.nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!sameIdentity(statFd(destination.get()), node.destination->metadata)) {
        throwFileError(FileErrorKind::Changed, "The destination folder changed. Start a new transfer.");
      }
      if (node.merging) {
        const auto existing = info(destination.get(), node.source.name);
        // The child destination retains the original merge target's identity.
        validateDestination(*node.mergeTarget);
        node.result = merge(node, destination.get(), action, work, existing);
      } else {
        requireUnchanged(work.expected(node.source.metadata), info(node.source.parent.get(), node.source.name));
        node.result = transferItem(node, destination.get(), action, work);
        if (work.pendingMoves.size() >= kMoveSyncBatch) {
          flushMoves(work);
        }
      }
    } catch (const TransferCancelled&) {
      const bool effects = std::ranges::any_of(node.children, [](const auto& child) {
        return child->result.outcome == TransferOutcome::Moved
            || child->result.outcome == TransferOutcome::Copied
            || child->result.outcome == TransferOutcome::Partial;
      });
      node.result = {
          node.source.location, effects ? TransferOutcome::Partial : TransferOutcome::Cancelled,
          FileError{
              .kind = FileErrorKind::Cancelled,
              .message = "Transfer cancelled. Completed child transfers remain applied."
          }
      };
    } catch (const std::exception&) {
      node.result = {
          node.source.location, node.merging ? TransferOutcome::Partial : TransferOutcome::Failed, currentFileError()
      };
    }
    node.source.parent = Fd{};
    node.attempted = true;
  }

  void flushMoves(Work& work) {
    using enum TransferOutcome;
    auto pending = std::exchange(work.pendingMoves, {});
    std::optional<FileError> syncError;
    std::set<dev_t> synced;
    for (const auto& move : pending) {
      try {
        if (synced.insert(statFd(move.destination.get()).st_dev).second) {
          synchronize(move.destination.get());
        }
      } catch (const std::exception&) {
        syncError = currentFileError();
        break;
      }
    }
    for (auto& move : pending) {
      auto& result = move.node->result;
      result.outcome = Partial;
      if (syncError) {
        appendError(
            result.error, syncError->kind,
            "The destination was created, but transfer cleanup is incomplete: " + syncError->message
        );
        continue;
      }
      try {
        work.check();
        const auto parent = openSourceParent(move.node->source);
        verifyTree(parent.get(), move.tree, work);
        if (move.proof) {
          move.proof->beforeSourceRemoval(work.token);
        }
        removeSource(parent.get(), move.tree, work);
        if (move.proof) {
          try {
            move.proof->sourceRemoved(work.token);
          } catch (const FileErrorException&) {
            // Optional history validation cannot undo the already confirmed source removal.
          }
        }
        result.outcome = move.outcome;
      } catch (const TransferCancelled&) {
        result.error = FileError{
            .kind = FileErrorKind::Cancelled, .message = "The copy completed; cancellation left source items in place."
        };
      } catch (const std::exception&) {
        const auto error = currentFileError();
        appendError(
            result.error, error.kind,
            "The destination was created, but transfer cleanup is incomplete: " + error.message
        );
      }
    }
  }
} // namespace luciole::transfer
