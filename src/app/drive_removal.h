#pragma once

#include "fs/drive_monitor.h"
#include "fs/file_error.h"

#include <functional>
#include <optional>

// Prepare mount-specific ownership first, then wait for cleanup and retiring readers before normal removal.
// Only transient busy results naming this process alone are retried. No retry discards additional history.
void removeDriveAfterPreparation(
    const std::function<void(std::function<void(std::optional<luciole::FileError>)>)>& prepare,
    std::function<bool()> ready, std::function<void(std::function<void(luciole::DriveRemoveResult)>)> remove,
    std::function<void(luciole::DriveRemoveResult)> done
);
