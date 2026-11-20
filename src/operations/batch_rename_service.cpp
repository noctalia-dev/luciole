#include "operations/batch_rename_service.h"

#include "fs/background_job.h"
#include "fs/cancel_token.h"
#include "fs/filename.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "operations/history_mount.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <map>
#include <ranges>
#include <regex>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  using luciole::FileErrorKind;
  std::string fdPath(int fd) {
    std::vector<char> buffer(8192);
    const auto count = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buffer.data(), buffer.size());
    if (count <= 0 || static_cast<std::size_t>(count) == buffer.size()) {
      luciole::throwFileError(FileErrorKind::Unavailable, "The held folder's current path is unavailable.");
    }
    return {buffer.data(), static_cast<std::size_t>(count)};
  }
  struct Item {
    luciole::DirectoryEntry entry;
    std::string name;
    luciole::Fd fd;
    struct stat identity;
    // Replay copies share the current revision among hard-link aliases.
    std::shared_ptr<struct stat> revision;
    Item(luciole::DirectoryEntry entry, int parent, std::string name)
        : entry(std::move(entry)), name(std::move(name)),
          fd(luciole::checkedFd(::openat(parent, this->name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC))),
          identity(luciole::statFd(fd.get())) {}
    bool at(int parent, const std::string& name) const {
      struct stat value{};
      return ::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0
          && luciole::sameIdentity(value, identity);
    }
    bool current(int parent, const std::string& name) const {
      if (!revision) {
        return at(parent, name);
      }
      struct stat value{};
      struct stat held{};
      return ::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0
          && ::fstat(fd.get(), &held) == 0
          && luciole::sameFileRevision(value, *revision)
          && luciole::sameFileRevision(held, *revision);
    }
  };
  struct Prepared {
    luciole::Location parent;
    std::string path;
    luciole::Fd fd;
    std::vector<std::unique_ptr<Item>> items;
    long nameLimit;
    explicit Prepared(luciole::Location parent)
        : parent(std::move(parent)), path(*this->parent.nativePath()),
          fd(luciole::checkedFd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))),
          nameLimit(::fpathconf(fd.get(), _PC_NAME_MAX)) {}
    void requireCurrent() const {
      const auto current = luciole::checkedFd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!luciole::sameIdentity(luciole::statFd(current.get()), luciole::statFd(fd.get()))) {
        luciole::throwFileError(
            FileErrorKind::Changed, "The containing folder changed. Close this dialog and select the items again."
        );
      }
    }
    std::optional<luciole::Location>
    actual(int directory, const std::string& name, const Item& item, bool originalFolder) const {
      if (!item.at(directory, name)) {
        return std::nullopt;
      }
      try {
        std::string base;
        if (originalFolder) {
          try {
            requireCurrent();
            base = path;
          } catch (const std::exception&) {
            base = fdPath(directory);
          }
        } else {
          base = fdPath(directory);
        }
        return luciole::Location::fromPath((std::filesystem::path(base) / name).native());
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }
  };
  std::shared_ptr<Prepared>
  prepareItems(std::vector<luciole::DirectoryEntry> entries, const luciole::CancelToken& token) {
    const auto first = entries.front().location().parent();
    if (!first) {
      luciole::throwFileError(FileErrorKind::InvalidArgument, "Select entries inside a native folder.");
    }
    if (!first->nativePath()) {
      luciole::throwFileError(FileErrorKind::Unsupported, "Batch rename requires a native folder.");
    }
    auto prepared = std::make_shared<Prepared>(*first);
    std::set<std::string> names;
    for (auto& entry : entries) {
      if (token.cancelled()) {
        return {};
      }
      const auto location = entry.location();
      if (!location.nativePath() || !entry.identity() || location.parent() != first) {
        luciole::throwFileError(FileErrorKind::InvalidArgument, "Select identified items from one native folder.");
      }
      const std::string name = location.basename().value_or(std::string{});
      if (name.empty() || name == "." || name == ".." || name != entry.name || !names.insert(name).second) {
        luciole::throwFileError(FileErrorKind::InvalidArgument, "The selection contains invalid or repeated names.");
      }
      auto item = std::make_unique<Item>(entry, prepared->fd.get(), name);
      const luciole::GioPtr<GFileInfo> info(g_file_query_info(
          luciole::gioFile(location).get(),
          "id::file,id::filesystem,standard::type,standard::is-symlink,access::can-rename",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
      ));
      if (!info
          || luciole::readFileIdentity(info.get()) != entry.identity()
          || luciole::fileType(g_file_info_get_file_type(info.get())) != entry.type
          || (g_file_info_get_is_symlink(info.get()) != FALSE) != entry.symlink
          || !item->at(prepared->fd.get(), name)
          || !luciole::readFileAccess(info.get()).rename.value_or(true)) {
        luciole::throwFileError(
            FileErrorKind::Changed, "A selected item changed or cannot be renamed. Select it again."
        );
      }
      prepared->items.push_back(std::move(item));
    }
    prepared->requireCurrent();
    return prepared;
  }
  std::shared_ptr<std::vector<luciole::BatchRenameRow>> validate(
      const Prepared& plan, const std::vector<std::string>& names, const luciole::CancelToken& token,
      bool capturedNames = false
  ) {
    auto rows = std::make_shared<std::vector<luciole::BatchRenameRow>>();
    std::map<std::string, std::size_t> sources, targets;
    for (std::size_t i = 0; i < names.size(); ++i) {
      sources.emplace(plan.items[i]->name, i);
      rows->push_back(
          {plan.items[i]->entry.displayName(),
           luciole::filenameDisplayText(names[i]),
           names[i],
           {},
           names[i] != plan.items[i]->name}
      );
    }
    plan.requireCurrent();
    const bool writable = ::faccessat(plan.fd.get(), ".", W_OK | X_OK, AT_EACCESS) == 0;
    for (std::size_t i = 0; i < names.size() && !token.cancelled(); ++i) {
      auto& row = (*rows)[i];
      if (!plan.items[i]->current(plan.fd.get(), plan.items[i]->name)) {
        row.error = "Source changed. Close and select it again.";
      }
      if (row.changed) {
        if (const auto problem = capturedNames ? std::nullopt : luciole::filenameProblem(row.name)) {
          row.error = *problem;
        }
        if (plan.nameLimit >= 0 && row.name.size() > static_cast<std::size_t>(plan.nameLimit)) {
          row.error = "Name is too long for this folder.";
        }
        if (!writable) {
          row.error = "The containing folder is not writable.";
        }
      }
      const auto [found, inserted] = targets.emplace(row.name, i);
      if (!inserted) {
        row.error = "Duplicate destination name.";
        (*rows)[found->second].error = row.error;
      }
      if (row.changed && row.error.empty() && !sources.contains(row.name)) {
        struct stat info{};
        // A case-insensitive folder resolves a case-only rename to the item itself; apply's
        // RENAME_NOREPLACE still rejects a hard-link alias under the new name.
        if (::fstatat(plan.fd.get(), row.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
          if (!luciole::sameIdentity(info, plan.items[i]->identity)) {
            row.error = "An unselected item already has this name.";
          }
        } else if (errno != ENOENT) {
          row.error = std::strerror(errno);
        }
      }
    }
    return rows;
  }
  bool applicable(const std::vector<luciole::BatchRenameRow>& rows) {
    return std::ranges::any_of(rows, [](const auto& row) { return row.changed; })
        && std::ranges::none_of(rows, [](const auto& row) { return !row.error.empty(); });
  }
  class Stage {
  public:
    explicit Stage(const Prepared& plan) : m_parent(plan.fd.get()) {
      const luciole::GString uuid(g_uuid_string_random());
      m_name = ".luciole-rename-" + std::string(uuid.get());
      if (::mkdirat(m_parent, m_name.c_str(), 0700) != 0) {
        luciole::throwErrno(errno);
      }
      luciole::Fd fd(::openat(m_parent, m_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (!fd.valid()) {
        const int code = errno;
        (void)::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR);
        luciole::throwErrno(code);
      }
      m_fd = std::move(fd);
    }
    ~Stage() {
      struct stat current{}, held{};
      if (m_fd.valid()
          && ::fstat(m_fd.get(), &held) == 0
          && ::fstatat(m_parent, m_name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
          && luciole::sameIdentity(current, held)) {
        // Never remove contents: incomplete recovery must remain available for inspection.
        (void)::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR);
      }
    }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    int fd() const { return m_fd.get(); }

  private:
    int m_parent;
    std::string m_name;
    luciole::Fd m_fd;
  };
  void move(const Item& item, int from, const std::string& oldName, int to, const std::string& newName) {
    if (!item.current(from, oldName)) {
      luciole::throwFileError(FileErrorKind::Changed, "An item changed during the operation.");
    }
    if (luciole::renameNoReplace(from, oldName.c_str(), to, newName.c_str()) != 0) {
      luciole::throwErrno(errno);
    }
    if (item.revision) {
      // Do not throw after a committed move: the caller must record its new position.
      struct stat stamp{};
      if (::fstat(item.fd.get(), &stamp) != 0) {
        stamp = {};
      }
      *item.revision = stamp;
    }
  }
  std::vector<std::vector<std::size_t>>
  components(const Prepared& plan, const std::vector<luciole::BatchRenameRow>& rows) {
    std::map<std::string, std::size_t> old;
    std::vector<std::vector<std::size_t>> edges(rows.size()), groups;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      old.emplace(plan.items[i]->name, i);
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (const auto found = old.find(rows[i].name); rows[i].changed && found != old.end() && found->second != i) {
        edges[i].push_back(found->second);
        edges[found->second].push_back(i);
      }
    }
    std::vector<bool> seen(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (seen[i] || !rows[i].changed) {
        continue;
      }
      std::vector<std::size_t> group, pending{i};
      seen[i] = true;
      while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        group.push_back(index);
        for (const auto adjacent : edges[index]) {
          if (!seen[adjacent]) {
            seen[adjacent] = true;
            pending.push_back(adjacent);
          }
        }
      }
      std::ranges::sort(group);
      groups.push_back(std::move(group));
    }
    return groups;
  }
  enum class Position { Original, Staged, Published, Unknown };
  // Adds a sentence to a result's error, or starts one of kind.
  void appendError(std::optional<luciole::FileError>& error, FileErrorKind kind, const std::string& text) {
    if (error) {
      error->message += " " + text;
    } else {
      error = luciole::FileError{.kind = kind, .message = text};
    }
  }
  std::shared_ptr<std::vector<luciole::BatchRenameResult>> execute(
      const Prepared& plan, const std::vector<luciole::BatchRenameRow>& rows, const luciole::CancelToken& token,
      std::atomic<std::size_t>& completed
  ) {
    using O = luciole::BatchRenameOutcome;
    auto results = std::make_shared<std::vector<luciole::BatchRenameResult>>();
    std::vector<Position> positions(rows.size(), Position::Original);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      results->push_back(
          {plan.items[i]->entry.location(), std::nullopt, rows[i].changed ? O::Cancelled : O::Unchanged, {}}
      );
      if (!rows[i].changed) {
        completed.fetch_add(1);
      }
    }
    std::unique_ptr<Stage> stage;
    for (const auto& group : components(plan, rows)) {
      if (token.cancelled()) {
        break;
      }
      std::optional<luciole::FileError> problem;
      try {
        plan.requireCurrent();
        if (!stage) {
          stage = std::make_unique<Stage>(plan);
        }
        for (const auto index : group) {
          if (token.cancelled()) {
            break;
          }
          plan.requireCurrent();
          move(*plan.items[index], plan.fd.get(), plan.items[index]->name, stage->fd(), std::to_string(index));
          positions[index] = Position::Staged;
        }
        if (!token.cancelled()) {
          // Once publication starts, finish this connected group before observing cancellation.
          for (const auto index : group) {
            plan.requireCurrent();
            move(*plan.items[index], stage->fd(), std::to_string(index), plan.fd.get(), rows[index].name);
            positions[index] = Position::Published;
          }
          for (const auto index : group) {
            (*results)[index].outcome = O::Renamed;
            completed.fetch_add(1);
          }
          continue;
        }
      } catch (const std::exception&) {
        problem = luciole::currentFileError();
      }
      // Recover an incomplete connected group without replacing any intervening entry.
      for (const auto index : std::views::reverse(group)) {
        if (positions[index] != Position::Published) {
          continue;
        }
        try {
          move(*plan.items[index], plan.fd.get(), rows[index].name, stage->fd(), std::to_string(index));
          positions[index] = Position::Staged;
        } catch (const std::exception&) {
          positions[index] = Position::Unknown;
        }
      }
      for (const auto index : group) {
        auto& result = (*results)[index];
        result.outcome = problem ? O::Failed : O::Cancelled;
        result.error = problem;
        if (positions[index] == Position::Staged) {
          try {
            move(*plan.items[index], stage->fd(), std::to_string(index), plan.fd.get(), plan.items[index]->name);
            positions[index] = Position::Original;
          } catch (const std::exception&) {
            const auto failure = luciole::currentFileError();
            result.outcome = O::NeedsAttention;
            appendError(result.error, failure.kind, "Original name could not be restored: " + failure.message);
          }
        } else if (positions[index] == Position::Unknown) {
          result.outcome = O::Unconfirmed;
          appendError(result.error, FileErrorKind::Other, "The item's final location could not be confirmed.");
        }
        completed.fetch_add(1);
      }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      auto& result = (*results)[i];
      const bool staged = positions[i] == Position::Staged;
      if (positions[i] != Position::Unknown) {
        result.actual = plan.actual(
            staged ? stage->fd() : plan.fd.get(),
            staged                                    ? std::to_string(i)
                : positions[i] == Position::Published ? rows[i].name
                                                      : plan.items[i]->name,
            *plan.items[i], !staged
        );
      }
      if (staged && result.actual) {
        appendError(
            result.error, FileErrorKind::Other,
            "Retained at " + luciole::filenameDisplayText(*result.actual->nativePath())
        );
      }
      if (!result.actual && result.outcome != O::Unconfirmed) {
        result.outcome = O::Unconfirmed;
        appendError(result.error, FileErrorKind::Other, "The item's final location could not be confirmed.");
      }
    }
    return results;
  }
} // namespace

struct luciole::BatchRenameReceipt {
  std::shared_ptr<const Prepared> plan;
  std::shared_ptr<const std::vector<BatchRenameRow>> rows;
};

bool luciole::batchRenameUsesMount(const BatchRenameReceipt& receipt, const HistoryMount& mount) {
  return mount.contains(receipt.plan->fd.get())
      || std::ranges::any_of(receipt.plan->items, [&](const auto& item) { return mount.contains(item->fd.get()); });
}

namespace {
  using Revisions = std::map<std::pair<dev_t, ino_t>, std::shared_ptr<struct stat>>;

  void captureItem(
      Prepared& copy, const Item& original, luciole::DirectoryEntry entry, const std::string& name, Revisions& revisions
  ) {
    const auto stamp = luciole::statFd(original.fd.get());
    if (original.revision && !luciole::sameFileRevision(stamp, *original.revision)) {
      luciole::throwFileError(FileErrorKind::Changed, "A renamed item changed. Rename history is no longer valid.");
    }
    auto item = std::make_unique<Item>(std::move(entry), copy.fd.get(), name);
    if (!luciole::sameFileRevision(stamp, luciole::statFd(item->fd.get()))) {
      luciole::throwFileError(
          FileErrorKind::Changed, "A renamed item was replaced. Rename history is no longer valid."
      );
    }
    const auto [found, inserted] =
        revisions.try_emplace(std::pair{stamp.st_dev, stamp.st_ino}, std::make_shared<struct stat>(stamp));
    if (!inserted && !luciole::sameFileRevision(stamp, *found->second)) {
      luciole::throwFileError(FileErrorKind::Changed, "A renamed item changed while its history was checked.");
    }
    item->revision = found->second;
    copy.items.push_back(std::move(item));
  }

  std::shared_ptr<Prepared> copyParent(const Prepared& plan) {
    auto copy = std::make_shared<Prepared>(plan.parent);
    if (!luciole::sameIdentity(luciole::statFd(plan.fd.get()), luciole::statFd(copy->fd.get()))) {
      luciole::throwFileError(
          FileErrorKind::Changed, "The containing folder changed. Rename history is no longer valid."
      );
    }
    return copy;
  }

  std::shared_ptr<const luciole::BatchRenameReceipt> captureHistory(
      const Prepared& plan, const std::vector<luciole::BatchRenameRow>& rows,
      const std::vector<luciole::BatchRenameResult>& results
  ) {
    using O = luciole::BatchRenameOutcome;
    if (std::ranges::any_of(results, [](const auto& result) {
          return result.outcome != O::Renamed && result.outcome != O::Unchanged;
        })) {
      return {};
    }
    auto reverse = copyParent(plan);
    auto targets = std::make_shared<std::vector<luciole::BatchRenameRow>>();
    Revisions revisions;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (!rows[i].changed) {
        continue;
      }
      auto entry = plan.items[i]->entry;
      entry.place = *results[i].actual;
      entry.name = rows[i].name;
      entry.setDisplayName(rows[i].after);
      captureItem(*reverse, *plan.items[i], std::move(entry), rows[i].name, revisions);
      targets->push_back({rows[i].after, rows[i].before, plan.items[i]->name, {}, true});
    }
    if (targets->empty()) {
      return {};
    }
    return std::make_shared<const luciole::BatchRenameReceipt>(std::move(reverse), std::move(targets));
  }

  std::shared_ptr<Prepared> replayPlan(const Prepared& proof, const luciole::CancelToken& token) {
    auto plan = copyParent(proof);
    Revisions revisions;
    for (const auto& item : proof.items) {
      if (token.cancelled()) {
        luciole::throwFileError(FileErrorKind::Cancelled, "Batch rename reversal cancelled.");
      }
      captureItem(*plan, *item, item->entry, item->name, revisions);
    }
    return plan;
  }

  constexpr std::size_t maxGeneratedName = 4096;

  struct GeneratedName {
    using value_type = char;
    std::string text;

    void append(std::string_view suffix) {
      if (suffix.size() > maxGeneratedName - text.size()) {
        luciole::throwFileError(FileErrorKind::NameTooLong, "Generated name is too long.");
      }
      text += suffix;
    }

    // Let regex_replace enforce the same bound as literal replacement during expansion.
    void push_back(char byte) { append(std::string_view(&byte, 1)); }
  };

  class NameGenerator {
  public:
    explicit NameGenerator(const luciole::BatchRenameOptions& options);
    std::string operator()(const std::string& name, bool directory, std::size_t index) const;

  private:
    const luciole::BatchRenameOptions& m_options;
    std::optional<std::regex> m_regex;
  };
} // namespace

NameGenerator::NameGenerator(const luciole::BatchRenameOptions& o) : m_options(o) {
  if (o.pattern.size() > 4096
      || o.find.size() > 4096
      || o.replacement.size() > 4096
      || o.start < 0
      || o.start > 1000000000
      || o.digits < 1
      || o.digits > 9) {
    luciole::throwFileError(FileErrorKind::InvalidArgument, "Rename options exceed their limits.");
  }
  if (o.mode == luciole::BatchRenameMode::Regex) {
    if (o.find.empty()) {
      luciole::throwFileError(FileErrorKind::InvalidArgument, "Enter a regular expression to find.");
    }
    auto flags = std::regex_constants::ECMAScript;
#ifdef __GLIBCXX__
    // The default backtracking matcher can take exponential time and cannot be cancelled mid-match.
    // Polynomial mode bounds it, at the cost of rejecting back-references.
    flags |= std::regex_constants::__polynomial;
#endif
    try {
      m_regex.emplace(o.find, flags);
    } catch (const std::regex_error& error) {
      luciole::throwFileError(
          FileErrorKind::InvalidArgument, std::string("Invalid regular expression: ") + error.what()
      );
    }
  }
}

std::string NameGenerator::operator()(const std::string& name, bool directory, std::size_t index) const {
  using luciole::BatchRenameMode;
  using luciole::throwFileError;
  const auto& o = m_options;
  const auto stemLength = o.keepExtensions ? luciole::filenameStemLength(name, directory) : name.size();
  const auto stem = std::string_view(name).substr(0, stemLength), extension = std::string_view(name).substr(stemLength);
  GeneratedName result;
  if (o.mode == BatchRenameMode::Numbering) {
    auto number = std::to_string(static_cast<std::uint64_t>(o.start) + index);
    number.insert(
        0, number.size() < static_cast<std::size_t>(o.digits) ? static_cast<std::size_t>(o.digits) - number.size() : 0,
        '0'
    );
    for (std::size_t pos = 0; pos < o.pattern.size();) {
      const auto tail = std::string_view(o.pattern).substr(pos);
      if (tail.starts_with("{name}")) {
        result.append(stem);
        pos += 6;
      } else if (tail.starts_with("{number}")) {
        result.append(number);
        pos += 8;
      } else if (tail.starts_with("{{") || tail.starts_with("}}")) {
        result.append(tail.substr(0, 1));
        pos += 2;
      } else if (tail[0] == '{' || tail[0] == '}') {
        throwFileError(FileErrorKind::InvalidArgument, "Use {name}, {number}, {{ or }} in the pattern.");
      } else {
        result.append(tail.substr(0, 1));
        ++pos;
      }
    }
  } else if (o.mode == BatchRenameMode::Replace) {
    if (o.find.empty()) {
      throwFileError(FileErrorKind::InvalidArgument, "Enter text to find.");
    }
    std::size_t start = 0;
    for (;;) {
      const auto found = stem.find(o.find, start);
      if (found == std::string_view::npos) {
        result.append(stem.substr(start));
        break;
      }
      result.append(stem.substr(start, found - start));
      result.append(o.replacement);
      start = found + o.find.size();
    }
  } else if (m_regex) {
    try {
      std::regex_replace(std::back_inserter(result), stem.begin(), stem.end(), *m_regex, o.replacement);
    } catch (const std::regex_error& error) {
      throwFileError(
          FileErrorKind::InvalidArgument, std::string("Regular expression matching failed: ") + error.what()
      );
    }
  } else {
    throwFileError(FileErrorKind::InvalidArgument, "Unknown rename mode.");
  }
  result.append(extension);
  return std::move(result.text);
}

std::string
luciole::batchRenameName(const std::string& name, bool directory, std::size_t index, const BatchRenameOptions& o) {
  return NameGenerator(o)(name, directory, index);
}

struct luciole::BatchRenameService::State {
  BatchRenameSnapshot snapshot;
  BatchRenameOptions options;
  std::shared_ptr<Prepared> prepared;
  // One token per prepared selection, shared by its apply; each preview has its own.
  CancelToken token;
  CancelToken previewToken;
  std::atomic<std::size_t> completed{0};
  std::uint64_t version = 0;
  bool active = false;
  std::function<void()> changed;
  enum class Kind : std::uint8_t { Prepare, Preview, Apply, Replay };
  struct Reply {
    std::shared_ptr<Prepared> prepared;
    std::shared_ptr<std::vector<BatchRenameRow>> rows;
    std::shared_ptr<std::vector<BatchRenameResult>> results;
    std::string report;
    std::shared_ptr<const BatchRenameReceipt> receipt;
  };
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void launch(const std::shared_ptr<State>& owner, Kind kind, std::move_only_function<Reply()> work) {
    owner->active = true;
    runJob<Reply>(std::move(work), [owner, kind, version = owner->version](JobResult<Reply> reply) {
      owner->active = false;
      if (kind != Kind::Apply && kind != Kind::Replay && owner->token.cancelled()) {
        owner->snapshot.phase = BatchRenamePhase::Cancelled;
        owner->prepared.reset();
      } else if (kind == Kind::Preview && version != owner->version) {
        startPreview(owner);
        return;
      } else if (!reply) {
        owner->snapshot.phase =
            reply.error().kind == FileErrorKind::Cancelled ? BatchRenamePhase::Cancelled : BatchRenamePhase::Error;
        owner->snapshot.error = std::move(reply.error());
      } else if (kind == Kind::Prepare) {
        owner->prepared = reply->prepared;
        owner->snapshot.parent = owner->prepared->parent;
        startPreview(owner);
        return;
      } else if (kind == Kind::Preview || !reply->results) {
        owner->snapshot.phase = BatchRenamePhase::Ready;
        owner->snapshot.rows = reply->rows;
        owner->snapshot.canApply = reply->rows && applicable(*reply->rows);
      } else {
        owner->snapshot.phase = BatchRenamePhase::Finished;
        owner->snapshot.results = reply->results;
        owner->snapshot.rows = reply->rows;
        owner->snapshot.report = std::move(reply->report);
        owner->snapshot.receipt = std::move(reply->receipt);
        owner->prepared.reset();
      }
      owner->notify();
    });
    owner->notify();
  }
  static void startPreview(const std::shared_ptr<State>& owner) {
    owner->snapshot.phase = BatchRenamePhase::Previewing;
    owner->snapshot.canApply = false;
    owner->previewToken = CancelToken{};
    launch(owner, Kind::Preview, [plan = owner->prepared, options = owner->options, token = owner->previewToken] {
      if (token.cancelled()) {
        return Reply{};
      }
      const NameGenerator generateName(options);
      std::vector<std::string> names;
      for (std::size_t i = 0; i < plan->items.size() && !token.cancelled(); ++i) {
        const auto& entry = plan->items[i]->entry;
        names.push_back(generateName(plan->items[i]->name, entry.type == FileType::Directory && !entry.symlink, i));
      }
      if (token.cancelled()) {
        return Reply{};
      }
      return Reply{.prepared = {}, .rows = validate(*plan, names, token), .results = {}, .report = {}, .receipt = {}};
    });
  }
  static Reply executePlan(
      const std::shared_ptr<State>& owner, const std::shared_ptr<Prepared>& plan,
      std::shared_ptr<std::vector<BatchRenameRow>> rows, const CancelToken& token
  ) {
    auto results = execute(*plan, *rows, token, owner->completed);
    std::shared_ptr<const BatchRenameReceipt> receipt;
    try {
      receipt = captureHistory(*plan, *rows, *results);
    } catch (const FileErrorException&) {
      // Capturing optional history cannot turn completed effects into a failed operation.
    }
    std::string report = "Batch rename in " + filenameDisplayText(plan->path) + "\n";
    constexpr std::array labels{"Renamed", "Unchanged", "Failed", "Cancelled", "Needs attention", "Unconfirmed"};
    for (std::size_t i = 0; i < rows->size(); ++i) {
      const auto& result = (*results)[i];
      (*rows)[i].error = result.error ? result.error->message : std::string{};
      report += (*rows)[i].before + " -> " + (*rows)[i].after + ": " + labels[static_cast<std::size_t>(result.outcome)];
      if (result.actual) {
        report += "\n  " + result.actual->uri();
      }
      if (result.error) {
        report += "\n  " + result.error->message;
      }
      report += '\n';
    }
    return Reply{
        .prepared = {},
        .rows = std::move(rows),
        .results = std::move(results),
        .report = std::move(report),
        .receipt = std::move(receipt)
    };
  }
};
luciole::BatchRenameService::BatchRenameService() : m_state(std::make_shared<State>()) {}
luciole::BatchRenameService::~BatchRenameService() {
  m_state->changed = {};
  cancel();
}
bool luciole::BatchRenameService::busy() const { return m_state->active; }
bool luciole::BatchRenameService::applying() const {
  return m_state->snapshot.phase == BatchRenamePhase::Applying && busy();
}
std::size_t luciole::BatchRenameService::completed() const { return m_state->completed.load(); }
const luciole::BatchRenameSnapshot& luciole::BatchRenameService::snapshot() const { return m_state->snapshot; }
std::shared_ptr<const luciole::BatchRenameReceipt> luciole::BatchRenameService::takeReceipt() {
  return std::move(m_state->snapshot.receipt);
}
void luciole::BatchRenameService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void luciole::BatchRenameService::cancel() {
  m_state->token.cancel();
  m_state->previewToken.cancel();
  m_state->snapshot.canApply = false;
  if (!busy()) {
    m_state->prepared.reset();
    if (m_state->snapshot.phase != BatchRenamePhase::Finished) {
      m_state->snapshot.phase = BatchRenamePhase::Cancelled;
    }
  }
}
bool luciole::BatchRenameService::prepare(std::vector<DirectoryEntry> entries) {
  if (busy() || entries.size() < 2 || entries.size() > 4096) {
    return false;
  }
  m_state->prepared.reset();
  m_state->token = CancelToken{};
  m_state->completed.store(0);
  ++m_state->version;
  m_state->options = {};
  m_state->snapshot = {};
  m_state->snapshot.phase = BatchRenamePhase::Preparing;
  State::launch(m_state, State::Kind::Prepare, [token = m_state->token, entries = std::move(entries)] mutable {
    return State::Reply{
        .prepared = prepareItems(std::move(entries), token), .rows = {}, .results = {}, .report = {}, .receipt = {}
    };
  });
  return true;
}
bool luciole::BatchRenameService::preview(BatchRenameOptions options) {
  if (applying()
      || m_state->token.cancelled()
      || (!m_state->prepared && m_state->snapshot.phase != BatchRenamePhase::Preparing)) {
    return false;
  }
  m_state->options = std::move(options);
  ++m_state->version;
  m_state->snapshot.canApply = false;
  m_state->snapshot.error.reset();
  m_state->previewToken.cancel();
  if (!busy()) {
    State::startPreview(m_state);
  } else if (m_state->prepared) {
    m_state->snapshot.phase = BatchRenamePhase::Previewing;
  }
  return true;
}
bool luciole::BatchRenameService::apply() {
  if (busy() || !m_state->snapshot.canApply || !m_state->prepared) {
    return false;
  }
  m_state->snapshot.phase = BatchRenamePhase::Applying;
  m_state->snapshot.canApply = false;
  State::launch(
      m_state, State::Kind::Apply,
      [owner = m_state, token = m_state->token, plan = m_state->prepared, approved = m_state->snapshot.rows] {
        std::vector<std::string> names;
        for (const auto& row : *approved) {
          names.push_back(row.name);
        }
        auto rows = validate(*plan, names, token);
        if (!token.cancelled() && !applicable(*rows)) {
          return State::Reply{.prepared = {}, .rows = std::move(rows), .results = {}, .report = {}, .receipt = {}};
        }
        return State::executePlan(owner, plan, std::move(rows), token);
      }
  );
  return true;
}

bool luciole::BatchRenameService::replay(std::shared_ptr<const BatchRenameReceipt> receipt) {
  if (busy() || !receipt) {
    return false;
  }
  m_state->prepared.reset();
  m_state->token = CancelToken{};
  m_state->completed.store(0);
  ++m_state->version;
  m_state->snapshot = {};
  m_state->snapshot.phase = BatchRenamePhase::Applying;
  m_state->snapshot.parent = receipt->plan->parent;
  m_state->snapshot.rows = receipt->rows;
  State::launch(m_state, State::Kind::Replay, [owner = m_state, token = m_state->token, receipt = std::move(receipt)] {
    auto plan = replayPlan(*receipt->plan, token);
    std::vector<std::string> names;
    for (const auto& row : *receipt->rows) {
      names.push_back(row.name);
    }
    auto rows = validate(*plan, names, token, true);
    if (!token.cancelled() && !applicable(*rows)) {
      for (const auto& item : plan->items) {
        if (!item->current(plan->fd.get(), item->name)) {
          throwFileError(FileErrorKind::Changed, "A renamed item changed. Rename history is no longer valid.");
        }
      }
      const auto problem = std::ranges::find_if(*rows, [](const auto& row) { return !row.error.empty(); });
      throwFileError(FileErrorKind::Exists, problem == rows->end() ? "No names can be restored." : problem->error);
    }
    return State::executePlan(owner, plan, std::move(rows), token);
  });
  return true;
}
