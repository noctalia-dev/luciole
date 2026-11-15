#pragma once

namespace luciole {
  // Returns freed heap pages to the system once the main context is idle. Repeated calls before that idle run
  // coalesce. Call after releasing a large allocation burst, such as a replaced folder listing. Main context only.
  void scheduleHeapTrim();
} // namespace luciole
