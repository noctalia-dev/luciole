#pragma once

#include "fs/network_process.h"

namespace luciole {
  void releaseWorkerPrograms(const NetworkMessage& request);
  NetworkMessage networkProgramRequest(const NetworkMessage& request);
} // namespace luciole
