#include "fs/heap_trim.h"

#include <glib.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

void luciole::scheduleHeapTrim() {
  static guint pending = 0;
  if (pending != 0) {
    return;
  }
  pending = g_idle_add_full(
      G_PRIORITY_LOW,
      [](gpointer) -> gboolean {
        pending = 0;
#ifdef __GLIBC__
        malloc_trim(0);
#endif
        return G_SOURCE_REMOVE;
      },
      nullptr, nullptr
  );
}
