#include "browser/entry_details.h"

#include "browser/location_presentation.h"
#include "fs/gio_ptr.h"

#include <array>
#include <chrono>
#include <format>

namespace {
  std::string mediaDuration(std::uint64_t micros) {
    const std::chrono::hh_mm_ss split{std::chrono::microseconds(micros)};
    return split.hours().count()
        ? std::format("{}:{:02d}:{:02d}", split.hours().count(), split.minutes().count(), split.seconds().count())
        : std::format("{}:{:02d}", split.minutes().count(), split.seconds().count());
  }

  std::string_view orientationLabel(std::uint8_t orientation) {
    constexpr std::array<std::string_view, 9> labels{
        "Unknown",       "Normal",     "Mirrored horizontally", "180°", "Mirrored vertically", "Transposed",
        "90° clockwise", "Transverse", "90° counterclockwise"
    };
    return orientation < labels.size() ? labels[orientation] : "Unknown";
  }
} // namespace

std::string luciole::metadataTime(std::optional<std::uint64_t> seconds) {
  if (!seconds || *seconds > G_MAXINT64) {
    return "Unknown";
  }
  const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
      g_date_time_new_from_unix_local(static_cast<gint64>(*seconds)), &g_date_time_unref
  );
  const GString value(date ? g_date_time_format(date.get(), "%Y-%m-%d %H:%M") : nullptr);
  return value ? value.get() : "Unknown";
}

std::uint8_t luciole::entryColumnDetails(DetailsColumn column) {
  switch (column) {
  case DetailsColumn::Accessed:
    return EntryDetails::Accessed;
  case DetailsColumn::Created:
    return EntryDetails::Created;
  case DetailsColumn::Owner:
    return EntryDetails::Owner;
  case DetailsColumn::Group:
    return EntryDetails::Group;
  case DetailsColumn::Permissions:
    return EntryDetails::Permissions;
  case DetailsColumn::Dimensions:
  case DetailsColumn::Duration:
  case DetailsColumn::BitRate:
  case DetailsColumn::AudioCodec:
  case DetailsColumn::VideoCodec:
  case DetailsColumn::DatePhotographed:
  case DetailsColumn::Width:
  case DetailsColumn::Height:
  case DetailsColumn::Orientation:
  case DetailsColumn::Album:
  case DetailsColumn::Artist:
  case DetailsColumn::Genre:
  case DetailsColumn::ReleaseYear:
  case DetailsColumn::Track:
  case DetailsColumn::AspectRatio:
  case DetailsColumn::ColorSpace:
  case DetailsColumn::FrameRate:
  case DetailsColumn::PixelFormat:
    return EntryDetails::Media;
  default:
    return 0;
  }
}

std::string luciole::entryColumnText(const DirectoryEntry& entry, DetailsColumn column, std::string_view typeLabel) {
  const auto details = entryColumnDetails(column);
  if (column != DetailsColumn::Location && (entry.metadataPending || (entry.queriedDetails & details) != details)) {
    return "Reading…";
  }
  switch (column) {
  case DetailsColumn::Type:
    return std::string(typeLabel);
  case DetailsColumn::Size: {
    if (entry.type == FileType::Directory) {
      return {};
    }
    const GString size(entry.size ? g_format_size_full(*entry.size, G_FORMAT_SIZE_IEC_UNITS) : nullptr);
    return size ? size.get() : "Unknown";
  }
  case DetailsColumn::Modified:
    return metadataTime(entry.modifiedSeconds);
  case DetailsColumn::Accessed:
    return metadataTime(entry.details.accessedSeconds);
  case DetailsColumn::Created:
    return metadataTime(entry.details.createdSeconds);
  case DetailsColumn::Owner:
    return std::string(entry.details.owner.value_or("Unknown"));
  case DetailsColumn::Group:
    return std::string(entry.details.group.value_or("Unknown"));
  case DetailsColumn::Permissions:
    return entry.details.mode ? std::format("{:04o}", *entry.details.mode) : "Unknown";
  case DetailsColumn::Location: {
    const auto parent = entry.location().parent();
    return parent ? locationEditText(*parent) : "Unknown";
  }
  case DetailsColumn::Dimensions:
    return entry.media && entry.media->width && entry.media->height
        ? std::format("{} × {}", *entry.media->width, *entry.media->height)
        : "Unknown";
  case DetailsColumn::Width:
    return entry.media && entry.media->width ? std::format("{}", *entry.media->width) : "Unknown";
  case DetailsColumn::Height:
    return entry.media && entry.media->height ? std::format("{}", *entry.media->height) : "Unknown";
  case DetailsColumn::Duration:
    return entry.media && entry.media->durationMicros ? mediaDuration(*entry.media->durationMicros) : "Unknown";
  case DetailsColumn::BitRate:
    return entry.media && entry.media->bitsPerSecond
        ? std::format("{} kb/s", (*entry.media->bitsPerSecond + 999U) / 1000U)
        : "Unknown";
  case DetailsColumn::AudioCodec:
    return std::string(entry.media ? entry.media->audioCodec.value_or("Unknown") : "Unknown");
  case DetailsColumn::VideoCodec:
    return std::string(entry.media ? entry.media->videoCodec.value_or("Unknown") : "Unknown");
  case DetailsColumn::DatePhotographed:
    return metadataTime(entry.media ? entry.media->photoDateSeconds : std::nullopt);
  case DetailsColumn::Orientation:
    return entry.media && entry.media->orientation ? std::string(orientationLabel(*entry.media->orientation))
                                                   : "Unknown";
  case DetailsColumn::Album:
    return std::string(entry.media ? entry.media->album.value_or("Unknown") : "Unknown");
  case DetailsColumn::Artist:
    return std::string(entry.media ? entry.media->artist.value_or("Unknown") : "Unknown");
  case DetailsColumn::Genre:
    return std::string(entry.media ? entry.media->genre.value_or("Unknown") : "Unknown");
  case DetailsColumn::ReleaseYear:
    return entry.media && entry.media->releaseYear ? std::format("{}", *entry.media->releaseYear) : "Unknown";
  case DetailsColumn::Track:
    return entry.media && entry.media->track ? std::format("{}", *entry.media->track) : "Unknown";
  case DetailsColumn::AspectRatio:
    return std::string(entry.media ? entry.media->aspectRatio.value_or("Unknown") : "Unknown");
  case DetailsColumn::ColorSpace:
    return std::string(entry.media ? entry.media->colorSpace.value_or("Unknown") : "Unknown");
  case DetailsColumn::PixelFormat:
    return std::string(entry.media ? entry.media->pixelFormat.value_or("Unknown") : "Unknown");
  case DetailsColumn::FrameRate:
    return entry.media && entry.media->frameRateMilli
        ? (*entry.media->frameRateMilli % 1000U == 0U
               ? std::format("{} fps", *entry.media->frameRateMilli / 1000U)
               : std::format("{:.3f} fps", *entry.media->frameRateMilli / 1000.0))
        : "Unknown";
  }
  return "Unknown";
}

std::string luciole::entryFolderSizeText(const FolderTotalsSnapshot& totals) {
  if (totals.phase != FolderTotalsPhase::Complete) {
    return {};
  }
  const GString size(g_format_size_full(totals.bytes, G_FORMAT_SIZE_IEC_UNITS));
  if (!size) {
    return {};
  }
  if (totals.partial()) {
    return std::format("≥ {}", size.get());
  }
  return size.get();
}

std::optional<std::uint64_t> luciole::entrySortNumber(const DirectoryEntry& entry, SortField field) {
  switch (field) {
  case SortField::Size:
    return entry.size;
  case SortField::Modified:
    return entry.modifiedSeconds;
  case SortField::Accessed:
    return entry.details.accessedSeconds;
  case SortField::Created:
    return entry.details.createdSeconds;
  case SortField::Permissions:
    return entry.details.mode;
  case SortField::Dimensions: {
    if (!entry.media || !entry.media->width || !entry.media->height) {
      return {};
    }
    return static_cast<std::uint64_t>(*entry.media->width) * *entry.media->height;
  }
  case SortField::Width: {
    if (!entry.media) {
      return {};
    }
    return entry.media->width;
  }
  case SortField::Height: {
    if (!entry.media) {
      return {};
    }
    return entry.media->height;
  }
  case SortField::Duration: {
    if (!entry.media) {
      return {};
    }
    return entry.media->durationMicros;
  }
  case SortField::BitRate: {
    if (!entry.media) {
      return {};
    }
    return entry.media->bitsPerSecond;
  }
  case SortField::FrameRate: {
    if (!entry.media) {
      return {};
    }
    return entry.media->frameRateMilli;
  }
  case SortField::AspectRatio: {
    if (!entry.media) {
      return {};
    }
    return entry.media->aspectRatioPerMille;
  }
  case SortField::DatePhotographed: {
    if (!entry.media) {
      return {};
    }
    return entry.media->photoDateSeconds;
  }
  case SortField::Orientation: {
    if (!entry.media) {
      return {};
    }
    return entry.media->orientation;
  }
  case SortField::ReleaseYear: {
    if (!entry.media) {
      return {};
    }
    return entry.media->releaseYear;
  }
  case SortField::Track: {
    if (!entry.media) {
      return {};
    }
    return entry.media->track;
  }
  default:
    return {};
  }
}
