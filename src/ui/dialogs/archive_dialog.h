#pragma once

#include "operations/archive_service.h"
#include "ui/controls/flex.h"

#include <functional>
#include <string>

class Label;
class Input;
class InputArea;
class ScrollView;

class ArchiveDialog final : public Flex {
public:
  ArchiveDialog(
      bool compress, std::string name, std::string destination,
      std::function<std::string(
          const std::string&, const std::string&, luciole::ArchiveFormat, luciole::ArchiveCompressionLevel,
          const std::string&
      )>
          submit,
      std::function<void()> cancel, std::size_t archiveCount = 1
  );
  InputArea* focusArea() const;
  void reveal(const Node* node);

private:
  void submit();
  void showPassword(bool visible);
  void showCompression(bool visible);
  Input *m_name = nullptr, *m_destination = nullptr;
  Flex* m_compressionSection = nullptr;
  Flex* m_passwordSection = nullptr;
  Input *m_password = nullptr, *m_confirmation = nullptr;
  ScrollView* m_scroll = nullptr;
  Label* m_error = nullptr;
  std::size_t m_format = 0;
  luciole::ArchiveCompressionLevel m_level = luciole::ArchiveCompressionLevel::Balanced;
  std::function<std::string(
      const std::string&, const std::string&, luciole::ArchiveFormat, luciole::ArchiveCompressionLevel,
      const std::string&
  )>
      m_submit;
};
class ArchivePasswordDialog final : public Flex {
public:
  ArchivePasswordDialog(
      std::string name, bool retry, std::function<bool(std::string)> submit, std::function<void()> cancel,
      luciole::ArchiveFormat format = luciole::ArchiveFormat::Zip
  );
  InputArea* focusArea() const;

private:
  void submit();
  Input* m_password = nullptr;
  Label* m_error = nullptr;
  std::function<bool(std::string)> m_submit;
  luciole::ArchiveFormat m_format = luciole::ArchiveFormat::Zip;
};
