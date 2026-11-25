#include "operations/permission_walk.h"

#include "fs/filename.h"
#include "fs/system_mounts.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using luciole::PermissionOutcome;
  using luciole::PermissionTarget;

  constexpr std::size_t maxItems = 16384;
  constexpr std::size_t maxDepth = 128;

  // Collection is complete before mutations begin. The item and depth limits also
  // bound retained descriptors, result memory and the traversal stack.
  class CollectionLimit final : public luciole::FileErrorException {
  public:
    using luciole::FileErrorException::FileErrorException;
  };
  [[noreturn]] void limit(luciole::FileErrorKind kind, const char* message) { throw CollectionLimit({kind, message}); }
  [[noreturn]] void descriptorLimit(int code) {
    limit(luciole::errnoError(code).kind, "Not enough file descriptors to collect this job. Select smaller folders.");
  }
  [[noreturn]] void throwErrnoWithContext(int code, const std::string& context) {
    auto error = luciole::errnoError(code);
    luciole::throwFileError(error.kind, context + error.message);
  }
  struct CloseDirectory {
    void operator()(DIR* directory) const { ::closedir(directory); }
  };
  std::uint64_t mountId(int fd) {
    struct statx value{};
    if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &value) != 0
        || (value.stx_mask & STATX_MNT_ID) == 0) {
      luciole::throwFileError(luciole::FileErrorKind::Unsupported, "Cannot identify the filesystem mount boundary.");
    }
    return value.stx_mnt_id;
  }

  class Walker {
  public:
    Walker(
        const luciole::PermissionBatch& roots, luciole::PermissionBatch& plan, const luciole::CancelToken& token,
        std::atomic<std::size_t>& found
    )
        : m_roots(roots), m_plan(plan), m_token(token), m_found(found), m_mounts(luciole::configuredSystemMounts()),
          m_hasAutomounts(std::ranges::any_of(m_mounts, &luciole::MountConfiguration::automount)) {
      for (std::size_t i = 0; i < roots.results->size(); ++i) {
        m_selected.emplace((*roots.results)[i].location.uri(), i);
      }
    }

    void run() {
      std::vector<std::size_t> order;
      order.reserve(m_roots.targets.size());
      for (std::size_t i = 0; i < m_roots.targets.size(); ++i) {
        order.push_back(i);
      }
      // An explicitly selected descendant is reused by its ancestor's walk.
      // Sorting roots by path length ensures ancestors are visited first.
      std::ranges::stable_sort(order, {}, [&](std::size_t index) {
        return (*m_roots.results)[index].location.uri().size();
      });
      for (const auto i : order) {
        if (m_token.cancelled()) {
          break;
        }
        const auto& target = m_roots.targets[i];
        if (target) {
          walk(target, mountId(target->fd.get()), automountPath(target->entry.location()), 0);
        } else if (m_seen.insert((*m_roots.results)[i].location.uri()).second) {
          append((*m_roots.results)[i], {});
        }
      }
    }

  private:
    std::string automountPath(const luciole::Location& location) const {
      if (!m_hasAutomounts) {
        return {};
      }
      const auto config = luciole::resolveAutomountFor(location, m_mounts, false);
      return config ? config->path : std::string{};
    }

    void append(const luciole::PermissionResult& row, std::shared_ptr<PermissionTarget> target) {
      if (m_plan.results->size() >= maxItems) {
        limit(luciole::FileErrorKind::Unsupported, "The recursive job exceeds 16384 items. Select smaller folders.");
      }
      m_plan.results->push_back(row);
      m_plan.targets.push_back(std::move(target));
    }

    void skip(luciole::PermissionResult& row, std::shared_ptr<PermissionTarget> target, std::string message) {
      row.outcome = PermissionOutcome::Skipped;
      row.message = std::move(message);
      append(row, std::move(target));
    }

    void walk(
        std::shared_ptr<PermissionTarget> target, std::uint64_t rootMount, const std::string& rootAutomount,
        std::size_t depth
    ) {
      const auto location = target->entry.location();
      if (m_token.cancelled() || !m_seen.insert(location.uri()).second) {
        return;
      }
      if (m_found.fetch_add(1) >= maxItems) {
        limit(luciole::FileErrorKind::Unsupported, "The recursive job exceeds 16384 items. Select smaller folders.");
      }
      luciole::PermissionResult row{
          .location = location,
          .name = target->entry.displayName(),
          .info = {},
          .outcome = PermissionOutcome::Ready,
          .error = {},
          .message = {}
      };
      try {
        target->validate();
        // A discovered hard link can be changed before its separately selected
        // alias. Capture its ACL too so the job can recognize its own chmod.
        if (S_ISREG(target->baseline.st_mode) && target->baseline.st_nlink > 1 && !target->aclBaseline) {
          target->loadAcl(true);
        }
        row.info = target->inspect(false);
        if (S_ISLNK(target->baseline.st_mode)) {
          skip(row, std::move(target), "Symbolic link skipped; its target was not followed.");
          return;
        }
        if (mountId(target->fd.get()) != rootMount) {
          skip(row, std::move(target), "Mounted subtree skipped.");
          return;
        }
        if (depth > 0 && m_hasAutomounts && S_ISDIR(target->baseline.st_mode)) {
          if (const auto childMount = automountPath(location); !childMount.empty() && childMount != rootAutomount) {
            skip(row, std::move(target), "Configured automount subtree skipped.");
            return;
          }
        }
        if (S_ISDIR(target->baseline.st_mode)) {
          if (depth >= maxDepth) {
            limit(
                luciole::FileErrorKind::Unsupported,
                "The recursive job exceeds 128 folder levels. Select smaller folders."
            );
          }
          if (!m_directories.emplace(target->baseline.st_dev, target->baseline.st_ino).second) {
            skip(row, std::move(target), "Folder already visited; subtree skipped.");
            return;
          }
          walkChildren(target, rootMount, rootAutomount, depth);
        }
        // Postorder keeps parent access intact while processing descendants.
        append(row, std::move(target));
      } catch (const CollectionLimit&) {
        throw;
      } catch (const std::exception&) {
        row.outcome = PermissionOutcome::Failed;
        row.error = luciole::currentFileError();
        append(row, {});
      }
    }

    void walkChildren(
        const std::shared_ptr<PermissionTarget>& target, std::uint64_t rootMount, const std::string& rootAutomount,
        std::size_t depth
    ) {
      const int descriptor = ::openat(target->fd.get(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (descriptor < 0) {
        if (errno == EMFILE || errno == ENFILE) {
          descriptorLimit(errno);
        }
        throwErrnoWithContext(errno, "Contents not inspected: ");
      }
      // fdopendir owns the descriptor only on success.
      const std::unique_ptr<DIR, CloseDirectory> directory(::fdopendir(descriptor));
      if (!directory) {
        const int code = errno;
        ::close(descriptor);
        luciole::throwErrno(code);
      }
      const auto folder = target->entry.location();
      while (!m_token.cancelled()) {
        errno = 0;
        const auto* child = ::readdir(directory.get());
        if (!child) {
          if (errno != 0) {
            throwErrnoWithContext(errno, "Contents only partly inspected: ");
          }
          break;
        }
        const std::string name(child->d_name);
        if (name == "." || name == "..") {
          continue;
        }
        target->validate();
        auto entry = target->entry;
        const auto location = folder.child(name);
        const auto& uri = location.uri();
        entry.place = location;
        entry.name = name;
        entry.setDisplayName(luciole::filenameDisplayText(name));
        if (const auto found = m_selected.find(uri); found != m_selected.end()) {
          const auto& captured = m_roots.targets[found->second];
          if (captured) {
            captured->ancestors = target->ancestors;
            captured->ancestors.push_back(target);
            walk(captured, mountId(captured->fd.get()), automountPath(captured->entry.location()), depth + 1);
          } else if (m_seen.insert(uri).second) {
            append((*m_roots.results)[found->second], {});
          }
          continue;
        }
        if (m_seen.contains(uri)) {
          continue;
        }
        const int fd = ::openat(::dirfd(directory.get()), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
          if (errno == EMFILE || errno == ENFILE) {
            descriptorLimit(errno);
          }
          append(
              {.location = location,
               .name = entry.displayName(),
               .info = {},
               .outcome = PermissionOutcome::Failed,
               .error = luciole::errnoError(errno),
               .message = {}},
              {}
          );
          continue;
        }
        auto ancestors = target->ancestors;
        ancestors.push_back(target);
        walk(
            std::make_shared<PermissionTarget>(std::move(entry), fd, std::move(ancestors)), rootMount, rootAutomount,
            depth + 1
        );
      }
    }

    const luciole::PermissionBatch& m_roots;
    luciole::PermissionBatch& m_plan;
    const luciole::CancelToken& m_token;
    std::atomic<std::size_t>& m_found;
    std::vector<luciole::MountConfiguration> m_mounts;
    bool m_hasAutomounts = false;
    std::map<std::string, std::size_t> m_selected;
    std::set<std::string> m_seen;
    std::set<std::pair<dev_t, ino_t>> m_directories;
  };
} // namespace

void luciole::collectPermissionTargets(
    const PermissionBatch& roots, PermissionBatch& plan, const CancelToken& token, std::atomic<std::size_t>& found
) {
  Walker(roots, plan, token, found).run();
}
