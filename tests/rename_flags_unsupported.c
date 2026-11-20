// Preloaded into test runs to behave like NFS and FUSE filesystems that reject renameat2 flags: a flagged
// rename within one mount fails with EINVAL, a cross-mount one still fails with EXDEV first, as in the kernel.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int flaggedRenameError(int fromDir, int toDir) {
  struct statx from = {0};
  struct statx to = {0};
  if (statx(fromDir, "", AT_EMPTY_PATH, STATX_MNT_ID, &from) == 0
      && statx(toDir, "", AT_EMPTY_PATH, STATX_MNT_ID, &to) == 0
      && from.stx_mnt_id != to.stx_mnt_id) {
    return EXDEV;
  }
  return EINVAL;
}

long syscall(long number, ...) {
  va_list list;
  va_start(list, number);
  long args[6];
  for (int i = 0; i < 6; ++i) {
    args[i] = va_arg(list, long);
  }
  va_end(list);
  if (number == SYS_renameat2 && args[4] != 0) {
    errno = flaggedRenameError((int)args[0], (int)args[2]);
    return -1;
  }
  static long (*real)(long, ...);
  if (real == NULL) {
    *(void**)&real = dlsym(RTLD_NEXT, "syscall");
  }
  return real(number, args[0], args[1], args[2], args[3], args[4], args[5]);
}

int renameat2(int fromDir, const char* from, int toDir, const char* to, unsigned int flags) {
  if (flags != 0) {
    errno = flaggedRenameError(fromDir, toDir);
    return -1;
  }
  return renameat(fromDir, from, toDir, to);
}
