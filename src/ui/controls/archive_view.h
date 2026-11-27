#pragma once

#include "operations/archive_service.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/controls/flex.h"
#include "ui/controls/virtual_grid_view.h"

#include <array>
#include <set>

class Label;
class Button;
namespace luciole {
  class FileIcon;
}
class ArchiveView final : public Flex {
public:
  ArchiveView(
      luciole::FileIconCache& icons, std::function<std::uint32_t()> modifiers, std::function<void()> close,
      std::function<void(std::vector<std::string>)> extract, std::function<void(std::vector<std::string>)> extractHere,
      std::function<void(const std::string&)> open, std::function<void()> reload, std::function<void()> cancel,
      std::function<void(std::size_t)> archiveParent
  );
  ~ArchiveView() override;
  void sync(
      std::shared_ptr<const luciole::ArchiveCatalog> catalog, luciole::ArchivePhase phase, std::size_t items,
      std::uint64_t bytes, const std::optional<luciole::FileError>& error, const std::vector<std::string>& archives
  );
  enum class SortColumn : std::uint8_t { Name, Type, Size, Modified };
  struct Position {
    std::string parent;
    std::set<std::string> selection;
    std::optional<std::size_t> focus, anchor;
    float scroll = 0.0F;
    SortColumn sort = SortColumn::Name;
    bool descending = false;
  };
  Position position() const;
  void restore(Position position);
  bool key(std::uint32_t symbol, std::uint32_t modifiers);
  InputArea* focusArea() const;

private:
  class Adapter;
  void rebuildCrumbs();
  void openFolder(const std::string& path);
  std::string statusText() const;
  void sortBy(SortColumn column, bool descending);
  void syncInfo();
  std::vector<std::string> selected() const;
  void rebuild();
  void choose(std::size_t index, std::uint32_t modifiers);
  void activate(std::size_t index);
  void up();
  void selectionChanged();
  void doLayout(Renderer& renderer) override;
  std::unique_ptr<Adapter> m_adapter;
  std::shared_ptr<const luciole::ArchiveCatalog> m_catalog;
  std::vector<std::size_t> m_rows;
  std::set<std::string> m_selection;
  std::string m_parent;
  std::optional<std::size_t> m_focus, m_anchor;
  std::function<std::uint32_t()> m_modifiers;
  std::function<void(std::vector<std::string>)> m_extract, m_extractHere;
  std::function<void(const std::string&)> m_open;
  std::function<void(std::size_t)> m_archiveParent;
  std::vector<std::string> m_archives;
  VirtualGridView* m_grid = nullptr;
  InputArea* m_shield = nullptr;
  Label *m_title = nullptr, *m_info = nullptr, *m_status = nullptr, *m_cardName = nullptr;
  Button *m_up = nullptr, *m_extractTo = nullptr, *m_extractHereButton = nullptr, *m_openButton = nullptr,
         *m_cancel = nullptr, *m_reload = nullptr;
  std::array<Button*, 4> m_headers{};
  Flex *m_crumbs = nullptr, *m_headerRow = nullptr, *m_card = nullptr;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  luciole::FileIcon* m_cardIcon = nullptr;
  SortColumn m_sort = SortColumn::Name;
  bool m_descending = false;
  bool m_busy = false;
};
