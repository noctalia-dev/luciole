#include "i18n/i18n.h"
#include "test_check.h"
#include "ui/size_format.h"

#include <limits>

int main() {
  auto& service = i18n::Service::instance();
  service.init("uk-UA");
  TEST_CHECK(service.language() == "uk");
  TEST_CHECK(i18n::trp("file", 0) == "файлів");
  TEST_CHECK(i18n::trp("file", 1) == "файл");
  TEST_CHECK(i18n::trp("file", 11) == "файлів");
  TEST_CHECK(i18n::trp("folder", 12) == "тек");
  TEST_CHECK(i18n::trp("link", 14) == "посилань");
  TEST_CHECK(i18n::trp("rename.items", 21) == "Перейменувати 21 об’єкт");
  TEST_CHECK(i18n::trp("item", 22) == "об’єкти");
  TEST_CHECK(i18n::trp("result", 25) == "результатів");
  TEST_CHECK(i18n::trp("item", 111) == "об’єктів");
  TEST_CHECK(i18n::trp("result", 121) == "результат");

  TEST_CHECK(i18n::trp("operations.copied", 24) == "Скопійовано 24 об’єкти");
  TEST_CHECK(i18n::trp("operations.trashed", 114) == "Переміщено до кошика 114 об’єктів");

  const auto ukrainianUnits = luciole::localizedSizeUnits();
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(0) == "0 байтів");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(1) == "1 байт");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(13) == "13 байтів");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(21) == "21 байт");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(1023) == "1023 байти");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(1024) == "1.00 КіБ");
  TEST_CHECK(luciole::formatByteCount(1024, ukrainianUnits) == "1024 байти");

  service.setLanguage("en");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(1) == "1 byte");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(13) == "13 bytes");
  TEST_CHECK(luciole::formatSizeWithUnits(22, ukrainianUnits) == "22 байти");
  TEST_CHECK(i18n::trp("file", 1) == "file");
  TEST_CHECK(i18n::trp("file", 2) == "files");
  TEST_CHECK(i18n::trp("folder", 0) == "folders");
  TEST_CHECK(i18n::trp("link", 21) == "links");
  TEST_CHECK(i18n::trp("item", 1) == "item");
  TEST_CHECK(i18n::trp("result", 2) == "results");
  TEST_CHECK(i18n::trp("rename.items", 21) == "Rename 21 items");

  service.setLanguage("zz-BB");
  TEST_CHECK(service.language() == "zz");
  TEST_CHECK(i18n::trp("rename.items", 0) == "Nothing to rename");
  TEST_CHECK(i18n::trp("rename.items", 1) == "Rename 1 item");
  TEST_CHECK(i18n::trp("rename.items", 21) == "Rename 21 items");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(0) == "0 empty");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(11) == "11 single");
  TEST_CHECK(luciole::formatSizeWithLocalizedUnits(22) == "22 multiple");
  TEST_CHECK(i18n::trp("byte", std::numeric_limits<std::uint64_t>::max()) == "multiple");

  service.setLanguage("xx");
  TEST_CHECK(i18n::trp("file", 1) == "entry");
  TEST_CHECK(i18n::trp("file", 21) == "entries");
}
