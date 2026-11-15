#include "fs/interned_text.h"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
  };

  struct Pool {
    std::mutex mutex;
    // Node-based: element addresses stay valid across rehashing.
    std::unordered_set<std::string, Hash, std::equal_to<>> texts;
  };

  Pool& pool() {
    static auto* instance = new Pool(); // Never destroyed: entries may outlive static destruction order.
    return *instance;
  }
} // namespace

luciole::InternedText::InternedText(const char* text) {
  if (text != nullptr) {
    *this = InternedText(std::string_view(text));
  }
}

luciole::InternedText::InternedText(std::string_view text) {
  auto& shared = pool();
  const std::scoped_lock lock(shared.mutex);
  auto found = shared.texts.find(text);
  if (found == shared.texts.end()) {
    found = shared.texts.emplace(text).first;
  }
  m_text = &*found;
}
