#pragma once
#include "fs/network_process.h"
namespace luciole {
  // Wake the worker context to sample progress and receive cancellation during a native job.
  class WorkerProgressTimer {
  public:
    WorkerProgressTimer();
    ~WorkerProgressTimer();
    WorkerProgressTimer(const WorkerProgressTimer&) = delete;
    WorkerProgressTimer& operator=(const WorkerProgressTimer&) = delete;

  private:
    unsigned m_source;
  };
  void releaseWorkerObjects(const NetworkMessage& request);
  NetworkMessage networkOperationRequest(const NetworkMessage& request, const NetworkEvent& event, int descriptor = -1);
} // namespace luciole
