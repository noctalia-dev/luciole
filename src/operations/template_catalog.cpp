#include "operations/template_catalog.h"

#include "fs/filename.h"
#include "fs/network_job.h"
#include "operations/network_types.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <glib.h>

luciole::TemplateRevision luciole::templateRevision(const struct stat& info) {
  if (!S_ISREG(info.st_mode) || info.st_size < 0) {
    throwFileError(FileErrorKind::InvalidArgument, "The template is not a regular file.");
  }
  return {
      static_cast<std::uint64_t>(info.st_dev),
      static_cast<std::uint64_t>(info.st_ino),
      static_cast<std::uint64_t>(info.st_size),
      info.st_mtim.tv_sec,
      info.st_mtim.tv_nsec,
      info.st_ctim.tv_sec,
      info.st_ctim.tv_nsec
  };
}
namespace {
  // Fills result in place so a failure still reports the templates read before it.
  void listTemplates(luciole::TemplateListing& result, const luciole::CancelToken& token) {
    using namespace luciole;
    const char* configured = g_get_user_special_dir(G_USER_DIRECTORY_TEMPLATES);
    if (!configured || !g_path_is_absolute(configured)) {
      throwFileError(FileErrorKind::Unavailable, "No Templates folder is configured.");
    }
    struct stat root{}, home{};
    if (::stat(configured, &root) != 0 || !S_ISDIR(root.st_mode)) {
      throwFileError(FileErrorKind::Unavailable, "The Templates folder is unavailable.");
    }
    if (::stat(g_get_home_dir(), &home) == 0 && root.st_dev == home.st_dev && root.st_ino == home.st_ino) {
      throwFileError(FileErrorKind::Unavailable, "The Templates folder is disabled.");
    }
    namespace fs = std::filesystem;
    const fs::path base(configured);
    std::error_code error;
    fs::recursive_directory_iterator it(base, error), end;
    std::size_t visited = 0;
    while (!error && it != end && !token.cancelled()) {
      if (++visited > 4096 || result.items.size() >= 256) {
        result.truncated = true;
        break;
      }
      const auto path = it->path();
      const auto name = path.filename().native();
      if (name.starts_with('.')) {
        it.disable_recursion_pending();
      } else {
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0) {
          throwFileError(errnoError(errno).kind, "Some templates could not be read.");
        }
        if (S_ISDIR(info.st_mode) && it.depth() >= 8) {
          it.disable_recursion_pending();
          result.truncated = true;
        } else if (S_ISREG(info.st_mode)) {
          result.items.push_back(
              {Location::fromPath(path.native()), name, filenameDisplayText(path.lexically_relative(base).native()),
               templateRevision(info)}
          );
        }
      }
      it.increment(error);
    }
    if (error) {
      result.error = FileError{
          .kind = errnoError(error.value()).kind, .message = "Some templates could not be read: " + error.message()
      };
    }
    std::ranges::sort(result.items, [](const auto& a, const auto& b) {
      const int order = g_utf8_collate(a.label.c_str(), b.label.c_str());
      return order != 0 ? order < 0 : a.location.uri() < b.location.uri();
    });
  }
} // namespace

luciole::TemplateListing luciole::readTemplates(const CancelToken& token) {
  TemplateListing result;
  try {
    if (!inNetworkWorker()) {
      const auto value = NetworkProcess::acquire(IoLane::NetworkOperation)->call({{"action", "templates"}}, token);
      return {
          value.at("items").get<std::vector<DocumentTemplate>>(), value.at("error").get<std::optional<FileError>>(),
          value.at("truncated").get<bool>()
      };
    }
    listTemplates(result, token);
  } catch (...) {
    result.error = currentFileError();
  }
  return result;
}

struct luciole::TemplateCatalog::State {
  CancelToken token;
  std::uint64_t revision = 0;
  bool active = true;
};
luciole::TemplateCatalog::TemplateCatalog() : m_state(std::make_shared<State>()) {}
luciole::TemplateCatalog::~TemplateCatalog() {
  m_state->active = false;
  cancel();
}
void luciole::TemplateCatalog::cancel() {
  ++m_state->revision;
  m_state->token.cancel();
  m_state->token = CancelToken{};
}
void luciole::TemplateCatalog::load(std::function<void(TemplateListing)> completed) {
  cancel();
  runIoJob<TemplateListing>(
      IoLane::NetworkOperation, [token = m_state->token] { return readTemplates(token); },
      [owner = m_state, revision = m_state->revision,
       completed = std::move(completed)](JobResult<TemplateListing> result) {
        if (owner->active && owner->revision == revision) {
          completed(result ? std::move(*result) : TemplateListing{.items = {}, .error = result.error()});
        }
      }
  );
}
