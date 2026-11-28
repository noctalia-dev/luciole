#pragma once

#include "fs/file_error.h"
#include "fs/location.h"

#include <optional>
#include <utility>
#include <vector>

namespace luciole {
  // Verified transfer backends; this check never performs remote I/O.
  bool transferLocationSupported(const Location& location);
  // Items on an MTP phone or PTP camera, below the device root. They copy or move into local folders, and onto the
  // same phone.
  bool transferDeviceSource(const Location& location);
  // A folder on an MTP phone below its top level, which only lists the phone's storages and cannot be written.
  // Files from local folders or the same phone can be published there.
  bool transferDeviceDestination(const Location& location);
  // Folders a transfer can publish into, for at least some sources.
  bool transferDestinationSupported(const Location& location);
  // Whether a transfer can read `source` and publish it in `destination`; never performs remote I/O.
  bool transferSupported(const Location& source, const Location& destination);

  // Includes conservative same-server SMB case aliases, without remote I/O.
  bool transferLocationsEqual(const Location& source, const Location& target);
  bool transferLocationInside(const Location& child, const Location& parent);

  // Namespace eligibility only. The worker's staged symlink operation checks actual provider support.
  std::optional<std::string> transferLinkTarget(const Location& source, const Location& destination);

  enum class TransferFlush { None, Device, Final };
  enum class TransferAction { Move, Copy, Link };
  enum class TransferOutcome { Moved, Copied, Skipped, Failed, Cancelled, Partial, Linked, Deleted };
  struct TransferResult {
    TransferResult(
        Location location, TransferOutcome value = TransferOutcome::Failed, std::optional<FileError> failure = {}
    )
        : source(std::move(location)), outcome(value), error(std::move(failure)) {}
    Location source;
    TransferOutcome outcome = TransferOutcome::Failed;
    std::optional<FileError> error;
    std::vector<TransferResult> children;
    bool retryable = false;
    std::optional<Location> destination;
  };
  bool
  transferAffectsLocation(const std::vector<TransferResult>& results, const Location& location, bool recursive = false);
} // namespace luciole
