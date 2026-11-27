#pragma once

#include "thumbnails/preview_service.h"
#include "ui/browser/file_icon.h"
#include "ui/controls/flex.h"

class Label;
class Button;
class ScrollView;
class FileMetadataView;
class FilePreviewView final : public Flex {
public:
  // `quick` builds the quick preview dialog (dialog chrome, the file name as title, Open as the primary action);
  // otherwise the flush docked pane. Both show the file facts in a Details card.
  FilePreviewView(
      luciole::FileIconCache& icons, bool quick, std::function<void()> close, std::function<void()> open,
      std::function<void()> openWith
  );
  void sync(const luciole::PreviewSnapshot& snapshot, float width, float height, std::string_view typeLabel = {});
  // False while the image row is hidden (no selection, or a text preview); only then may its texture be released.
  [[nodiscard]] bool showsImage() const;
  void clear(Renderer& renderer);

private:
  luciole::FileIcon* m_image;
  Label *m_heading, *m_name, *m_status, *m_text;
  FileMetadataView* m_metadata;
  // The fixed preview box, the text page shown in it in place of the image, and the status badge on Details.
  Flex *m_details, *m_stage, *m_page, *m_statusPill;
  Button *m_open, *m_openWith;
  ScrollView *m_scroll, *m_textScroll;
  std::string m_location;
  bool m_quick;
};
