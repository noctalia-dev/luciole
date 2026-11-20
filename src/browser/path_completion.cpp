#include "browser/path_completion.h"

#include "fs/file_query.h"
#include "fs/gio_ptr.h"
#include "fs/network_info.h"
#include "fs/network_job.h"

#include <algorithm>
#include <exception>
#include <stdexcept>

struct luciole::PathCompletion::State {
  CancelToken token;
  std::uint64_t revision = 0;
  bool active = true;
};
luciole::PathCompletion::PathCompletion() : m_state(std::make_shared<State>()) {}
luciole::PathCompletion::~PathCompletion() {
  m_state->active = false;
  cancel();
}
void luciole::PathCompletion::cancel() {
  ++m_state->revision;
  m_state->token.cancel();
  m_state->token = CancelToken{};
}
void luciole::PathCompletion::complete(
    const std::string& text, const Location& base, bool showHidden, std::function<void(PathSuggestions)> completed
) {
  try {
    if (text.empty()) {
      cancel();
      completed({});
      return;
    }
    if (text.ends_with('/') || text == "~") {
      enumerate(resolveLocationText(text, base), {}, showHidden, std::move(completed));
      return;
    }
    const auto slash = text.find_last_of('/');
    const auto parent = slash == std::string::npos ? base : resolveLocationText(text.substr(0, slash + 1), base);
    std::string prefix = slash == std::string::npos ? text : text.substr(slash + 1);
    if (const GString scheme(g_uri_parse_scheme(text.c_str())); scheme) {
      const GString decoded(g_uri_unescape_string(prefix.c_str(), "/"));
      if (!decoded) {
        throw std::invalid_argument("Invalid URI filename prefix.");
      }
      prefix = decoded.get();
    }
    enumerate(parent, prefix, showHidden || prefix.starts_with('.'), std::move(completed));
  } catch (const std::exception& error) {
    cancel();
    completed({.items = {}, .error = error.what()});
  }
}
void luciole::PathCompletion::children(
    const Location& parent, bool showHidden, std::function<void(PathSuggestions)> completed
) {
  enumerate(parent, {}, showHidden, std::move(completed));
}
void luciole::PathCompletion::enumerate(
    const Location& parent, std::string prefix, bool showHidden, std::function<void(PathSuggestions)> completed
) {
  cancel();
  const CancelToken token = m_state->token;
  runIoJob<PathSuggestions>(
      ioLane(parent), token,
      [token, parent, prefix = std::move(prefix), showHidden] {
        constexpr std::size_t limit = 256;
        PathSuggestions result;
        const auto error = enumerateChildren(parent, token, [&](const ChildInfo& child) {
          if (child.type != FileType::Directory
              || (!showHidden && child.hidden)
              || !std::string_view(child.name).starts_with(prefix)) {
            return true;
          }
          if (result.items.size() == limit) {
            result.truncated = true;
            return false;
          }
          auto location = parent.child(child.name);
          auto text = locationEditText(location);
          if (!text.ends_with('/')) {
            text += '/';
          }
          result.items.push_back(
              {std::move(location), child.displayName.empty() ? child.name : child.displayName, std::move(text)}
          );
          return true;
        });
        if (error) {
          result.error = error->message;
        }
        std::ranges::sort(result.items, [](const auto& a, const auto& b) {
          return g_utf8_collate(a.label.c_str(), b.label.c_str()) < 0;
        });
        return result;
      },
      [state = m_state, revision = m_state->revision,
       completed = std::move(completed)](JobResult<PathSuggestions> result) mutable {
        if (state->active && state->revision == revision) {
          completed(result ? std::move(*result) : PathSuggestions{.items = {}, .error = result.error().message});
        }
      }
  );
}
