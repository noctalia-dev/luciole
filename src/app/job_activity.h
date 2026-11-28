#pragma once

#include "model/job_activity.h"

class OperationJob;
namespace luciole {
  struct TransferProgress;
}

// Main-thread snapshots of service facts only. Never performs filesystem I/O or provider queries.
std::optional<luciole::JobActivity> jobActivity(const OperationJob& job, bool details = false);
// The row line of a running transfer: bytes, speed, files and time left, then the current item.
std::string transferMetersText(const luciole::TransferProgress& measured);
