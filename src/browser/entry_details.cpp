#include "browser/entry_details.h"

#include "browser/location_presentation.h"
#include "fs/gio_ptr.h"

#include <format>

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
  default:
    return {};
  }
}
