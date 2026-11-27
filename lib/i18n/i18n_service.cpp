#include "i18n/i18n_service.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "i18n/language_tag.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace i18n {

  namespace {

    constexpr Logger kLog("i18n");

    struct PluralRule {
      std::string suffix;
      std::uint64_t modulus = 0;
      std::uint64_t first = 0;
      std::uint64_t last = std::numeric_limits<std::uint64_t>::max();

      bool matches(std::uint64_t count) const {
        const auto value = modulus ? count % modulus : count;
        return first <= value && value <= last;
      }
    };

    std::vector<PluralRule> parsePluralRules(const nlohmann::json& json) {
      if (!json.is_array() || json.empty()) {
        throw std::runtime_error("pluralRules must be a nonempty array");
      }
      std::vector<PluralRule> rules;
      for (std::size_t i = 0; i < json.size(); ++i) {
        const auto& entry = json[i];
        PluralRule rule{.suffix = entry.at("suffix").get<std::string>()};
        const bool conditional = entry.contains("range");
        if (conditional != (i + 1 < json.size())) {
          throw std::runtime_error("only the last plural rule must be unconditional");
        }
        if (conditional) {
          const auto& range = entry.at("range");
          if (!range.is_array()
              || range.size() != 2
              || !range[0].is_number_unsigned()
              || !range[1].is_number_unsigned()) {
            throw std::runtime_error("plural range must contain two unsigned integers");
          }
          rule.first = range[0].get<std::uint64_t>();
          rule.last = range[1].get<std::uint64_t>();
          if (rule.first > rule.last) {
            throw std::runtime_error("plural range endpoints are reversed");
          }
        }
        if (entry.contains("mod")) {
          const auto& mod = entry.at("mod");
          if (!conditional || !mod.is_number_unsigned() || mod.get<std::uint64_t>() == 0) {
            throw std::runtime_error("plural mod requires a range and a positive integer");
          }
          rule.modulus = mod.get<std::uint64_t>();
        }
        if (entry.size() != 1U + conditional + entry.contains("mod")) {
          throw std::runtime_error("unexpected property in plural rule");
        }
        rules.push_back(std::move(rule));
      }
      return rules;
    }

    void flatten(const nlohmann::json& node, const std::string& prefix, Catalog& out) {
      for (auto it = node.begin(); it != node.end(); ++it) {
        std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) {
          flatten(*it, path, out);
        } else if (it->is_string()) {
          out.insert_or_assign(std::move(path), it->get<std::string>());
        }
      }
    }

    // Identity keys can be looked up by their English text. Message IDs in
    // rename and operations are addressed by their full dotted keys.
    void buildTextIndex(const nlohmann::json& root, TextIndex& out) {
      for (const auto& [section, node] : root.items()) {
        if (section == "rename" || section == "operations" || !node.is_object()) {
          continue;
        }
        for (const auto& [key, value] : node.items()) {
          if (value.is_string()) {
            out.emplace(key, value.get<std::string>());
          }
        }
      }
    }

    // Reads $LC_ALL / $LC_MESSAGES / $LANG. Returns empty string if unset.
    std::string detectSystemLanguage() {
      for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(var);
        if (value != nullptr && value[0] != '\0') {
          return detail::normalizeLanguageTag(value);
        }
      }
      return {};
    }

    std::string resolveRequestedLanguage(std::string_view preferredLang) {
      return preferredLang.empty() ? detectSystemLanguage() : detail::normalizeLanguageTag(preferredLang);
    }

  } // namespace

  struct Translations::Loaded {
    Catalog catalog;
    TextIndex text;
    // Catalogs predating pluralRules used the base key for one and -plural for other counts.
    std::vector<PluralRule> rules{{.suffix = "", .first = 1, .last = 1}, {.suffix = "-plural"}};

    std::string_view lookup(std::string_view key) const {
      if (const auto it = catalog.find(key); it != catalog.end()) {
        return it->second;
      }
      const auto it = text.find(key);
      return it != text.end() ? std::string_view(it->second) : std::string_view{};
    }

    std::string_view lookupPlural(std::string_view key, std::uint64_t count) const {
      for (const auto& rule : rules) {
        if (rule.matches(count)) {
          return rule.suffix.empty() ? lookup(key) : lookup(std::string(key) + rule.suffix);
        }
      }
      return {};
    }
  };

  Service& Service::instance() {
    static Service s_instance;
    return s_instance;
  }

  std::shared_ptr<const Translations::Loaded> Service::loadCatalog(std::string_view lang) const {
    const std::filesystem::path path = paths::assetPath("translations/" + std::string(lang) + ".json");
    std::ifstream file(path);
    if (!file.is_open()) {
      return nullptr;
    }
    try {
      auto json = nlohmann::json::parse(file);
      if (!json.is_object()) {
        kLog.warn("catalog {} is not a JSON object", path.string());
        return nullptr;
      }
      auto loaded = std::make_shared<Translations::Loaded>();
      if (const auto rules = json.find("pluralRules"); rules != json.end()) {
        loaded->rules = parsePluralRules(*rules);
        json.erase(rules);
      }
      flatten(json, {}, loaded->catalog);
      buildTextIndex(json, loaded->text);
      return loaded;
    } catch (const std::exception& e) {
      kLog.error("failed to parse {}: {}", path.string(), e.what());
      return nullptr;
    }
  }

  void Service::init(std::string_view preferredLang) {
    const std::string candidate = resolveRequestedLanguage(preferredLang);
    m_requestedLanguage = candidate;

    // English fallback is always loaded first so lookup() can fall back to it
    // even if the active catalog is English itself.
    m_translations.m_fallback = loadCatalog("en");
    if (!m_translations.m_fallback) {
      kLog.warn("could not load English fallback catalog");
    }
    m_translations.m_active = m_translations.m_fallback;

    if (candidate.empty() || candidate == "en") {
      m_language = "en";
      m_rtl = false;
      kLog.info("language: en");
      return;
    }

    for (const std::string& lang : detail::catalogLanguageCandidates(candidate)) {
      if (auto active = loadCatalog(lang)) {
        m_translations.m_active = std::move(active);
        m_language = lang;
        m_rtl = detail::isRtlLanguage(m_language);
        if (lang == candidate) {
          kLog.info("language: {}", m_language);
        } else {
          kLog.info("language: {} (from {})", m_language, candidate);
        }
        return;
      }
    }

    kLog.warn("no catalog for '{}', falling back to English", candidate);
    m_language = "en";
    m_rtl = false;
  }

  void Service::setLanguage(std::string_view lang) {
    // Compare the resolved request, not the loaded catalog: an empty (auto) or regional tag
    // resolves to a different catalog name and would otherwise reload on every config reload.
    if (resolveRequestedLanguage(lang) == m_requestedLanguage) {
      return;
    }
    init(lang);
  }

  std::string_view Translations::lookup(std::string_view key) const {
    if (m_active && m_active != m_fallback) {
      if (const auto value = m_active->lookup(key); !value.empty()) {
        return value;
      }
    }
    return m_fallback ? m_fallback->lookup(key) : std::string_view{};
  }

  std::string_view Translations::lookupPlural(std::string_view key, std::uint64_t count) const {
    if (m_active && m_active != m_fallback) {
      if (const auto value = m_active->lookupPlural(key, count); !value.empty()) {
        return value;
      }
    }
    return m_fallback ? m_fallback->lookupPlural(key, count) : std::string_view{};
  }

  std::vector<std::string> Service::availableLanguages() const {
    std::vector<std::string> languages;
    const auto directory = paths::assetPath("translations");
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
      if (error || !entry.is_regular_file(error) || entry.path().extension() != ".json") {
        continue;
      }
      const auto name = entry.path().stem().string();
      if (!name.empty()) {
        languages.push_back(name);
      }
    }
    std::ranges::sort(languages);
    return languages;
  }

} // namespace i18n
