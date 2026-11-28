#pragma once

#include "operations/archive_input.h"

namespace luciole::archive_work {
  // Header-encrypted (rar -hp) inputs are listed and unpacked by the 7-Zip provider.
  enum class RarInput { None, Rar4, Rar5, Rar4Encrypted, Rar5Encrypted };
  // Worker-only envelope check. Do not enable an SFX or multipart RAR reader
  // without this check: a first volume can contain complete readable members.
  RarInput validateRarInput(Input& input);
  // Validate the complete RAR5 end marker and any preceding trailing service blocks.
  void validateRar5End(Input& input, std::int64_t offset, Work& work);
  // Some libarchive versions skip the final CRC check for stored legacy members.
  void verifyRar4Stored(Input& input, std::int64_t offset, Work& work);
} // namespace luciole::archive_work
