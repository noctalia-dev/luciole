#pragma once

#include "fs/directory_session.h"
#include "model/thumbnail_options.h"

#include <memory>
#include <span>
#include <string_view>

namespace luciole {
  struct MetadataField {
    std::string label;
    std::string value;
    // Extended technical and filesystem facts belong only in Properties.
    bool preview = true;
    // One line, eliding the middle: long paths keep both their root and the file name.
    bool singleLine = false;
    bool operator==(const MetadataField&) const = default;
  };
  using MetadataFields = std::vector<MetadataField>;

  // One metadata probe and one replaceable latest request. Image-size changes do not reprobe a source.
  class PreviewMetadataService {
  public:
    PreviewMetadataService();
    ~PreviewMetadataService();
    PreviewMetadataService(const PreviewMetadataService&) = delete;
    PreviewMetadataService& operator=(const PreviewMetadataService&) = delete;
    void show(const DirectoryEntry& entry, ThumbnailOptions options);
    void clear();
    void setChangedCallback(std::function<void()> callback);
    const MetadataFields& fields() const;
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };

  // Private metadata helper: MIME type and encoded-byte limit. Input is descriptor 3, output is JSON on stdout.
  int runPreviewMetadataWorker(std::span<const std::string_view> args);
} // namespace luciole
