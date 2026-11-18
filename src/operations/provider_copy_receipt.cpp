#include "operations/provider_copy_receipt.h"

#include "fs/background_job.h"
#include "fs/transfer_types.h"
#include "operations/history_mount.h"
#include "operations/provider_move_receipt.h"
#include "operations/provider_transfer.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <print>
#include <utility>

namespace {
  using namespace luciole;
  namespace ph = luciole::provider_history;
  std::atomic<unsigned> pendingCleanup{0};
  std::mutex cleanupMutex;
  std::vector<FileError> cleanupErrors;
  constexpr const char* kMarker = "history-token";

  // Cleanup is best effort; request cancellation rather than holding shutdown indefinitely on a lost server.
  class CleanupDeadline {
  public:
    CleanupDeadline() : m_source(g_timeout_source_new_seconds(5)) {
      g_source_set_callback(
          m_source,
          [](gpointer data) -> gboolean {
            static_cast<CancelToken*>(data)->cancel();
            return G_SOURCE_REMOVE;
          },
          new CancelToken(token), [](gpointer data) { delete static_cast<CancelToken*>(data); }
      );
      g_source_attach(m_source, nullptr);
    }
    ~CleanupDeadline() {
      g_source_destroy(m_source);
      g_source_unref(m_source);
    }
    CleanupDeadline(const CleanupDeadline&) = delete;
    CleanupDeadline& operator=(const CleanupDeadline&) = delete;
    CancelToken token;

  private:
    GSource* m_source;
  };
  [[noreturn]] void fail(GError* raw) {
    const GErrorPtr error(raw);
    throw FileErrorException(error ? fileError(*error) : FileError{FileErrorKind::Other, "Remote history failed."});
  }
  void createDirectory(const Location& location, const CancelToken& token) {
    GError* error = nullptr;
    if (!g_file_make_directory(gioFile(location).get(), gioCancellable(token), &error)) {
      fail(error);
    }
  }
  void restrictDirectory(const Location& location, const CancelToken& token) {
    GError* error = nullptr;
    if (!g_file_set_attribute_uint32(
            gioFile(location).get(), G_FILE_ATTRIBUTE_UNIX_MODE, 0700, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            gioCancellable(token), &error
        )) {
      fail(error);
    }
    const auto value = ph::info(location, token);
    if (readIsSymlink(value.get())
        || g_file_info_get_file_type(value.get()) != G_FILE_TYPE_DIRECTORY
        || !g_file_info_has_attribute(value.get(), G_FILE_ATTRIBUTE_UNIX_MODE)
        || (g_file_info_get_attribute_uint32(value.get(), G_FILE_ATTRIBUTE_UNIX_MODE) & 0777U) != 0700U) {
      throwFileError(FileErrorKind::Unsupported, "The provider cannot create private copy history storage.");
    }
  }
  void writeMarker(const Location& location, const std::string& nonce, const CancelToken& token) {
    GError* error = nullptr;
    const GioPtr<GFileOutputStream> file(
        g_file_create(gioFile(location).get(), G_FILE_CREATE_PRIVATE, gioCancellable(token), &error)
    );
    if (!file) {
      fail(error);
    }
    gsize written = 0;
    if (!g_output_stream_write_all(
            G_OUTPUT_STREAM(file.get()), nonce.data(), nonce.size(), &written, gioCancellable(token), &error
        )) {
      fail(error);
    }
    if (!g_output_stream_close(G_OUTPUT_STREAM(file.get()), gioCancellable(token), &error)) {
      fail(error);
    }
    if (written != nonce.size()) {
      throwFileError(FileErrorKind::Other, "Remote history marker write was incomplete.");
    }
  }
  void marker(const Location& anchor, const std::string& nonce, const CancelToken& token) {
    const auto folder = ph::info(anchor, token);
    if (readIsSymlink(folder.get())
        || g_file_info_get_file_type(folder.get()) != G_FILE_TYPE_DIRECTORY
        || (g_file_info_get_attribute_uint32(folder.get(), G_FILE_ATTRIBUTE_UNIX_MODE) & 0777U) != 0700U) {
      throwFileError(FileErrorKind::Changed, "Remote history storage changed.");
    }
    const auto path = anchor.child(kMarker);
    const auto value = ph::info(path, token);
    if (readIsSymlink(value.get())
        || g_file_info_get_file_type(value.get()) != G_FILE_TYPE_REGULAR
        || g_file_info_get_size(value.get()) != static_cast<goffset>(nonce.size())) {
      throwFileError(FileErrorKind::Changed, "Remote history marker changed.");
    }
    GError* error = nullptr;
    const GioPtr<GFileInputStream> stream(g_file_read(gioFile(path).get(), gioCancellable(token), &error));
    if (!stream) {
      fail(error);
    }
    std::string actual(nonce.size() + 1, '\0');
    gsize count = 0;
    if (!g_input_stream_read_all(
            G_INPUT_STREAM(stream.get()), actual.data(), actual.size(), &count, gioCancellable(token), &error
        )) {
      fail(error);
    }
    if (!g_input_stream_close(G_INPUT_STREAM(stream.get()), gioCancellable(token), &error)) {
      fail(error);
    }
    actual.resize(count);
    if (actual != nonce) {
      throwFileError(FileErrorKind::Changed, "Remote history marker changed.");
    }
  }
  void vacant(const Location& location, const CancelToken& token) {
    if (ph::info(location, token, true)) {
      throwFileError(FileErrorKind::Exists, "The remote destination name is occupied: " + location.uri());
    }
  }
  std::string_view attribute(const ph::Tree& tree, const char* name) {
    const auto found = tree.attributes.find(name);
    return found == tree.attributes.end() ? std::string_view{} : found->second;
  }
  bool sameContents(const ph::Tree& first, const ph::Tree& second, bool compareName = true) {
    if ((compareName && first.name != second.name)
        || first.directory != second.directory
        || first.digest != second.digest
        || first.children.size() != second.children.size()
        || attribute(first, G_FILE_ATTRIBUTE_STANDARD_TYPE) != attribute(second, G_FILE_ATTRIBUTE_STANDARD_TYPE)
        || attribute(first, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)
            != attribute(second, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)) {
      return false;
    }
    for (std::size_t i = 0; i < first.children.size(); ++i) {
      if (!sameContents(first.children[i], second.children[i])) {
        return false;
      }
    }
    return true;
  }
  Location copyItem(const Location& source, const Location& parent, const CancelToken& token) {
    ProviderTransfer transfer({{source}, parent, TransferAction::Copy});
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::size_t> completed{0};
    const auto results =
        transfer.run(token, bytes, completed, [](const TransferConflict&) { return TransferConflictChoice::Cancel; });
    if (results.size() != 1 || results.front().outcome != TransferOutcome::Copied || !results.front().destination) {
      if (!results.empty() && results.front().error) {
        throw FileErrorException(*results.front().error);
      }
      throwFileError(FileErrorKind::Other, "Copy history could not publish its retained data.");
    }
    return *results.front().destination;
  }
  struct DirectoryProof {
    Location location;
    std::map<std::string, std::string> attributes;
  };
  DirectoryProof directoryProof(const Location& location, const CancelToken& token) {
    const auto value = ph::info(location, token);
    if (readIsSymlink(value.get()) || g_file_info_get_file_type(value.get()) != G_FILE_TYPE_DIRECTORY) {
      throwFileError(FileErrorKind::Changed, "A remote copy containing folder changed.");
    }
    DirectoryProof proof{location, {}};
    // Membership and modification times change through our own effects and unrelated sibling additions.
    for (const char* attribute :
         {G_FILE_ATTRIBUTE_ID_FILE, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_ATTRIBUTE_UNIX_MODE,
          G_FILE_ATTRIBUTE_UNIX_UID, G_FILE_ATTRIBUTE_UNIX_GID}) {
      if (g_file_info_has_attribute(value.get(), attribute)) {
        const luciole::GString text(g_file_info_get_attribute_as_string(value.get(), attribute));
        proof.attributes.emplace(attribute, text.get());
      }
    }
    return proof;
  }
  std::vector<DirectoryProof>
  containingFolders(const Location& destination, const std::optional<Location>& root, const CancelToken& token) {
    std::vector<DirectoryProof> result;
    for (auto current = destination.parent(); current && result.size() < 129; current = current->parent()) {
      result.push_back(directoryProof(*current, token));
      if (!root || *current == *root) {
        std::ranges::reverse(result);
        return result;
      }
    }
    throwFileError(FileErrorKind::Unsupported, "Remote copy history requires bounded containing folders.");
  }
  bool overlaps(const Location& first, const Location& second) {
    const auto a = transferLinkTarget(first, second), b = transferLinkTarget(second, first);
    return a && b && (*a == *b || a->starts_with(*b + "/") || b->starts_with(*a + "/"));
  }
  bool invalidates(const FileError& error) {
    return error.kind == FileErrorKind::Changed
        || error.kind == FileErrorKind::Missing
        || error.kind == FileErrorKind::NotDirectory
        || error.kind == FileErrorKind::Unsupported;
  }
} // namespace

struct luciole::ProviderCopyStorage {
  // Keep GVfs mount specs alive until this record's worker I/O and cleanup have finished.
  // Location is URI-only; temporary query handles alone can lose GVfs's weakly interned spec.
  GioPtr<GFile> anchorHandle, peerHandle;
  Location destination, anchor;
  std::string nonce, payloadName;
  provider_history::Tree tree;
  std::vector<DirectoryProof> parents, originParents, mergeParents;
  std::optional<Location> mergeDestination;
  std::shared_ptr<const ProviderCopyStorage> mergeOwner;
  std::optional<Location> origin, peerAnchor;
  std::string peerNonce;
  bool peerCreated = false, peerMarkerReady = false;
  std::optional<ph::Tree> original;
  bool originRemoved = false, atOrigin = false;
  std::optional<provider_history::Tree> previous;
  bool previousStored = false;
  bool local = false, localItemCreated = false, localPreviousCreated = false;
  std::optional<ph::Tree> localTree, localPrevious;
  std::optional<DirectoryProof> localItemParent, localPreviousParent;
  bool anchorAttempted = false, anchorCreated = false, markerReady = false, published = false, hidden = false,
       retain = false;
  ProviderCopyStorage(
      Location target, Location storage, std::string secret, provider_history::Tree proof,
      std::optional<provider_history::Tree> predecessor, std::vector<DirectoryProof> containing, std::string name
  )
      : destination(std::move(target)), anchor(std::move(storage)), nonce(std::move(secret)),
        payloadName(std::move(name)), tree(std::move(proof)), parents(std::move(containing)),
        previous(std::move(predecessor)) {}

  Location current() const {
    if (originRemoved) {
      return atOrigin ? *origin : destination;
    }
    return hidden ? (local ? payload(false) : anchor.child("item")) : destination;
  }
  void bindPeer(const Location& parent, const CancelToken& token) {
    const GString uuid(g_uuid_string_random()), secret(g_uuid_string_random());
    peerHandle = gioFile(parent);
    peerAnchor = parent.child(std::string(".luciole-history-") + uuid.get());
    peerNonce = secret.get();
    createDirectory(*peerAnchor, token);
    peerCreated = true;
    restrictDirectory(*peerAnchor, token);
    writeMarker(peerAnchor->child(kMarker), peerNonce, token);
    peerMarkerReady = true;
  }
  Location originalLocation() const { return origin ? *origin : mergeOwner ? mergeOwner->destination : destination; }
  std::string inspectionPaths() const {
    const auto target = mergeDestination ? *mergeDestination : destination;
    auto message = " Inspect " + originalLocation().uri() + ", " + target.uri() + " and " + anchor.uri();
    if (peerAnchor) {
      message += ", with folder binding " + peerAnchor->uri();
    }
    return message;
  }
  Location expectedLocation(bool undone) const {
    if (origin) {
      return undone ? *origin : destination;
    }
    return (mergeDestination ? !undone : undone) ? anchor.child("item") : destination;
  }
  void reverseServer(const CancelToken& token, TransferResult& result, bool& changed) {
    const auto from = previous ? destination : current();
    const auto to = previous ? anchor.child(hidden ? "previous" : "item") : hidden ? destination : anchor.child("item");
    result.source = from;
    ph::move(from, to, token);
    changed = true;
    result.destination = to;
    if (previous) {
      result.children.emplace_back(from, TransferOutcome::Moved);
      result.children.back().destination = to;
      const auto restore = anchor.child(hidden ? "item" : "previous");
      ph::verify(to, hidden ? *previous : tree, token);
      ph::verify(restore, hidden ? tree : *previous, token);
      vacant(destination, token);
      ph::move(restore, destination, token);
      result.children.emplace_back(restore, TransferOutcome::Moved);
      result.children.back().destination = destination;
      result.destination = destination;
    }
    hidden = !hidden;
    validate(token);
    result.outcome = TransferOutcome::Moved;
  }
  void validatePeerGuard(const CancelToken& token) const {
    if (!peerAnchor) {
      return;
    }
    if (!peerCreated) {
      if (ph::info(*peerAnchor, token, true)) {
        throwFileError(FileErrorKind::Changed, "History folder binding creation was not confirmed.");
      }
      return;
    }
    if (peerMarkerReady) {
      marker(*peerAnchor, peerNonce, token);
    }
    const auto expected = peerMarkerReady ? std::vector<std::string>{kMarker} : std::vector<std::string>{};
    if (ph::children(*peerAnchor, token, 1) != expected) {
      throwFileError(FileErrorKind::Changed, "A history folder binding contains unrecorded data.");
    }
  }
  void cleanupPeerGuard(const CancelToken& token) const {
    if (!peerCreated) {
      return;
    }
    validatePeerGuard(token);
    if (peerMarkerReady) {
      ph::erase(peerAnchor->child(kMarker), token);
    }
    ph::erase(*peerAnchor, token);
  }
  void validateOriginParents(const CancelToken& token) const {
    validatePeerGuard(token);
    for (const auto& parent : originParents) {
      if (directoryProof(parent.location, token).attributes != parent.attributes) {
        throwFileError(FileErrorKind::Changed, "A moved item's original containing folder changed.");
      }
    }
  }
  void validateMove(const CancelToken& token) const {
    validateOriginParents(token);
    ph::verify(current(), tree, token);
    if (previous) {
      ph::verify(atOrigin ? destination : anchor.child("previous"), *previous, token);
      vacant(atOrigin ? anchor.child("previous") : *origin, token);
    } else {
      vacant(atOrigin ? destination : *origin, token);
    }
  }
  void reverseMove(const CancelToken& token, TransferResult& result, bool& changed) {
    const auto returnTo = atOrigin ? destination : *origin;
    result.source = current();
    if (atOrigin && previous) {
      ph::move(destination, anchor.child("previous"), token);
      changed = true;
      result.children.emplace_back(destination, TransferOutcome::Moved);
      result.children.back().destination = anchor.child("previous");
    }
    vacant(returnTo, token);
    ph::move(current(), returnTo, token);
    changed = true;
    result.children.emplace_back(result.source, TransferOutcome::Moved);
    result.children.back().destination = returnTo;
    if (!atOrigin && previous) {
      ph::verify(anchor.child("previous"), *previous, token);
      vacant(destination, token);
      ph::move(anchor.child("previous"), destination, token);
      result.children.emplace_back(anchor.child("previous"), TransferOutcome::Moved);
      result.children.back().destination = destination;
    }
    atOrigin = !atOrigin;
    validate(token);
    result.destination = returnTo;
    result.outcome = TransferOutcome::Moved;
  }
  Location payload(bool predecessor) const {
    return anchor.child(predecessor ? "previous" : "item").child(payloadName);
  }
  void storeLocal(const Location& source, const ph::Tree& proof, bool predecessor, const CancelToken& token) {
    const auto container = anchor.child(predecessor ? "previous" : "item");
    createDirectory(container, token);
    (predecessor ? localPreviousCreated : localItemCreated) = true;
    (predecessor ? localPreviousParent : localItemParent) = directoryProof(container, token);
    const auto copied = copyItem(source, container, token), target = payload(predecessor);
    if (copied != target) {
      ph::move(copied, target, token);
    }
    ph::Budget budget;
    auto retained = ph::capture(target, budget, token);
    if (!sameContents(proof, retained)) {
      throwFileError(FileErrorKind::Changed, "Retained copy contents differ from the captured provider data.");
    }
    ph::verify(source, proof, token);
    (predecessor ? localPrevious : localTree) = std::move(retained);
  }
  void validateStored(bool predecessor, const CancelToken& token) const {
    const auto& parent = predecessor ? localPreviousParent : localItemParent;
    const auto& proof = predecessor ? localPrevious : localTree;
    if (!parent || directoryProof(parent->location, token).attributes != parent->attributes) {
      throwFileError(FileErrorKind::Changed, "A local copy history containing folder changed.");
    }
    const auto names = ph::children(parent->location, token, 1);
    if (!proof) {
      if (!names.empty()) {
        throwFileError(FileErrorKind::Changed, "Unverified local history data was preserved.");
      }
      return;
    }
    if (names != std::vector<std::string>{payloadName}) {
      throwFileError(FileErrorKind::Changed, "Local copy history contains unknown entries.");
    }
    ph::verify(payload(predecessor), *proof, token);
  }
  void validateLocal(const CancelToken& token) const {
    if (!localTree || (previous && !localPrevious)) {
      throwFileError(FileErrorKind::Changed, "Local copy history is incomplete.");
    }
    validateStored(false, token);
    if (previous) {
      validateStored(true, token);
    }
    if (!hidden) {
      ph::verify(destination, tree, token);
    } else if (previous) {
      ph::verify(destination, *previous, token);
    } else {
      vacant(destination, token);
    }
  }
  void reverseLocal(const CancelToken& token, TransferResult& result, bool& changed) {
    result.source = destination;
    if (!hidden || previous) {
      ph::remove(destination, hidden ? *previous : tree, token);
      changed = true;
      result.children.emplace_back(destination, TransferOutcome::Moved);
      result.children.back().destination = payload(hidden);
    }
    if (hidden || previous) {
      const auto source = payload(!hidden);
      vacant(destination, token);
      const auto copied = copyItem(source, *destination.parent(), token);
      changed = true;
      result.children.emplace_back(source, TransferOutcome::Copied);
      result.children.back().destination = copied;
      if (!transferLocationsEqual(copied, destination)) {
        throwFileError(FileErrorKind::Changed, "Copy history published at an unexpected name.");
      }
      ph::Budget budget;
      auto restored = ph::capture(destination, budget, token);
      if (!sameContents(restored, hidden ? *localTree : *localPrevious)) {
        throwFileError(FileErrorKind::Changed, "The restored provider data changed.");
      }
      (hidden ? tree : *previous) = std::move(restored);
    }
    hidden = !hidden;
    validate(token);
    result.destination = hidden && !previous ? payload(false) : destination;
    result.outcome = TransferOutcome::Moved;
  }
  void cleanupLocal(const CancelToken& token) const {
    // Check both versions before discarding either one.
    for (bool predecessor : {false, true}) {
      if (predecessor ? localPreviousCreated : localItemCreated) {
        validateStored(predecessor, token);
      }
    }
    for (bool predecessor : {false, true}) {
      if (!(predecessor ? localPreviousCreated : localItemCreated)) {
        continue;
      }
      const auto& proof = predecessor ? localPrevious : localTree;
      if (proof) {
        ph::remove(payload(predecessor), *proof, token);
      }
      ph::erase(anchor.child(predecessor ? "previous" : "item"), token);
    }
  }
  std::vector<std::string> storageNames() const {
    if (originRemoved) {
      return !atOrigin && previousStored ? std::vector<std::string>{kMarker, "previous"}
                                         : std::vector<std::string>{kMarker};
    }
    if (local) {
      std::vector<std::string> names{kMarker};
      if (localItemCreated) {
        names.emplace_back("item");
      }
      if (localPreviousCreated) {
        names.emplace_back("previous");
      }
      return names;
    }
    if (hidden) {
      return {kMarker, "item"};
    }
    return previousStored ? std::vector<std::string>{kMarker, "previous"} : std::vector<std::string>{kMarker};
  }
  void validateParents(const CancelToken& token) const {
    for (const auto& parent : parents) {
      if (directoryProof(parent.location, token).attributes != parent.attributes) {
        throwFileError(FileErrorKind::Changed, "A remote copy containing folder changed.");
      }
    }
  }
  void validate(const CancelToken& token) const {
    validateParents(token);
    if (mergeDestination) {
      validatePeerGuard(token);
      for (const auto& parent : mergeParents) {
        if (directoryProof(parent.location, token).attributes != parent.attributes) {
          throwFileError(FileErrorKind::Changed, "A merge destination containing folder changed.");
        }
      }
    }
    marker(anchor, nonce, token);
    const auto names = ph::children(anchor, token, local ? 3 : 2);
    if (names != storageNames() || (previous && !previousStored)) {
      throwFileError(FileErrorKind::Changed, "Remote history contains unknown entries.");
    }
    if (originRemoved) {
      validateMove(token);
      return;
    }
    if (local) {
      validateLocal(token);
      return;
    }
    ph::verify(current(), tree, token);
    if (previous) {
      ph::verify(hidden ? destination : anchor.child("previous"), *previous, token);
      vacant(anchor.child(hidden ? "previous" : "item"), token);
    } else {
      vacant(hidden ? destination : anchor.child("item"), token);
    }
  }
  void cleanup() const {
    if (!anchorCreated) {
      if (anchorAttempted) {
        CleanupDeadline deadline;
        if (!local) {
          validateParents(deadline.token);
        }
        if (ph::info(anchor, deadline.token, true)) {
          throwFileError(FileErrorKind::Other, "Remote history storage creation was not confirmed.");
        }
      }
      return;
    }
    if (retain) {
      throwFileError(FileErrorKind::Other, "Incomplete remote reversal retained data for inspection.");
    }
    CleanupDeadline deadline;
    validatePeerGuard(deadline.token);
    if (!local) {
      validateParents(deadline.token);
    }
    if (!markerReady) {
      if (!ph::children(anchor, deadline.token, 1).empty()) {
        throwFileError(FileErrorKind::Changed, "Unverified data remains in remote history storage.");
      }
    } else {
      marker(anchor, nonce, deadline.token);
      if (ph::children(anchor, deadline.token, local ? 3 : 2) != storageNames()) {
        throwFileError(FileErrorKind::Changed, "Unrecorded remote history data was preserved.");
      }
      if (local) {
        cleanupLocal(deadline.token);
      } else if ((originRemoved && !atOrigin && previousStored) || (!originRemoved && (hidden || previousStored))) {
        const auto location = anchor.child(hidden ? "item" : "previous");
        const auto& proof = hidden ? tree : *previous;
        ph::verify(location, proof, deadline.token);
        ph::remove(location, proof, deadline.token);
      }
      marker(anchor, nonce, deadline.token);
      ph::erase(anchor.child(kMarker), deadline.token);
    }
    ph::erase(anchor, deadline.token);
    cleanupPeerGuard(deadline.token);
  }
};

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepareWithStorage(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
    bool replacement, const std::optional<Location>& historyRoot, bool local
) {
  auto parents = containingFolders(destination, historyRoot, token);
  auto tree = provider_history::capture(staged, budget, token);
  auto previous = replacement ? std::optional(provider_history::capture(destination, budget, token)) : std::nullopt;
  const auto parent = destination.parent();
  const auto name = destination.basename();
  if (!parent || !name) {
    throwFileError(FileErrorKind::Unsupported, "Remote copy history requires a containing folder.");
  }
  const GString uuid(g_uuid_string_random()), nonce(g_uuid_string_random());
  const auto storageParent = local ? Location::fromPath(g_get_tmp_dir()) : *parent;
  auto storage = std::shared_ptr<ProviderCopyStorage>(
      new ProviderCopyStorage(
          destination, storageParent.child(std::string(".luciole-history-") + uuid.get()), nonce.get(), std::move(tree),
          std::move(previous), std::move(parents), *name
      ),
      [](ProviderCopyStorage* raw) {
        auto owned = std::unique_ptr<ProviderCopyStorage>(raw);
        ++pendingCleanup;
        detail::runInThread(
            [owned = std::move(owned)] {
              try {
                owned->cleanup();
              } catch (...) {
                auto error = currentFileError();
                error.message += " Inspect remote history at " + owned->anchor.uri();
                if (owned->peerAnchor) {
                  error.message += " and " + owned->peerAnchor->uri();
                }
                std::println(stderr, "[file-history] {}", error.message);
                const std::scoped_lock lock(cleanupMutex);
                if (cleanupErrors.size() == 12) {
                  cleanupErrors.erase(cleanupErrors.begin());
                }
                cleanupErrors.push_back(std::move(error));
              }
            },
            [] { --pendingCleanup; }
        );
      }
  );
  storage->local = local;
  if (!local) {
    storage->anchorHandle = gioFile(storage->anchor);
  }
  storage->anchorAttempted = true;
  createDirectory(storage->anchor, token);
  storage->anchorCreated = true;
  restrictDirectory(storage->anchor, token);
  writeMarker(storage->anchor.child(kMarker), storage->nonce, token);
  storage->markerReady = true;
  if (local) {
    storage->storeLocal(staged, storage->tree, false, token);
    if (storage->previous) {
      storage->storeLocal(destination, *storage->previous, true, token);
      storage->previousStored = true;
    }
  }
  return std::shared_ptr<const ProviderCopyEffect>(new ProviderCopyEffect(std::move(storage)));
}

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepare(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
    bool replacement, const std::optional<Location>& historyRoot
) {
  const bool local = destination.uri().starts_with("smb:")
      || destination.uri().starts_with("dav:")
      || destination.uri().starts_with("davs:");
  if (local) {
    return prepareLocal(destination, staged, budget, token, replacement, historyRoot);
  }
  return prepareWithStorage(destination, staged, budget, token, replacement, historyRoot, false);
}

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepareLocal(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
    bool replacement, const std::optional<Location>& historyRoot
) {
  return prepareWithStorage(destination, staged, budget, token, replacement, historyRoot, true);
}

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepareMove(
    const Location& source, const Location& destination, const Location& staged, provider_history::Budget& budget,
    const CancelToken& token, bool replacement, const std::optional<Location>& historyRoot
) {
  if (!transferLinkTarget(source, destination)
      || (!source.isNative() && !source.uri().starts_with("sftp:"))
      || (!destination.isNative() && !destination.uri().starts_with("sftp:"))) {
    throwFileError(FileErrorKind::Unsupported, "Move history requires one local or SFTP namespace.");
  }
  auto parents = containingFolders(source, source.resolve("/"), token);
  auto original = ph::capture(source, budget, token);
  auto effect = prepare(destination, staged, budget, token, replacement, historyRoot);
  auto& storage = *effect->m_storage;
  if (storage.local || !sameContents(original, storage.tree)) {
    throwFileError(FileErrorKind::Changed, "The moved source differs from the staged data.");
  }
  storage.origin = source;
  storage.originParents = std::move(parents);
  storage.original = std::move(original);
  storage.bindPeer(*source.parent(), token);
  return effect;
}

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepareMergeSource(
    const Location& source, const Location& destination, provider_history::Budget& budget, const CancelToken& token,
    const std::optional<Location>& historyRoot
) {
  if (!transferLinkTarget(source, destination) || overlaps(source, destination)) {
    throwFileError(FileErrorKind::Unsupported, "A Move merge requires independent paths in one server namespace.");
  }
  auto effect = prepare(source, source, budget, token, false, source.parent());
  auto& storage = *effect->m_storage;
  if (!storage.tree.directory || storage.local) {
    throwFileError(FileErrorKind::Unsupported, "Move merge history requires an original folder.");
  }
  storage.mergeDestination = destination;
  storage.mergeParents = containingFolders(destination, historyRoot, token);
  storage.mergeParents.push_back(directoryProof(destination, token));
  storage.bindPeer(destination, token);
  effect->published(token);
  return effect;
}

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepareMergeCopy(
    const std::shared_ptr<const ProviderCopyEffect>& owner, const Location& source, const Location& destination,
    const Location& staged, provider_history::Budget& budget, const CancelToken& token, bool replacement,
    const std::optional<Location>& historyRoot
) {
  if (!owner || !owner->m_storage->mergeDestination || owner->m_storage->hidden) {
    throwFileError(FileErrorKind::Changed, "The Move merge's original source is unavailable.");
  }
  const auto& original = *owner->m_storage;
  const GString relative(g_file_get_relative_path(gioFile(original.destination).get(), gioFile(source).get()));
  if (!relative || !*relative.get()) {
    throwFileError(FileErrorKind::Changed, "The copied child is outside the Move merge source.");
  }
  const ph::Tree* subtree = &original.tree;
  std::string_view remaining(relative.get());
  while (!remaining.empty()) {
    const auto slash = remaining.find('/');
    const auto name = remaining.substr(0, slash);
    const auto found = std::ranges::find(subtree->children, name, &ph::Tree::name);
    if (found == subtree->children.end()) {
      throwFileError(FileErrorKind::Changed, "The copied child changed since merge preparation.");
    }
    subtree = &*found;
    remaining = slash == std::string_view::npos ? std::string_view{} : remaining.substr(slash + 1);
  }
  auto effect = prepare(destination, staged, budget, token, replacement, historyRoot);
  if (!sameContents(*subtree, effect->m_storage->tree, false)) {
    throwFileError(FileErrorKind::Changed, "Copied contents differ from the retained merge source.");
  }
  effect->m_storage->mergeOwner = owner->m_storage;
  return effect;
}

void luciole::ProviderCopyEffect::retireMergedSource(const CancelToken& token) const {
  auto& storage = *m_storage;
  if (!storage.mergeDestination || !storage.published || storage.hidden) {
    throwFileError(FileErrorKind::Changed, "The original merge source cannot be retained.");
  }
  storage.validate(token);
  ph::check(token);
  // Preserve an unconfirmed rename even if the server committed before its reply was lost.
  storage.retain = true;
  try {
    ph::move(storage.destination, storage.anchor.child("item"), token);
    storage.hidden = true;
    storage.validate(token);
    storage.retain = false;
  } catch (...) {
    auto error = currentFileError();
    error.message += storage.inspectionPaths();
    throw FileErrorException(std::move(error));
  }
}

void luciole::ProviderCopyEffect::beforeSourceRemoval(const CancelToken& token) const {
  auto& storage = *m_storage;
  if (!storage.origin || !storage.original) {
    throwFileError(FileErrorKind::Changed, "The original Move proof is unavailable.");
  }
  storage.validateOriginParents(token);
  storage.validate(token);
  ph::verify(*storage.origin, *storage.original, token);
}
void luciole::ProviderCopyEffect::sourceRemoved(const CancelToken& token) const {
  m_storage->originRemoved = true;
  m_storage->original.reset();
  m_storage->validate(token);
}

void luciole::ProviderCopyEffect::published(const CancelToken& token, const std::optional<Location>& previous) const {
  if (m_storage->previous) {
    if (!previous) {
      throwFileError(FileErrorKind::Changed, "The overwritten destination was not retained.");
    }
    ph::verify(*previous, *m_storage->previous, token);
    if (!m_storage->local) {
      // An accepted rename can lose its reply. Preserve the anchor after any unconfirmed retention attempt.
      m_storage->retain = true;
      ph::move(*previous, m_storage->anchor.child("previous"), token);
      m_storage->previousStored = true;
    }
  }
  m_storage->validate(token);
  m_storage->published = true;
  m_storage->retain = false;
}

void luciole::ProviderCopyEffect::preserveInactive() const {
  m_storage->retain |= m_storage->hidden || m_storage->previousStored;
}

struct luciole::ProviderCopyReceipt::State {
  std::mutex mutex;
  std::vector<std::shared_ptr<const ProviderCopyEffect>> effects;
  std::uint64_t generation = 0;
  TransferAction action = TransferAction::Copy;
};

bool luciole::ProviderCopyReceipt::usesMount(const HistoryMount& mount) const {
  return std::ranges::any_of(m_state->effects, [&](const auto& effect) {
    return mount.contains(effect->m_storage->destination) || mount.contains(effect->m_storage->anchor);
  });
}
luciole::ProviderCopyReceipt::ProviderCopyReceipt(std::shared_ptr<State> state, std::uint64_t generation)
    : CopyReceipt(state->action), m_state(std::move(state)), m_generation(generation) {}

std::shared_ptr<const luciole::CopyReceipt> luciole::ProviderCopyReceipt::capture(
    const std::vector<std::shared_ptr<const ProviderCopyEffect>>& effects, const CancelToken& token,
    TransferAction action
) {
  if (effects.empty() || effects.size() > 128 || action == TransferAction::Move) {
    return {};
  }
  auto state = std::make_shared<State>();
  state->action = action;
  std::vector<Location> destinations;
  destinations.reserve(effects.size());
  try {
    for (const auto& effect : effects) {
      const auto& destination = effect->m_storage->destination;
      if (!effect->m_storage->published
          || effect->m_storage->origin
          || effect->m_storage->mergeDestination
          || effect->m_storage->mergeOwner
          || std::ranges::any_of(destinations, [&](const Location& earlier) {
               return transferLocationsEqual(destination, earlier)
                   || transferLocationInside(destination, earlier)
                   || transferLocationInside(earlier, destination);
             })) {
        throwFileError(FileErrorKind::Changed, "The remote copy selection cannot supply history.");
      }
      effect->m_storage->validate(token);
      destinations.push_back(destination);
    }
  } catch (...) {
    for (const auto& effect : effects) {
      effect->m_storage->retain |= effect->m_storage->hidden || effect->m_storage->previousStored;
    }
    throw;
  }
  state->effects = effects;
  return std::shared_ptr<const CopyReceipt>(new ProviderCopyReceipt(std::move(state), 0));
}

std::size_t luciole::ProviderCopyReceipt::size() const { return m_state->effects.size(); }

std::vector<luciole::TransferResult> luciole::ProviderCopyReceipt::failureResults(const FileError& error) const {
  std::vector<TransferResult> results;
  for (const auto& effect : m_state->effects) {
    auto value = error;
    value.message += " Inspect " + effect->m_storage->destination.uri() + " and " + effect->m_storage->anchor.uri();
    results.emplace_back(effect->m_storage->destination, TransferOutcome::Failed, std::move(value));
  }
  return results;
}

std::vector<luciole::TransferResult> luciole::ProviderCopyReceipt::unconfirmed(const FileError& error) const {
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  if (lock.owns_lock() && m_generation == m_state->generation) {
    for (const auto& effect : m_state->effects) {
      effect->m_storage->retain |=
          effect->m_storage->local || effect->m_storage->hidden || effect->m_storage->previousStored;
    }
  }
  return failureResults(error);
}

luciole::CopyReplayResult
luciole::ProviderCopyReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  CopyReplayResult reply;
  for (const auto& effect : m_state->effects) {
    reply.results.emplace_back(effect->m_storage->destination, TransferOutcome::Skipped);
  }
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  try {
    if (!lock.owns_lock()) {
      throwFileError(FileErrorKind::Other, "Another remote reversal is using this history.");
    }
    if (m_generation != m_state->generation) {
      throwFileError(FileErrorKind::Changed, "Remote copy history is stale.");
    }
    for (const auto& effect : m_state->effects) {
      effect->m_storage->validate(token);
    }
  } catch (...) {
    const auto error = currentFileError();
    reply.results = failureResults(error);
    if (error.kind == FileErrorKind::Cancelled) {
      for (auto& result : reply.results) {
        result.outcome = TransferOutcome::Cancelled;
      }
    }
    reply.retireHistory = invalidates(error);
    if (reply.retireHistory && lock.owns_lock() && m_generation == m_state->generation) {
      for (const auto& effect : m_state->effects) {
        effect->m_storage->retain |= effect->m_storage->hidden || effect->m_storage->previousStored;
      }
    }
    completed.store(size());
    return reply;
  }
  bool stopped = false, applied = false;
  for (std::size_t i = 0; i < size(); ++i) {
    auto& item = *m_state->effects[i]->m_storage;
    auto& result = reply.results[i];
    result.source = item.previous ? item.destination : item.current();
    if (stopped) {
      result.error = FileError{FileErrorKind::Other, "Not attempted after remote reversal stopped."};
      completed.fetch_add(1);
      continue;
    }
    bool attempted = false, moved = false;
    try {
      item.validate(token);
      provider_history::check(token);
      attempted = true;
      if (item.local) {
        item.reverseLocal(token, result, moved);
        applied |= moved;
      } else {
        item.reverseServer(token, result, moved);
        applied |= moved;
      }
    } catch (...) {
      applied |= moved;
      auto error = currentFileError();
      bool uncertain = attempted && !moved;
      if (uncertain) {
        // A failed provider reply can follow a committed mutation. Do not assume zero effects.
        try {
          CleanupDeadline deadline;
          // Prove the entire old state, including both versions for replacement replay.
          item.validate(deadline.token);
          uncertain = false;
        } catch (...) {
        }
      }
      reply.retireHistory = applied || uncertain || invalidates(error);
      if (reply.retireHistory) {
        ++m_state->generation;
        for (const auto& effect : m_state->effects) {
          effect->m_storage->retain |= effect->m_storage->hidden || effect->m_storage->previousStored || uncertain;
        }
      }
      error.message += " Inspect " + item.destination.uri() + " and " + item.anchor.uri();
      result.error = std::move(error);
      result.outcome = moved || uncertain ? TransferOutcome::Partial
          : token.cancelled()             ? TransferOutcome::Cancelled
                                          : TransferOutcome::Failed;
      stopped = true;
    }
    completed.fetch_add(1);
  }
  if (!stopped) {
    ++m_state->generation;
    reply.receipt = std::shared_ptr<const CopyReceipt>(new ProviderCopyReceipt(m_state, m_state->generation));
  }
  return reply;
}

bool luciole::ProviderCopyReceipt::cleanupPending() { return pendingCleanup.load() != 0; }
std::vector<luciole::FileError> luciole::ProviderCopyReceipt::takeCleanupErrors() {
  const std::scoped_lock lock(cleanupMutex);
  return std::exchange(cleanupErrors, {});
}

struct luciole::ProviderMoveReceipt::State {
  std::mutex mutex;
  std::vector<std::shared_ptr<const ProviderCopyEffect>> effects;
  std::uint64_t generation = 0;
};

bool luciole::ProviderMoveReceipt::usesMount(const HistoryMount& mount) const {
  return std::ranges::any_of(m_state->effects, [&](const auto& effect) {
    const auto& item = *effect->m_storage;
    return mount.contains(item.destination)
        || mount.contains(item.anchor)
        || (item.origin && mount.contains(*item.origin))
        || (item.peerAnchor && mount.contains(*item.peerAnchor))
        || (item.mergeDestination && mount.contains(*item.mergeDestination));
  });
}
luciole::ProviderMoveReceipt::ProviderMoveReceipt(std::shared_ptr<State> state, std::uint64_t generation, bool atOrigin)
    : m_state(std::move(state)), m_generation(generation), m_atOrigin(atOrigin) {}

std::shared_ptr<const luciole::MoveReceipt> luciole::ProviderMoveReceipt::capture(
    const std::vector<std::shared_ptr<const ProviderCopyEffect>>& effects, const CancelToken& token
) {
  if (effects.empty() || effects.size() > 128) {
    return {};
  }
  std::vector<Location> paths;
  paths.reserve(effects.size() * 2);
  try {
    for (const auto& effect : effects) {
      const auto& item = *effect->m_storage;
      const bool moved = item.origin && item.originRemoved && !item.atOrigin;
      const bool mergedSource = item.mergeDestination && item.hidden && !item.origin;
      const bool mergedCopy = item.mergeOwner && !item.hidden && !item.origin;
      if (!item.published || (!moved && !mergedSource && !mergedCopy)) {
        throwFileError(FileErrorKind::Changed, "The remote Move proof is incomplete.");
      }
      if (mergedCopy && !std::ranges::any_of(effects, [&](const auto& owner) {
            return owner->m_storage == item.mergeOwner && owner->m_storage->mergeDestination.has_value();
          })) {
        throwFileError(FileErrorKind::Changed, "The Move merge source proof is unavailable.");
      }
      const auto original = moved ? *item.origin : item.destination;
      if (moved && overlaps(original, item.destination)) {
        throwFileError(FileErrorKind::Unsupported, "A remote Move overlaps itself.");
      }
      for (const auto& path : paths) {
        if (overlaps(path, original) || (moved && overlaps(path, item.destination))) {
          throwFileError(FileErrorKind::Unsupported, "Overlapping remote Move effects cannot supply history.");
        }
      }
      item.validate(token);
      paths.push_back(original);
      if (moved) {
        paths.push_back(item.destination);
      }
    }
  } catch (...) {
    for (const auto& effect : effects) {
      effect->preserveInactive();
    }
    throw;
  }
  auto state = std::make_shared<State>();
  state->effects = effects;
  // Source trees are restored/retained before their destination copies change in either direction.
  std::stable_partition(state->effects.begin(), state->effects.end(), [](const auto& effect) {
    return effect->m_storage->mergeDestination.has_value();
  });
  return std::shared_ptr<const MoveReceipt>(new ProviderMoveReceipt(std::move(state), 0, false));
}

std::size_t luciole::ProviderMoveReceipt::size() const { return m_state->effects.size(); }
std::vector<luciole::TransferResult> luciole::ProviderMoveReceipt::failureResults(const FileError& error) const {
  std::vector<TransferResult> results;
  results.reserve(size());
  for (const auto& effect : m_state->effects) {
    auto detail = error;
    detail.message += effect->m_storage->inspectionPaths();
    results.emplace_back(effect->m_storage->expectedLocation(m_atOrigin), TransferOutcome::Failed, std::move(detail));
  }
  return results;
}
std::vector<luciole::TransferResult> luciole::ProviderMoveReceipt::unconfirmed(const FileError& error) const {
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  if (lock.owns_lock() && m_generation == m_state->generation) {
    ++m_state->generation;
    for (const auto& effect : m_state->effects) {
      effect->m_storage->retain = true;
    }
  }
  auto results = failureResults(error);
  for (std::size_t i = 0; i < size(); ++i) {
    results[i].outcome = TransferOutcome::Partial;
  }
  return results;
}

luciole::MoveReplayResult
luciole::ProviderMoveReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  MoveReplayResult reply;
  reply.results.reserve(size());
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  try {
    if (!lock.owns_lock()) {
      throwFileError(FileErrorKind::Other, "Another remote reversal is using this Move history.");
    }
    if (m_generation != m_state->generation) {
      throwFileError(FileErrorKind::Changed, "Remote Move history is stale.");
    }
    for (const auto& effect : m_state->effects) {
      effect->m_storage->validate(token);
    }
  } catch (...) {
    const auto error = currentFileError();
    reply.results = failureResults(error);
    if (error.kind == FileErrorKind::Cancelled) {
      for (auto& result : reply.results) {
        result.outcome = TransferOutcome::Cancelled;
      }
    }
    reply.retireHistory = invalidates(error);
    if (reply.retireHistory && lock.owns_lock() && m_generation == m_state->generation) {
      ++m_state->generation;
      for (const auto& effect : m_state->effects) {
        effect->m_storage->retain = true;
      }
    }
    completed.store(size());
    return reply;
  }
  bool stopped = false, applied = false;
  for (const auto& effect : m_state->effects) {
    auto& item = *effect->m_storage;
    reply.results.emplace_back(item.current(), TransferOutcome::Skipped);
    auto& result = reply.results.back();
    if (stopped) {
      result.error = FileError{FileErrorKind::Other, "Not attempted after remote Move reversal stopped."};
      completed.fetch_add(1);
      continue;
    }
    bool attempted = false, moved = false;
    try {
      item.validate(token);
      ph::check(token);
      attempted = true;
      if (item.origin) {
        item.reverseMove(token, result, moved);
      } else {
        item.reverseServer(token, result, moved);
      }
      applied |= moved;
    } catch (...) {
      applied |= moved;
      auto error = currentFileError();
      bool uncertain = attempted && !moved;
      if (uncertain) {
        try {
          CleanupDeadline deadline;
          item.validate(deadline.token);
          uncertain = false;
        } catch (...) {
        }
      }
      reply.retireHistory = applied || uncertain || invalidates(error);
      if (reply.retireHistory) {
        ++m_state->generation;
        for (const auto& other : m_state->effects) {
          other->m_storage->retain = true;
        }
      }
      error.message += item.inspectionPaths();
      result.error = std::move(error);
      result.outcome = moved || uncertain ? TransferOutcome::Partial
          : token.cancelled()             ? TransferOutcome::Cancelled
                                          : TransferOutcome::Failed;
      stopped = true;
    }
    completed.fetch_add(1);
  }
  if (!stopped) {
    ++m_state->generation;
    reply.receipt =
        std::shared_ptr<const MoveReceipt>(new ProviderMoveReceipt(m_state, m_state->generation, !m_atOrigin));
  }
  return reply;
}
