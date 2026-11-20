#include "operations/transfer_copy.h"

#include "operations/history_revisions.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <linux/fs.h>
#include <memory>
#include <sys/ioctl.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace luciole::transfer {
  namespace {
    void copyAttributes(int from, int to, CopyWork& work) {
      const auto count = ::flistxattr(from, nullptr, 0);
      if (count < 0) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP) {
          return;
        }
        fail("Could not read file attributes");
      }
      if (count == 0) {
        return;
      }
      if (count > 1024 * 1024) {
        throwFileError(FileErrorKind::Unsupported, "File attributes are too large.");
      }
      std::vector<char> names(static_cast<std::size_t>(count));
      if (::flistxattr(from, names.data(), names.size()) != count) {
        fail("File attributes changed");
      }
      for (std::size_t offset = 0; offset < names.size();) {
        work.check();
        const auto length = ::strnlen(names.data() + offset, names.size() - offset);
        if (length == names.size() - offset) {
          throwFileError(FileErrorKind::Other, "Invalid file attributes.");
        }
        const std::string key(names.data() + offset, length);
        offset += length + 1;
        if (!key.starts_with("user.") && key != "system.posix_acl_access" && key != "system.posix_acl_default") {
          continue;
        }
        const auto size = ::fgetxattr(from, key.c_str(), nullptr, 0);
        if (size < 0) {
          fail("Could not read file attribute");
        }
        if (size > 1024 * 1024) {
          throwFileError(FileErrorKind::Unsupported, "A file attribute is too large.");
        }
        std::vector<char> value(static_cast<std::size_t>(size));
        if (::fgetxattr(from, key.c_str(), value.data(), value.size()) != size) {
          fail("File attributes changed");
        }
        if (::fsetxattr(to, key.c_str(), value.data(), value.size(), 0) != 0) {
          if (errno == ENOTSUP || errno == EOPNOTSUPP) {
            work.metadataIncomplete = true;
          } else {
            fail("Could not preserve file attributes");
          }
        }
      }
    }

    void preserveMetadata(int input, int fd, const struct stat& source, CopyWork& work) {
      const timespec times[]{source.st_atim, source.st_mtim};
      if (::fchmod(fd, source.st_mode & 0777) != 0 || ::futimens(fd, times) != 0) {
        fail("Could not preserve file metadata");
      }
      copyAttributes(input, fd, work);
    }

    // Errors meaning "this kernel copy path does not apply here": fall back to the next one.
    bool unsupported(int error) {
      return error == EXDEV
          || error == EOPNOTSUPP
          || error == ENOTSUP
          || error == EINVAL
          || error == ENOSYS
          || error == EBADF
          || error == ETXTBSY
          || error == ENOTTY
          || error == EPERM;
    }

    // Copies the contents of `input` into the empty `output`: a copy-on-write clone (btrfs, XFS, bcachefs), then an
    // in-kernel copy, then a userspace loop. Each step resumes where the previous one stopped.
    void copyData(int input, int output, CopyWork& work) {
      if (!work.cloneUnsupported) {
        if (::ioctl(output, FICLONE, input) == 0) {
          work.bytes.fetch_add(static_cast<std::uint64_t>(statFd(output).st_size));
          return;
        }
        if (!unsupported(errno)) {
          fail("Could not clone source");
        }
        work.cloneUnsupported = errno == EOPNOTSUPP || errno == ENOTSUP || errno == ENOSYS || errno == ENOTTY;
      }
      // Chunks keep progress and cancellation responsive on large files.
      constexpr std::size_t kChunk = 8 * 1024 * 1024;
      while (!work.rangeUnsupported) {
        work.check();
        const auto copied = ::copy_file_range(input, nullptr, output, nullptr, kChunk, 0);
        if (copied > 0) {
          work.bytes.fetch_add(static_cast<std::uint64_t>(copied));
          continue;
        }
        if (copied == 0) {
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        if (!unsupported(errno)) {
          fail("Could not copy source");
        }
        work.rangeUnsupported = errno == ENOSYS;
        break;
      }
      auto& buffer = work.buffer;
      for (;;) {
        work.check();
        const auto size = ::read(input, buffer.data(), buffer.size());
        if (size < 0) {
          if (errno == EINTR) {
            continue;
          }
          fail("Could not read source");
        }
        if (size == 0) {
          return;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(size)) {
          work.check();
          const auto written = ::write(output, buffer.data() + offset, static_cast<std::size_t>(size) - offset);
          if (written < 0) {
            if (errno == EINTR) {
              continue;
            }
            fail("Could not write destination");
          }
          if (written == 0) {
            throwFileError(FileErrorKind::Other, "The destination stopped accepting data.");
          }
          offset += static_cast<std::size_t>(written);
          work.bytes.fetch_add(static_cast<std::uint64_t>(written));
        }
      }
    }
  } // namespace

  void synchronize(int fd) {
    if (::syncfs(fd) != 0) {
      fail("Could not synchronize copied data");
    }
  }

  void fail(const char* operation) {
    auto error = errnoError(errno);
    error.message = std::format("{}: {}", operation, error.message);
    throw FileErrorException(std::move(error));
  }

  Fd opened(int fd) {
    if (fd < 0) {
      fail("Could not open item");
    }
    return Fd(fd);
  }

  struct stat info(int parent, const std::string& name) {
    struct stat value{};
    if (::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
      fail("Could not inspect item");
    }
    return value;
  }

  bool unchanged(const struct stat& a, const struct stat& b) {
    return sameIdentity(a, b)
        && a.st_mode == b.st_mode
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
  }

  bool sameContent(const struct stat& a, const struct stat& b) {
    return sameIdentity(a, b)
        && a.st_mode == b.st_mode
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
  }

  void requireUnchanged(const struct stat& expected, const struct stat& actual) {
    if (!unchanged(expected, actual)) {
      throwFileError(FileErrorKind::Changed, "The source changed during the transfer.");
    }
  }

  Fd directory(int parent, const std::string& name) {
    return opened(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  }

  std::vector<std::string> children(int fd, std::size_t limit) {
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
      fail("Could not read folder");
    }
    struct CloseDirectory {
      void operator()(DIR* stream) const { ::closedir(stream); }
    };
    const std::unique_ptr<DIR, CloseDirectory> stream(::fdopendir(duplicate));
    if (!stream) {
      ::close(duplicate);
      fail("Could not read folder");
    }
    ::rewinddir(stream.get());
    std::vector<std::string> result;
    for (;;) {
      errno = 0;
      const auto* entry = ::readdir(stream.get());
      if (entry == nullptr) {
        if (errno != 0) {
          fail("Could not read folder");
        }
        break;
      }
      const std::string name(entry->d_name);
      if (name != "." && name != "..") {
        if (result.size() >= limit) {
          throwFileError(FileErrorKind::Unsupported, "The merge exceeds its 16384-item limit. Select smaller folders.");
        }
        result.push_back(name);
      }
    }
    return result;
  }

  bool hasAncestor(int fd, const struct stat& ancestor) {
    auto current = opened(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
    for (std::size_t depth = 0; depth < 65536; ++depth) {
      const auto here = statFd(current.get());
      if (sameIdentity(here, ancestor)) {
        return true;
      }
      auto parent = directory(current.get(), "..");
      if (sameIdentity(here, statFd(parent.get()))) {
        return false;
      }
      // Keep the old directory alive until the new descriptor is acquired.
      const int next = ::fcntl(parent.get(), F_DUPFD_CLOEXEC, 0);
      if (next < 0) {
        fail("Could not inspect parent folder");
      }
      current = Fd(next);
    }
    throwFileError(FileErrorKind::Unsupported, "The folder ancestry is too deep.");
  }

  struct stat CopyWork::expected(const struct stat& original) const {
    const auto found = changedLinks.find({original.st_dev, original.st_ino});
    const auto result = found != changedLinks.end() && sameContent(original, found->second) ? found->second : original;
    return history ? history->expected(result) : result;
  }

  void CopyWork::note(const struct stat& value) {
    if (history) {
      history->observed(value, false);
    }
    changedLinks[{value.st_dev, value.st_ino}] = value;
  }

  void CopyWork::noteUnlinked(const struct stat& value) {
    if (history) {
      history->observed(value, true);
    }
    changedLinks[{value.st_dev, value.st_ino}] = value;
  }

  void CopyWork::check() const {
    if (token.cancelled()) {
      throw TransferCancelled();
    }
  }

  Tree copyTree(int from, const std::string& name, int to, const std::string& target, CopyWork& work, unsigned depth) {
    work.check();
    if (depth > 256) {
      throwFileError(FileErrorKind::Unsupported, "The folder tree is too deep.");
    }
    Tree tree{.name = name, .metadata = info(from, name), .children = {}};
    const auto& source = tree.metadata;
    if (S_ISREG(source.st_mode)) {
      auto input = opened(::openat(from, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      requireUnchanged(source, statFd(input.get()));
      auto output = opened(::openat(to, target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
      copyData(input.get(), output.get(), work);
      requireUnchanged(source, statFd(input.get()));
      preserveMetadata(input.get(), output.get(), source, work);
    } else if (S_ISDIR(source.st_mode)) {
      auto input = directory(from, name);
      requireUnchanged(source, statFd(input.get()));
      if (::mkdirat(to, target.c_str(), 0700) != 0) {
        fail("Could not create destination folder");
      }
      auto output = directory(to, target);
      for (const auto& child : children(input.get())) {
        tree.children.push_back(copyTree(input.get(), child, output.get(), child, work, depth + 1));
      }
      requireUnchanged(source, statFd(input.get()));
      preserveMetadata(input.get(), output.get(), source, work);
    } else if (S_ISLNK(source.st_mode)) {
      std::vector<char> buffer(256);
      for (;;) {
        const auto size = ::readlinkat(from, name.c_str(), buffer.data(), buffer.size());
        if (size < 0) {
          fail("Could not read symbolic link");
        }
        if (static_cast<std::size_t>(size) < buffer.size()) {
          const std::string link(buffer.data(), static_cast<std::size_t>(size));
          if (::symlinkat(link.c_str(), to, target.c_str()) != 0) {
            fail("Could not copy symbolic link");
          }
          break;
        }
        if (buffer.size() >= 1024 * 1024) {
          throwFileError(FileErrorKind::Unsupported, "The symbolic link is too long.");
        }
        buffer.resize(buffer.size() * 2);
      }
      requireUnchanged(source, info(from, name));
      const timespec times[]{source.st_atim, source.st_mtim};
      if (::utimensat(to, target.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
        fail("Could not preserve link metadata");
      }
    } else {
      throwFileError(FileErrorKind::Unsupported, "Special files cannot be transferred.");
    }
    return tree;
  }

  void verifyTree(int parent, const Tree& tree, CopyWork& work) {
    work.check();
    requireUnchanged(work.expected(tree.metadata), info(parent, tree.name));
    if (S_ISDIR(tree.metadata.st_mode)) {
      auto fd = directory(parent, tree.name);
      requireUnchanged(tree.metadata, statFd(fd.get()));
      if (children(fd.get()).size() != tree.children.size()) {
        throwFileError(FileErrorKind::Changed, "The source folder changed.");
      }
      for (const auto& child : tree.children) {
        verifyTree(fd.get(), child, work);
      }
    }
  }

  void removeSource(int parent, const Tree& tree, CopyWork& work) {
    work.check();
    auto pinned = opened(::openat(parent, tree.name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    requireUnchanged(work.expected(tree.metadata), statFd(pinned.get()));
    requireUnchanged(work.expected(tree.metadata), info(parent, tree.name));
    const bool folder = S_ISDIR(tree.metadata.st_mode);
    if (folder) {
      auto fd = directory(parent, tree.name);
      requireUnchanged(tree.metadata, statFd(fd.get()));
      for (const auto& child : tree.children) {
        removeSource(fd.get(), child, work);
      }
      if (!sameIdentity(tree.metadata, info(parent, tree.name))) {
        throwFileError(FileErrorKind::Changed, "The source folder changed.");
      }
    }
    const auto before = statFd(pinned.get());
    if (::unlinkat(parent, tree.name.c_str(), folder ? AT_REMOVEDIR : 0) != 0) {
      fail("Could not remove copied source");
    }
    work.noteUnlinked(statUnlinked(parent, pinned.get(), before));
  }

  void removeTemporary(int parent, const std::string& name, CopyWork& work) {
    const auto value = info(parent, name);
    auto pinned = opened(::openat(parent, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    const bool folder = S_ISDIR(value.st_mode);
    if (folder) {
      auto fd = directory(parent, name);
      if (::fchmod(fd.get(), 0700) != 0) {
        fail("Could not clean temporary folder");
      }
      for (const auto& child : children(fd.get())) {
        removeTemporary(fd.get(), child, work);
      }
    }
    const auto before = statFd(pinned.get());
    if (::unlinkat(parent, name.c_str(), folder ? AT_REMOVEDIR : 0) != 0) {
      fail("Could not clean temporary item");
    }
    work.noteUnlinked(statUnlinked(parent, pinned.get(), before));
  }
} // namespace luciole::transfer
