#pragma once

#include "operations/archive_input.h"

namespace luciole::archive_work {
  // Encrypted 7z and RAR go through the system 7-Zip CLI (7zz, 7z or 7za), found at runtime.
  // type is the 7-Zip -t value: "7z", "rar" or "rar5".
  struct SevenZipMember {
    std::string path;
    std::uint64_t size;
    unsigned mode;
    bool directory, encrypted;
  };
  std::vector<SevenZipMember>
  sevenZipCatalog(Input& input, Work& work, const Passwords& passwords, std::string_view type);
  // One provider run per password attempt, unpacking the named members (every member when
  // names is empty) below the empty private directory. Callers verify each produced file.
  void extractSevenZip(
      Input& input, Work& work, const Passwords& passwords, const std::vector<std::string>& names, bool encrypted,
      std::uint64_t largest, const std::string& directory, std::string_view type
  );
} // namespace luciole::archive_work
