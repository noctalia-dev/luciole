#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace i18n {

  // Transparent hash/eq so Catalog::find() can take string_view without
  // allocating a temporary std::string.
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(s); }
  };

  struct StringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };

  using Catalog = std::unordered_map<std::string, std::string, StringHash, StringEq>;

  // Maps a catalog key (the English source text) to its translation so that
  // lookup() can resolve bare identity keys regardless of which purpose
  // section (menu, view, settings, ...) they live in.
  using TextIndex = std::unordered_map<std::string, std::string, StringHash, StringEq>;

  // A copy keeps both catalogs and their plural rules alive across language changes.
  class Translations {
  public:
    std::string_view lookup(std::string_view key) const;
    std::string_view lookupPlural(std::string_view key, std::uint64_t count) const;

  private:
    friend class Service;
    struct Loaded;
    std::shared_ptr<const Loaded> m_active;
    std::shared_ptr<const Loaded> m_fallback;
  };

  // Loads translation catalogs and resolves dotted keys against them.
  // English is always loaded as a fallback alongside the active language.
  class Service {
  public:
    static Service& instance();

    // Loads the active catalog. Pass an empty view to auto-detect from the
    // standard locale environment ($LC_ALL, $LC_MESSAGES, $LANG). Always
    // loads English as a fallback first; if the detected language is also
    // English, both slots end up pointing at the same catalog contents.
    void init(std::string_view preferredLang = {});

    void setLanguage(std::string_view lang);
    std::string_view language() const noexcept { return m_language; }
    std::string_view requestedLanguage() const noexcept { return m_requestedLanguage; }
    bool rtl() const noexcept { return m_rtl; }
    std::vector<std::string> availableLanguages() const;

    // Returns a view into the active or fallback catalog, or {} if the key
    // exists in neither.
    std::string_view lookup(std::string_view key) const { return m_translations.lookup(key); }
    const Translations& translations() const noexcept { return m_translations; }

  private:
    std::shared_ptr<const Translations::Loaded> loadCatalog(std::string_view lang) const;

    Translations m_translations;
    std::string m_language;
    std::string m_requestedLanguage;
    bool m_rtl = false;
  };

} // namespace i18n
