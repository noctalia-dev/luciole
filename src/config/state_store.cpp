#include "config/state_store.h"

#include "config/toml_decoder.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <format>
#include <glib.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {
  constexpr std::size_t kMaxWindows = 16;
  constexpr std::size_t kMaxTabs = 64;
  constexpr std::size_t kMaxUriBytes = 64 * 1024;

  std::optional<std::string> uriProblem(const std::string& uri) {
    if (uri.size() > kMaxUriBytes || uri.contains('\0')) {
      return "expected a URI of at most 64 KiB without NUL";
    }
    constexpr auto flags =
        static_cast<GUriFlags>(G_URI_FLAGS_ENCODED | G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_HAS_AUTH_PARAMS);
    const std::unique_ptr<GUri, decltype(&g_uri_unref)> parsed(g_uri_parse(uri.c_str(), flags, nullptr), g_uri_unref);
    if (!parsed) {
      return "expected an absolute URI";
    }
    if (g_uri_get_password(parsed.get()) != nullptr
        || g_uri_get_auth_params(parsed.get()) != nullptr
        || g_uri_get_query(parsed.get()) != nullptr
        || g_uri_get_fragment(parsed.get()) != nullptr) {
      return "stored locations cannot contain passwords, authentication parameters, queries or fragments";
    }
    return std::nullopt;
  }

  luciole::ConfigIssue problem(const std::filesystem::path& path, std::string key, std::string message) {
    return {.file = path, .key = std::move(key), .message = std::move(message)};
  }

  luciole::ConfigIssues validate(const luciole::SessionState& state, const std::filesystem::path& path) {
    luciole::ConfigIssues issues;
    const auto add = [&](std::string key, std::string message) {
      issues.push_back(problem(path, std::move(key), std::move(message)));
    };
    const auto location = [&](const luciole::Location& value, const std::string& key) {
      if (const auto error = uriProblem(value.uri())) {
        add(key, *error);
      }
    };
    if (state.bookmarks.size() > luciole::StateStore::placesLimit) {
      add("bookmarks", "at most 268 Places rows are supported");
    }
    std::unordered_set<std::string> uris;
    for (std::size_t i = 0; i < state.bookmarks.size(); ++i) {
      const auto& bookmark = state.bookmarks[i];
      const auto key = std::format("bookmarks[{}]", i);
      if (const auto error = luciole::bookmarkLabelProblem(bookmark.label)) {
        add(key + ".label", *error);
      }
      location(bookmark.location, key + ".uri");
      if (!uris.insert(bookmark.location.uri()).second) {
        add(key + ".uri", "duplicate bookmark location");
      }
    }
    if (state.windows.size() > kMaxWindows) {
      add("windows", "at most 16 windows are supported");
    }
    for (std::size_t i = 0; i < state.windows.size(); ++i) {
      const auto& window = state.windows[i];
      const auto key = std::format("windows[{}]", i);
      if (window.tabs.empty() || window.tabs.size() > kMaxTabs) {
        add(key + ".tabs", "expected between 1 and 64 tabs");
      }
      if (window.activeTab >= window.tabs.size()) {
        add(key + ".active_tab", "active tab is outside the saved tab list");
      }
      for (std::size_t j = 0; j < window.tabs.size(); ++j) {
        const auto& tab = window.tabs[j];
        const auto tabKey = std::format("{}.tabs[{}]", key, j);
        location(tab.primary, tabKey + ".primary_uri");
        if (tab.secondary) {
          location(*tab.secondary, tabKey + ".secondary_uri");
        }
        if ((tab.activePane != luciole::ActivePane::Primary && tab.activePane != luciole::ActivePane::Secondary)
            || (tab.activePane == luciole::ActivePane::Secondary && !tab.secondary)) {
          add(tabKey + ".active_pane", "active pane must exist and be primary or secondary");
        }
      }
    }
    if (state.chooserFolder) {
      location(*state.chooserFolder, "chooser_folder_uri");
    }
    return issues;
  }

  // Shape checks retain source positions; cross-record checks retain key paths.
  class Decoder : public luciole::TomlDecoder {
  public:
    using TomlDecoder::TomlDecoder;

    std::optional<luciole::Location>
    location(const toml::table& table, std::string_view field, const std::string& key) {
      const auto uri = text(table, field, key);
      if (!uri) {
        return std::nullopt;
      }
      if (const auto error = uriProblem(*uri)) {
        issue(*table.get(field), key, *error);
        return std::nullopt;
      }
      try {
        return luciole::Location::fromUri(*uri);
      } catch (const std::invalid_argument&) {
        issue(*table.get(field), key, "expected an absolute operation URI");
        return std::nullopt;
      }
    }

    std::optional<luciole::SavedTab> tab(const toml::node& node, const std::string& key) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        issue(node, key, "expected a tab table");
        return std::nullopt;
      }
      unknownKeys(*table, key + ".", {"primary_uri", "secondary_uri", "active_pane"}, "unknown state field");
      auto primary = location(*table, "primary_uri", key + ".primary_uri");
      std::optional<luciole::Location> secondary;
      if (table->contains("secondary_uri")) {
        secondary = location(*table, "secondary_uri", key + ".secondary_uri");
      }
      const auto active = text(*table, "active_pane", key + ".active_pane");
      if (active && *active != "primary" && *active != "secondary") {
        issue(*table->get("active_pane"), key + ".active_pane", "expected 'primary' or 'secondary'");
      }
      if (!primary || !active) {
        return std::nullopt;
      }
      return luciole::SavedTab{
          .primary = std::move(*primary),
          .secondary = std::move(secondary),
          .activePane = *active == "secondary" ? luciole::ActivePane::Secondary : luciole::ActivePane::Primary
      };
    }

    std::optional<luciole::SavedWindow> window(const toml::node& node, const std::string& key) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        issue(node, key, "expected a window table");
        return std::nullopt;
      }
      unknownKeys(*table, key + ".", {"tabs", "active_tab"}, "unknown state field");
      luciole::SavedWindow result;
      const auto* active = table->get("active_tab");
      if (active == nullptr
          || !active->is_integer()
          || active->as_integer()->get() < 0
          || active->as_integer()->get() >= static_cast<std::int64_t>(kMaxTabs)) {
        issue(active != nullptr ? *active : node, key + ".active_tab", "expected an integer from 0 to 63");
      } else {
        result.activeTab = static_cast<std::size_t>(active->as_integer()->get());
      }
      if (const auto* tabs = array(*table, "tabs", key + ".tabs", kMaxTabs, true)) {
        for (std::size_t i = 0; i < tabs->size(); ++i) {
          if (auto value = tab(*tabs->get(i), std::format("{}.tabs[{}]", key, i))) {
            result.tabs.push_back(std::move(*value));
          }
        }
      }
      return result;
    }
  };

  std::expected<luciole::SessionState, luciole::ConfigIssues>
  parseState(const std::optional<std::string>& text, const std::filesystem::path& path) {
    if (!text) {
      return luciole::SessionState{};
    }
    auto table = luciole::parseToml(*text, path);
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    Decoder decoder(path);
    decoder.unknownKeys(
        *table, "",
        {"schema_version", "places_customized", "bookmarks", "place_separators", "windows", "chooser_folder_uri"},
        "unknown state field"
    );
    const auto* version = table->get("schema_version");
    if (version == nullptr || !luciole::isSchemaVersion(*version)) {
      decoder.issue(
          version != nullptr ? *version : *table, "schema_version",
          std::format("expected integer state schema version {}", luciole::kSchemaVersion)
      );
    }
    luciole::SessionState state;
    if (const auto* customized = table->get("places_customized")) {
      if (!customized->is_boolean()) {
        decoder.issue(*customized, "places_customized", "expected a boolean");
      } else {
        state.placesCustomized = customized->as_boolean()->get();
      }
    }
    if (const auto* bookmarks =
            decoder.array(*table, "bookmarks", "bookmarks", luciole::StateStore::placesLimit, state.placesCustomized)) {
      for (std::size_t i = 0; i < bookmarks->size(); ++i) {
        const auto key = std::format("bookmarks[{}]", i);
        const auto* bookmark = bookmarks->get(i)->as_table();
        if (bookmark == nullptr) {
          decoder.issue(*bookmarks->get(i), key, "expected a bookmark table");
          continue;
        }
        decoder.unknownKeys(*bookmark, key + ".", {"label", "uri", "icon"}, "unknown state field");
        auto label = decoder.text(*bookmark, "label", key + ".label");
        auto location = decoder.location(*bookmark, "uri", key + ".uri");
        auto icon =
            bookmark->contains("icon") ? decoder.text(*bookmark, "icon", key + ".icon") : std::optional<std::string>{};
        if (label && location) {
          state.bookmarks.push_back(
              {.label = std::move(*label),
               .location = std::move(*location),
               .iconName = icon ? std::move(*icon) : std::string{}}
          );
        }
      }
    }
    if (const auto* separators =
            decoder.array(*table, "place_separators", "place_separators", luciole::StateStore::placesLimit, false)) {
      for (std::size_t i = 0; i < separators->size(); ++i) {
        const auto* value = separators->get(i);
        if (!value->is_integer() || value->as_integer()->get() <= 0) {
          decoder.issue(*value, std::format("place_separators[{}]", i), "expected a positive row number");
          continue;
        }
        const auto row = static_cast<std::size_t>(value->as_integer()->get());
        if (state.placeSeparators.empty() || state.placeSeparators.back() < row) {
          state.placeSeparators.push_back(row);
        }
      }
    }
    if (const auto* windows = decoder.array(*table, "windows", "windows", kMaxWindows, false)) {
      for (std::size_t i = 0; i < windows->size(); ++i) {
        if (auto window = decoder.window(*windows->get(i), std::format("windows[{}]", i))) {
          state.windows.push_back(std::move(*window));
        }
      }
    }
    if (table->contains("chooser_folder_uri")) {
      state.chooserFolder = decoder.location(*table, "chooser_folder_uri", "chooser_folder_uri");
    }
    if (!decoder.issues.empty()) {
      return std::unexpected(std::move(decoder.issues));
    }
    auto issues = validate(state, path);
    return issues.empty() ? std::expected<luciole::SessionState, luciole::ConfigIssues>(std::move(state))
                          : std::unexpected(std::move(issues));
  }

  std::string formatState(const luciole::SessionState& state) {
    toml::array bookmarks;
    for (const auto& bookmark : state.bookmarks) {
      toml::table row{{"label", bookmark.label}, {"uri", bookmark.location.uri()}};
      if (!bookmark.iconName.empty()) {
        row.insert("icon", bookmark.iconName);
      }
      bookmarks.push_back(std::move(row));
    }
    toml::array windows;
    for (const auto& window : state.windows) {
      toml::array tabs;
      for (const auto& tab : window.tabs) {
        toml::table value{
            {"primary_uri", tab.primary.uri()},
            {"active_pane", tab.activePane == luciole::ActivePane::Primary ? "primary" : "secondary"}
        };
        if (tab.secondary) {
          value.insert("secondary_uri", tab.secondary->uri());
        }
        tabs.push_back(std::move(value));
      }
      windows.push_back(
          toml::table{{"active_tab", static_cast<std::int64_t>(window.activeTab)}, {"tabs", std::move(tabs)}}
      );
    }
    toml::table document{
        {"schema_version", luciole::kSchemaVersion},
        {"places_customized", state.placesCustomized},
        {"bookmarks", std::move(bookmarks)},
        {"windows", std::move(windows)}
    };
    if (!state.placeSeparators.empty()) {
      toml::array separators;
      for (const auto row : state.placeSeparators) {
        separators.push_back(static_cast<std::int64_t>(row));
      }
      document.insert("place_separators", std::move(separators));
    }
    if (state.chooserFolder) {
      document.insert("chooser_folder_uri", state.chooserFolder->uri());
    }
    // Adapted from Noctalia StateStore's canonical TOML formatting.
    std::ostringstream output;
    output << toml::toml_formatter{
        document, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    auto text = output.str();
    if (!text.ends_with('\n')) {
      text.push_back('\n');
    }
    return text;
  }
  // The Places section: what saveBookmarks() writes.
  bool samePlaces(const luciole::SessionState& a, const luciole::SessionState& b) {
    return a.bookmarks == b.bookmarks
        && a.placesCustomized == b.placesCustomized
        && a.placeSeparators == b.placeSeparators;
  }
} // namespace

std::optional<std::string> luciole::storedUriProblem(const std::string& uri) { return uriProblem(uri); }

std::optional<std::string> luciole::bookmarkLabelProblem(std::string_view label) {
  if (label.empty()) {
    return "Enter a label.";
  }
  if (label.size() > 256) {
    return "Labels can contain at most 256 UTF-8 bytes.";
  }
  if (!g_utf8_validate(label.data(), static_cast<gssize>(label.size()), nullptr)) {
    return "Labels must be valid UTF-8 text.";
  }
  if (std::ranges::any_of(label, [](unsigned char c) { return c < 32 || c == 127; })) {
    return "Labels cannot contain control characters.";
  }
  return std::nullopt;
}

luciole::StateStore::StateStore(std::filesystem::path path) : m_path(std::move(path)) {}

std::filesystem::path luciole::StateStore::pathFromEnvironment() { return stateHome() / "luciole/state.toml"; }

bool luciole::StateStore::validPath() const {
  return m_path.is_absolute() && !m_path.native().contains('\0') && m_path.filename() == "state.toml";
}

bool luciole::StateStore::load() {
  m_issues.clear();
  if (!validPath()) {
    m_issues.push_back(problem(m_path, {}, "expected an absolute state.toml path"));
    return false;
  }
  auto text = readPrivateTextFile(m_path);
  if (!text) {
    m_issues.push_back(problem(m_path, {}, std::move(text.error())));
    return false;
  }
  auto parsed = parseState(*text, m_path);
  if (!parsed) {
    m_issues = std::move(parsed.error());
    return false;
  }
  m_state = std::move(*parsed);
  m_loaded = true;
  return true;
}

luciole::AtomicWriteState
luciole::StateStore::saveBookmarks(std::vector<Bookmark> bookmarks, std::vector<std::size_t> separators) {
  auto proposed = m_state;
  proposed.bookmarks = std::move(bookmarks);
  proposed.placeSeparators = std::move(separators);
  proposed.placesCustomized = true;
  return save(std::move(proposed), Section::Bookmarks);
}

luciole::AtomicWriteState luciole::StateStore::saveWindows(std::vector<SavedWindow> windows) {
  auto proposed = m_state;
  proposed.windows = std::move(windows);
  return save(std::move(proposed), Section::Windows);
}

luciole::AtomicWriteState luciole::StateStore::saveChooserFolder(Location folder) {
  auto proposed = m_state;
  proposed.chooserFolder = std::move(folder);
  return save(std::move(proposed), Section::ChooserFolder);
}

luciole::AtomicWriteState luciole::StateStore::save(SessionState proposed, Section section) {
  m_issues.clear();
  if (!m_loaded || !validPath()) {
    m_issues.push_back(problem(m_path, {}, "load a valid state snapshot before saving"));
    return AtomicWriteState::NotCommitted;
  }
  m_issues = validate(proposed, m_path);
  if (!m_issues.empty()) {
    return AtomicWriteState::NotCommitted;
  }
  SessionState candidate;
  const auto result = updateTextFileAtomic(
      m_path, [&](const std::optional<std::string>& text) -> std::expected<std::optional<std::string>, std::string> {
        auto current = parseState(text, m_path);
        if (!current) {
          m_issues = std::move(current.error());
          return std::unexpected("invalid existing state; repair or remove it explicitly before saving");
        }
        bool unchanged = false;
        bool matches = true; // The last picked folder simply wins over another writer's.
        switch (section) {
        case Section::Bookmarks:
          unchanged = samePlaces(*current, proposed);
          matches = samePlaces(*current, m_state);
          break;
        case Section::Windows:
          unchanged = current->windows == proposed.windows;
          matches = current->windows == m_state.windows;
          break;
        case Section::ChooserFolder:
          unchanged = current->chooserFolder == proposed.chooserFolder;
          break;
        }
        if (!unchanged && !matches) {
          return std::unexpected("state section changed in another writer; reload before retrying");
        }
        candidate = std::move(*current);
        switch (section) {
        case Section::Bookmarks:
          candidate.bookmarks = std::move(proposed.bookmarks);
          candidate.placesCustomized = proposed.placesCustomized;
          candidate.placeSeparators = std::move(proposed.placeSeparators);
          break;
        case Section::Windows:
          candidate.windows = std::move(proposed.windows);
          break;
        case Section::ChooserFolder:
          candidate.chooserFolder = std::move(proposed.chooserFolder);
          break;
        }
        if (unchanged) {
          return std::nullopt;
        }
        return formatState(candidate);
      }
  );
  if (result.state != AtomicWriteState::NotCommitted) {
    m_state = std::move(candidate);
  }
  if (!result.error.empty() && m_issues.empty()) {
    m_issues.push_back(problem(m_path, {}, result.error));
  }
  return result.state;
}
