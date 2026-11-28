#include "fs/recent_files.h"

#include "fs/background_job.h"

#include <algorithm>
#include <chrono>
#include <gio/gio.h>
#include <utility>

namespace {
  using BookmarkFile = std::unique_ptr<GBookmarkFile, decltype(&g_bookmark_file_free)>;
  using DateTime = std::unique_ptr<GDateTime, decltype(&g_date_time_unref)>;

  BookmarkFile emptyList() { return {g_bookmark_file_new(), g_bookmark_file_free}; }

  std::string takeMessage(GError* error) {
    std::string message = error != nullptr ? error->message : "Unknown error.";
    g_clear_error(&error);
    return message;
  }

  // Other applications skip entries without a type, so a file of unknown type is recorded as plain data.
  void recordUse(GBookmarkFile* list, const luciole::RecentEdit& edit, GDateTime* now) {
    const char* uri = edit.uri.c_str();
    g_bookmark_file_add_application(list, uri, "Luciole", "luciole %u");
    g_bookmark_file_set_mime_type(
        list, uri, edit.contentType.empty() ? "application/octet-stream" : edit.contentType.c_str()
    );
    g_bookmark_file_set_modified_date_time(list, uri, now);
    g_bookmark_file_set_visited_date_time(list, uri, now);
  }

  // The list belongs to every desktop application, so it is rewritten the way GTK does: read it, then atomically
  // replace it, with no size or ownership limits of Luciole's own files. A list that cannot be read is only
  // replaced when the edits clear it.
  std::string
  rewrite(const std::filesystem::path& file, const std::vector<luciole::RecentEdit>& edits, std::int64_t now) {
    const bool clears =
        std::ranges::any_of(edits, [](const auto& edit) { return edit.kind == luciole::RecentEdit::Kind::Clear; });
    std::optional<std::string> current;
    gchar* text = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    if (g_file_get_contents(file.c_str(), &text, &length, &error) != FALSE) {
      current.emplace(text, length);
      g_free(text);
    } else if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT) && !clears) {
      return "The recent files list could not be read: " + takeMessage(error);
    } else {
      g_clear_error(&error);
    }
    const auto edited = luciole::editRecentList(current, edits, now);
    if (!edited) {
      return edited.error();
    }
    if (!*edited) {
      return {};
    }
    const auto& content = **edited;
    if (g_file_set_contents_full(
            file.c_str(), content.data(), static_cast<gssize>(content.size()),
            static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600,
            &error
        )
        == FALSE) {
      return "The recent files list could not be saved: " + takeMessage(error);
    }
    return {};
  }
} // namespace

std::expected<std::optional<std::string>, std::string> luciole::editRecentList(
    const std::optional<std::string>& current, const std::vector<RecentEdit>& edits, std::int64_t now
) {
  const bool clears = std::ranges::any_of(edits, [](const auto& edit) { return edit.kind == RecentEdit::Kind::Clear; });
  auto list = emptyList();
  bool changed = false;
  if (current && !current->empty()) {
    GError* error = nullptr;
    if (g_bookmark_file_load_from_data(list.get(), current->data(), current->size(), &error) == FALSE) {
      auto message = takeMessage(error);
      if (!clears) {
        return std::unexpected("The recent files list could not be read: " + message);
      }
      list = emptyList();
      changed = true;
    }
  }
  const DateTime stamp(g_date_time_new_from_unix_utc(now), g_date_time_unref);
  for (const auto& edit : edits) {
    switch (edit.kind) {
    case RecentEdit::Kind::Clear:
      changed = changed || g_bookmark_file_get_size(list.get()) > 0;
      list = emptyList();
      break;
    case RecentEdit::Kind::Forget:
      changed = g_bookmark_file_remove_item(list.get(), edit.uri.c_str(), nullptr) != FALSE || changed;
      break;
    case RecentEdit::Kind::Use:
      recordUse(list.get(), edit, stamp.get());
      changed = true;
      break;
    }
  }
  if (!changed) {
    return std::nullopt;
  }
  GError* error = nullptr;
  gsize length = 0;
  const std::unique_ptr<gchar, decltype(&g_free)> text(g_bookmark_file_to_data(list.get(), &length, &error), g_free);
  if (!text) {
    return std::unexpected("The recent files list could not be written: " + takeMessage(error));
  }
  return std::string(text.get(), length);
}

struct luciole::RecentFiles::State {
  std::filesystem::path file;
  std::vector<RecentEdit> queued;
  std::vector<Done> waiting;
  bool writing = false;

  // One batch at a time, so concurrent edits never replace each other's writes. A caller waiting on no edits still
  // gets its answer, after the writes before it.
  static void start(const std::shared_ptr<State>& state) {
    if (state->writing || (state->queued.empty() && state->waiting.empty())) {
      return;
    }
    state->writing = true;
    runJob<std::string>(
        [file = state->file, edits = std::exchange(state->queued, {})] {
          const auto now =
              std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count();
          return rewrite(file, edits, now);
        },
        [weak = std::weak_ptr(state), done = std::exchange(state->waiting, {})](JobResult<std::string> result) {
          const auto owner = weak.lock();
          if (!owner) {
            return;
          }
          const auto error = result ? *result : result.error().message;
          for (const auto& callback : done) {
            if (callback) {
              callback(error);
            }
          }
          owner->writing = false;
          start(owner);
        }
    );
  }
};

luciole::RecentFiles::RecentFiles(std::filesystem::path file) : m_state(std::make_shared<State>()) {
  m_state->file = std::move(file);
}
luciole::RecentFiles::~RecentFiles() = default;

std::filesystem::path luciole::RecentFiles::defaultFile() {
  return std::filesystem::path(g_get_user_data_dir()) / "recently-used.xbel";
}

void luciole::RecentFiles::recordUse(const Location& file, std::string contentType) {
  auto uri = file.nativeFileUri();
  if (!uri) {
    return;
  }
  if (contentType.empty()) {
    const auto name = file.basename().value_or("");
    const std::unique_ptr<gchar, decltype(&g_free)> guessed(
        g_content_type_guess(name.c_str(), nullptr, 0, nullptr), g_free
    );
    const std::unique_ptr<gchar, decltype(&g_free)> mime(
        guessed ? g_content_type_get_mime_type(guessed.get()) : nullptr, g_free
    );
    contentType = mime ? mime.get() : "";
  }
  submit({{.kind = RecentEdit::Kind::Use, .uri = std::move(*uri), .contentType = std::move(contentType)}}, {});
}

void luciole::RecentFiles::forget(const std::vector<Location>& files, Done done) {
  std::vector<RecentEdit> edits;
  edits.reserve(files.size());
  for (const auto& file : files) {
    edits.push_back({.kind = RecentEdit::Kind::Forget, .uri = file.uri(), .contentType = {}});
  }
  submit(std::move(edits), std::move(done));
}

void luciole::RecentFiles::clear(Done done) {
  submit({{.kind = RecentEdit::Kind::Clear, .uri = {}, .contentType = {}}}, std::move(done));
}

bool luciole::RecentFiles::busy() const {
  return m_state->writing || !m_state->queued.empty() || !m_state->waiting.empty();
}

void luciole::RecentFiles::submit(std::vector<RecentEdit> edits, Done done) {
  std::ranges::move(edits, std::back_inserter(m_state->queued));
  if (done) {
    m_state->waiting.push_back(std::move(done));
  }
  State::start(m_state);
}
