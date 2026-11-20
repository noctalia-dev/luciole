#pragma once

#include "fs/folder_totals.h"
#include "ui/controls/flex.h"

class Button;
class Label;

// Size and counts text shared by the Properties card and the preview's Contents row.
std::string folderTotalsSizeText(const luciole::FolderTotalsSnapshot& value);
std::string folderTotalsCountsText(const luciole::FolderTotalsSnapshot& value);

// Properties-only recursive inspection, using the shared controls and a worker service.
class FolderTotalsView final : public Flex {
public:
  FolderTotalsView(std::span<const luciole::DirectoryEntry> targets, std::function<void()> requestSync);
  void sync(float width);

private:
  luciole::FolderTotalsService m_service;
  Label *m_status = nullptr, *m_counts = nullptr, *m_note = nullptr;
  Button* m_stop = nullptr;
};
