#include "config/folder_view_store.h"

#include "config/settings.h"
#include "config/state_store.h"
#include "config/toml_decoder.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <format>
#include <sstream>
#include <utility>

namespace {
  constexpr std::string_view kFileName = "folder_views.toml";

  class Decoder : public luciole::TomlDecoder {
  public:
    using TomlDecoder::TomlDecoder;

    std::optional<bool> flag(const toml::table& table, std::string_view field, const std::string& key, bool fallback) {
      const auto* node = table.get(field);
      if (node == nullptr) {
        return fallback;
      }
      if (!node->is_boolean()) {
        issue(*node, key, "expected a boolean");
        return std::nullopt;
      }
      return node->as_boolean()->get();
    }

    std::optional<luciole::FolderViewEntry> entry(const toml::node& node, const std::string& key) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        issue(node, key, "expected a folder table");
        return std::nullopt;
      }
      unknownKeys(
          *table, key + ".", {"uri", "sort_by", "sort_descending", "directories_first", "hidden_last", "view_mode"},
          "unknown folder view field"
      );
      const auto before = issues.size();
      luciole::FolderViewEntry result;
      if (auto uri = text(*table, "uri", key + ".uri")) {
        if (const auto error = luciole::storedUriProblem(*uri)) {
          issue(*table->get("uri"), key + ".uri", *error);
        }
        result.uri = std::move(*uri);
      }
      if (const auto* field = table->get("sort_by")) {
        const auto* name = field->as_string();
        const auto sortField = name != nullptr ? luciole::sortFieldFromName(name->get()) : std::nullopt;
        if (!sortField) {
          issue(*field, key + ".sort_by", "expected a sort field name such as 'name' or 'modified'");
        }
        const luciole::SortOptions defaults;
        const auto descending = flag(*table, "sort_descending", key + ".sort_descending", defaults.descending);
        const auto first = flag(*table, "directories_first", key + ".directories_first", defaults.directoriesFirst);
        const auto hidden = flag(*table, "hidden_last", key + ".hidden_last", defaults.hiddenLast);
        if (sortField && descending && first && hidden) {
          result.view.sort = luciole::SortOptions{
              .field = *sortField, .descending = *descending, .directoriesFirst = *first, .hiddenLast = *hidden
          };
        }
      } else if (
          table->contains("sort_descending") || table->contains("directories_first") || table->contains("hidden_last")
      ) {
        issue(*table, key + ".sort_by", "sort options need sort_by");
      }
      if (const auto* field = table->get("view_mode")) {
        const auto* name = field->as_string();
        const auto mode = name != nullptr ? luciole::viewModeFromName(name->get()) : std::nullopt;
        if (mode != luciole::ViewMode::List && mode != luciole::ViewMode::Grid) {
          issue(*field, key + ".view_mode", "expected 'list' or 'grid'");
        } else {
          result.view.mode = mode;
        }
      }
      if (issues.size() != before || result.view.empty()) {
        return std::nullopt;
      }
      return result;
    }
  };

  std::string format(const std::vector<luciole::FolderViewEntry>& entries) {
    toml::array folders;
    for (const auto& [uri, view] : entries) {
      toml::table row{{"uri", uri}};
      if (view.sort) {
        row.insert("sort_by", luciole::sortFieldName(view.sort->field));
        row.insert("sort_descending", view.sort->descending);
        row.insert("directories_first", view.sort->directoriesFirst);
        row.insert("hidden_last", view.sort->hiddenLast);
      }
      if (view.mode) {
        row.insert("view_mode", luciole::viewModeName(*view.mode));
      }
      folders.push_back(std::move(row));
    }
    const toml::table document{{"schema_version", luciole::kSchemaVersion}, {"folders", std::move(folders)}};
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
} // namespace

luciole::FolderViewStore::FolderViewStore(std::filesystem::path path) : m_path(std::move(path)) {}

std::filesystem::path luciole::FolderViewStore::pathFromEnvironment() { return stateHome() / "luciole" / kFileName; }

void luciole::FolderViewStore::load() {
  m_issues.clear();
  m_entries.clear();
  const auto text = readPrivateTextFile(m_path);
  if (!text) {
    m_issues.push_back({.file = m_path, .line = 0, .column = 0, .key = {}, .message = text.error()});
    return;
  }
  if (!*text) {
    return;
  }
  auto table = parseToml(**text, m_path);
  if (!table) {
    m_issues = std::move(table.error());
    return;
  }
  Decoder decoder(m_path);
  decoder.unknownKeys(*table, "", {"schema_version", "folders"}, "unknown folder view field");
  const auto* version = table->get("schema_version");
  if (version == nullptr || !isSchemaVersion(*version)) {
    decoder.issue(
        version != nullptr ? *version : *table, "schema_version",
        std::format("expected integer schema version {}", kSchemaVersion)
    );
    m_issues = std::move(decoder.issues);
    return;
  }
  if (const auto* folders = decoder.array(*table, "folders", "folders", limit, false)) {
    for (std::size_t index = 0; index < folders->size(); ++index) {
      if (auto entry = decoder.entry(*folders->get(index), std::format("folders[{}]", index))) {
        std::erase_if(m_entries, [&](const auto& previous) { return previous.uri == entry->uri; });
        m_entries.push_back(std::move(*entry));
      }
    }
  }
  m_issues = std::move(decoder.issues);
}

luciole::AtomicWriteState luciole::FolderViewStore::save(std::vector<FolderViewEntry> entries) {
  m_issues.clear();
  std::erase_if(entries, [](const auto& entry) { return entry.view.empty() || storedUriProblem(entry.uri); });
  if (entries.size() > limit) {
    entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(limit));
  }
  auto text = format(entries);
  // Long folder URIs can outgrow the file before the count does: drop the oldest until it fits.
  while (text.size() > maxBytes && !entries.empty()) {
    const auto perEntry = std::max<std::size_t>(1, text.size() / entries.size());
    const auto excess = std::min(entries.size(), (text.size() - maxBytes) / perEntry + 1);
    entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(excess));
    text = format(entries);
  }
  const auto result = updateTextFileAtomic(
      m_path, [&](const std::optional<std::string>& current) -> std::expected<std::optional<std::string>, std::string> {
        if (current == text) {
          return std::nullopt;
        }
        return std::move(text);
      }
  );
  if (result.state != AtomicWriteState::NotCommitted) {
    m_entries = std::move(entries);
  }
  if (!result.error.empty()) {
    m_issues.push_back({.file = m_path, .line = 0, .column = 0, .key = {}, .message = result.error});
  }
  return result.state;
}
