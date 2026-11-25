#pragma once

#include <sys/stat.h>

namespace luciole {
  // Owned POSIX descriptor; -1 is empty.
  class Fd {
  public:
    Fd() = default;
    explicit Fd(int fd) : m_fd(fd) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    // Closes now and returns close()'s result, so callers can check deferred write errors.
    int close();

  private:
    int m_fd = -1;
  };

  // Takes ownership of an open()/openat() result; a negative result throws errno as a FileErrorException.
  Fd checkedFd(int fd);
  // fstat; throws errno as a FileErrorException.
  struct stat statFd(int fd);
  // Distinguishes bind mounts even when their device and inode identities match.
  // Throws Unsupported if the kernel cannot report mount IDs; optional history callers may decline capture.
  bool sameMount(int first, int second);
  // Same inode and file type.
  bool sameIdentity(const struct stat& a, const struct stat& b);
  // Rename history ignores access time; metadata/content changes invalidate a captured revision.
  bool sameFileRevision(const struct stat& a, const struct stat& b);
  // A completed rename changes ctime; an atomic directory exchange can also change mtime (XFS).
  // Use only immediately after our own successful syscall, before adopting the new revision.
  bool sameRenamedRevision(const struct stat& before, const struct stat& after, bool exchanged = false);
  // fstat of an item pinned before it was unlinked from `parent`. NFS turns unlinking an open file into a rename to
  // `.nfsXXXX`, so the pinned inode keeps its link count, and a removed folder's handle goes stale; report what a
  // local filesystem would instead.
  struct stat statUnlinked(int parent, int pinned, const struct stat& before);

  // renameat2 with RENAME_NOREPLACE. Where the filesystem rejects the flag (NFS, many FUSE filesystems), checks
  // that `to` is vacant and renames, which is not atomic. Returns 0, or -1 with errno set.
  int renameNoReplace(int fromDir, const char* from, int toDir, const char* to);
  // renameat2 with RENAME_EXCHANGE. Where the filesystem rejects the flag, swaps through a temporary name beside
  // `from` and restores both names on failure; `to` is briefly absent. Returns 0, or -1 with errno set.
  int renameExchange(int fromDir, const char* from, int toDir, const char* to);

  // Raises the soft open-file limit to the hard limit at startup. The 1024 default exists for select(), which
  // Luciole never uses; undo history for large transfers needs several descriptors per item.
  void raiseFileDescriptorLimit();
} // namespace luciole
