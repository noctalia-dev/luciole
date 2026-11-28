#include "operations/transfer_copy.h"

#include "operations/history_revisions.h"
#include "operations/transfer_pacing.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <format>
#include <linux/fs.h>
#include <memory>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/xattr.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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

    ssize_t copyRangeChunk(int input, int output, std::size_t length, const CopyWork& work) {
      return work.writebackOps && work.writebackOps->copyRange
          ? work.writebackOps->copyRange(input, output, length)
          : ::copy_file_range(input, nullptr, output, nullptr, length, 0);
    }

    ssize_t writeOutputChunk(int output, const char* data, std::size_t length, const CopyWork& work) {
      return work.writebackOps && work.writebackOps->writeChunk ? work.writebackOps->writeChunk(output, data, length)
                                                                : ::write(output, data, length);
    }

    // Copies the contents of `input` into the empty `output`: a copy-on-write clone (btrfs, XFS, bcachefs), then an
    // in-kernel copy, then a userspace loop. Each step resumes where the previous one stopped.
    void copyData(int input, Fd& output, CopyWork& work, std::vector<char>& buffer) {
      if (!work.cloneUnsupported && (!work.writebackOps || work.writebackOps->allowClone)) {
        if (::ioctl(output.get(), FICLONE, input) == 0) {
          work.bytes.fetch_add(static_cast<std::uint64_t>(statFd(output.get()).st_size));
          return;
        }
        if (!unsupported(errno)) {
          fail("Could not clone source");
        }
        if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == ENOSYS || errno == ENOTTY) {
          work.cloneUnsupported = true;
        }
      }
      // Chunks keep progress and cancellation responsive on large files.
      constexpr std::size_t kChunk = 8 * 1024 * 1024;
      std::optional<PacedWriter> pacer;
      if (work.paceLimit > 0) {
        pacer.emplace(output, work);
      }
      const auto record = [&](std::uint64_t count) {
        if (pacer) {
          pacer->wrote(count);
        } else {
          work.bytes.fetch_add(count);
        }
      };
      const auto limit = [&](std::size_t maxChunk) {
        return pacer ? std::min(maxChunk, pacer->remaining()) : maxChunk;
      };
      while (!work.rangeUnsupported) {
        work.check();
        const auto copied = copyRangeChunk(input, output.get(), limit(kChunk), work);
        if (copied > 0) {
          record(static_cast<std::uint64_t>(copied));
          continue;
        }
        if (copied == 0) {
          if (pacer) {
            pacer->finish();
          }
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        if (!unsupported(errno)) {
          fail("Could not copy source");
        }
        if (errno == ENOSYS) {
          work.rangeUnsupported = true;
        }
        break;
      }
      for (;;) {
        work.check();
        const auto size = ::read(input, buffer.data(), limit(buffer.size()));
        if (size < 0) {
          if (errno == EINTR) {
            continue;
          }
          fail("Could not read source");
        }
        if (size == 0) {
          if (pacer) {
            pacer->finish();
          }
          return;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(size)) {
          work.check();
          const auto written = writeOutputChunk(
              output.get(), buffer.data() + offset, limit(static_cast<std::size_t>(size) - offset), work
          );
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
          record(static_cast<std::uint64_t>(written));
        }
      }
    }

    void copyFile(
        int from, const std::string& name, int to, const std::string& target, const struct stat& source, CopyWork& work,
        std::vector<char>& buffer
    ) {
      if (work.nameObserver) {
        work.nameObserver(name);
      }
      auto input = opened(::openat(from, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      requireUnchanged(source, statFd(input.get()));
      auto output = opened(::openat(to, target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
      copyData(input.get(), output, work, buffer);
      requireUnchanged(source, statFd(input.get()));
      preserveMetadata(input.get(), output.get(), source, work);
      if (closeOutput(output, work.writebackOps) != 0) {
        fail("Could not close copied file");
      }
      if (work.fileObserver) {
        work.fileObserver();
      }
    }

    void copyLink(
        int from, const std::string& name, int to, const std::string& target, const struct stat& source, CopyWork& work
    ) {
      if (work.nameObserver) {
        work.nameObserver(name);
      }
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
      if (work.fileObserver) {
        work.fileObserver();
      }
    }

    Fd makeFolder(int to, const std::string& target) {
      if (::mkdirat(to, target.c_str(), 0700) != 0) {
        fail("Could not create destination folder");
      }
      return directory(to, target);
    }

    void finishFolder(int input, int output, const struct stat& source, CopyWork& work) {
      requireUnchanged(source, statFd(input));
      preserveMetadata(input, output, source, work);
    }

    [[noreturn]] void throwTooDeep() { throwFileError(FileErrorKind::Unsupported, "The folder tree is too deep."); }

    // Copy threads per job, and files queued ahead of them so the walk stays only a little ahead.
    constexpr std::size_t kCopyThreads = 8;
    constexpr std::size_t kCopyQueue = 256;
    // Walked folders get their metadata in batches, bounding open folder descriptors.
    constexpr std::size_t kFolderBatch = 64;
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
      auto parent = opened(::openat(current.get(), "..", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
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
    if (revisionObserver) {
      revisionObserver(value, false);
    }
  }

  void CopyWork::noteUnlinked(const struct stat& value) {
    if (history) {
      history->observed(value, true);
    }
    changedLinks[{value.st_dev, value.st_ino}] = value;
    if (revisionObserver) {
      revisionObserver(value, true);
    }
  }

  void CopyWork::check() const {
    if (token.cancelled() || stopCopies.load(std::memory_order_relaxed)) {
      throw TransferCancelled();
    }
  }

  // Threads copying the regular files of parallel folder copies, fed by the walking thread.
  class CopyPool {
  public:
    explicit CopyPool(CopyWork& work) : m_work(work) {
      m_threads.reserve(kCopyThreads);
      for (std::size_t i = 0; i < kCopyThreads; ++i) {
        try {
          m_threads.emplace_back([this] { run(); });
        } catch (const std::system_error&) {
          if (m_threads.empty()) {
            throw;
          }
          break;
        }
      }
    }
    ~CopyPool() {
      {
        const std::scoped_lock lock(m_mutex);
        m_closing = true;
      }
      m_jobs.notify_all();
      for (auto& thread : m_threads) {
        thread.join();
      }
    }
    CopyPool(const CopyPool&) = delete;
    CopyPool& operator=(const CopyPool&) = delete;

    // Queues file (name and metadata already captured) for copying from `from` into `to` under the same name.
    void submit(const Tree& file, int from, int to) {
      std::unique_lock lock(m_mutex);
      m_room.wait(lock, [&] { return m_queue.size() < kCopyQueue || m_work.stopCopies; });
      if (m_work.stopCopies) {
        throw TransferCancelled();
      }
      m_queue.push_back({.from = from, .to = to, .file = &file});
      lock.unlock();
      m_jobs.notify_one();
    }

    // Waits until every queued file is copied; rethrows the first failure.
    void drain() {
      std::unique_lock lock(m_mutex);
      m_room.wait(lock, [&] { return m_queue.empty() && m_running == 0; });
      if (m_error) {
        const auto error = std::exchange(m_error, nullptr);
        m_work.stopCopies = false;
        lock.unlock();
        std::rethrow_exception(error);
      }
    }

    // Drops queued files, waits for running ones and returns the first failure, if any.
    std::exception_ptr abandon() noexcept {
      std::unique_lock lock(m_mutex);
      m_work.stopCopies = true;
      m_queue.clear();
      m_room.wait(lock, [&] { return m_running == 0; });
      m_work.stopCopies = false;
      return std::exchange(m_error, nullptr);
    }

  private:
    struct Job {
      int from;
      int to;
      const Tree* file;
    };

    void run() {
      std::vector<char> buffer(128 * 1024);
      for (;;) {
        Job job{};
        {
          std::unique_lock lock(m_mutex);
          m_jobs.wait(lock, [&] { return m_closing || !m_queue.empty(); });
          if (m_closing) {
            return;
          }
          job = m_queue.front();
          m_queue.pop_front();
          ++m_running;
        }
        m_room.notify_all();
        try {
          if (!m_work.stopCopies) {
            copyFile(job.from, job.file->name, job.to, job.file->name, job.file->metadata, m_work, buffer);
          }
        } catch (...) {
          const std::scoped_lock lock(m_mutex);
          // Copies stopped for a sibling's failure throw too; only the first real failure is kept.
          if (!m_error && !m_work.stopCopies) {
            m_error = std::current_exception();
          }
          m_work.stopCopies = true;
        }
        {
          const std::scoped_lock lock(m_mutex);
          --m_running;
        }
        m_room.notify_all();
      }
    }

    CopyWork& m_work;
    std::mutex m_mutex;
    // A job or closing, for the copy threads.
    std::condition_variable m_jobs;
    // Queue room, a failure, or idle, for the walking thread.
    std::condition_variable m_room;
    std::deque<Job> m_queue;
    std::size_t m_running = 0;
    bool m_closing = false;
    std::exception_ptr m_error;
    std::vector<std::thread> m_threads;
  };

  CopyWork::CopyWork(CancelToken cancel, std::atomic<std::uint64_t>& copied)
      : token(std::move(cancel)), bytes(copied), buffer(128 * 1024) {}
  CopyWork::~CopyWork() = default;

  namespace {
    // One parallel folder copy: this thread walks the source, creates folders and links, and queues regular files
    // for the pool. It owns every folder descriptor, so none closes while a queued or running copy uses it.
    class ParallelCopy {
    public:
      explicit ParallelCopy(CopyWork& work) : m_work(work) {}

      // tree.name and tree.metadata are set; copies it to `target` inside `to`.
      void walk(int from, int to, const std::string& target, Tree& tree, unsigned depth) {
        m_work.check();
        if (depth > 256) {
          throwTooDeep();
        }
        const auto& source = tree.metadata;
        if (S_ISREG(source.st_mode)) {
          pool().submit(tree, from, to);
        } else if (S_ISDIR(source.st_mode)) {
          auto& folder =
              m_folders.emplace_back(OpenFolder{.input = directory(from, tree.name), .output = {}, .tree = &tree});
          requireUnchanged(source, statFd(folder.input.get()));
          folder.output = makeFolder(to, target);
          auto names = children(folder.input.get());
          // Sized once: queued copies point at these entries.
          tree.children.resize(names.size());
          for (std::size_t i = 0; i < names.size(); ++i) {
            auto& child = tree.children[i];
            child.name = std::move(names[i]);
            child.metadata = info(folder.input.get(), child.name);
            walk(folder.input.get(), folder.output.get(), child.name, child, depth + 1);
          }
          m_walked.push_back(&folder);
          if (m_walked.size() >= kFolderBatch) {
            finish();
          }
        } else if (S_ISLNK(source.st_mode)) {
          copyLink(from, tree.name, to, target, source, m_work);
        } else {
          throwFileError(FileErrorKind::Unsupported, "Special files cannot be transferred.");
        }
      }

      // Waits for queued files, then gives every walked folder its metadata, children before parents.
      void finish() {
        if (m_work.copyPool) {
          m_work.copyPool->drain();
        }
        for (auto* folder : m_walked) {
          finishFolder(folder->input.get(), folder->output.get(), folder->tree->metadata, m_work);
          folder->input = Fd{};
          folder->output = Fd{};
        }
        m_walked.clear();
      }

    private:
      struct OpenFolder {
        Fd input;
        Fd output;
        const Tree* tree;
      };

      CopyPool& pool() {
        if (!m_work.copyPool) {
          m_work.copyPool = std::make_unique<CopyPool>(m_work);
        }
        return *m_work.copyPool;
      }

      CopyWork& m_work;
      // Stable addresses; finish() closes the descriptors.
      std::deque<OpenFolder> m_folders;
      // Walk-complete folders awaiting finish(), children before parents.
      std::vector<OpenFolder*> m_walked;
    };

    void copyInParallel(int from, int to, const std::string& target, Tree& tree, CopyWork& work) {
      // Outside the try block: its descriptors close only after abandon() has stopped every copy.
      ParallelCopy copy(work);
      try {
        copy.walk(from, to, target, tree, 0);
        copy.finish();
      } catch (...) {
        if (work.copyPool) {
          if (const auto error = work.copyPool->abandon()) {
            std::rethrow_exception(error);
          }
        }
        throw;
      }
    }
  } // namespace

  Tree copyTree(int from, const std::string& name, int to, const std::string& target, CopyWork& work, unsigned depth) {
    work.check();
    if (depth > 256) {
      throwTooDeep();
    }
    Tree tree{.name = name, .metadata = info(from, name), .children = {}};
    const auto& source = tree.metadata;
    // Paced destinations (removable media, network mounts) keep one file in flight.
    if (depth == 0 && S_ISDIR(source.st_mode) && work.paceLimit == 0) {
      copyInParallel(from, to, target, tree, work);
    } else if (S_ISREG(source.st_mode)) {
      copyFile(from, name, to, target, source, work, work.buffer);
    } else if (S_ISDIR(source.st_mode)) {
      auto input = directory(from, name);
      requireUnchanged(source, statFd(input.get()));
      auto output = makeFolder(to, target);
      for (const auto& child : children(input.get())) {
        tree.children.push_back(copyTree(input.get(), child, output.get(), child, work, depth + 1));
      }
      finishFolder(input.get(), output.get(), source, work);
    } else if (S_ISLNK(source.st_mode)) {
      copyLink(from, name, to, target, source, work);
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
      if ((value.st_mode & 0700) != 0700 && ::fchmod(fd.get(), 0700) != 0) {
        if (errno != EPERM && errno != EOPNOTSUPP && errno != ENOTSUP) {
          fail("Could not clean temporary folder");
        }
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
