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
  // Raises the soft open-file limit to the hard limit at startup. The 1024 default exists for select(), which
  // Luciole never uses; undo history for large transfers needs several descriptors per item.
  void raiseFileDescriptorLimit();
} // namespace luciole
