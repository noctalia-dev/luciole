#pragma once

#include "clipboard/file_payload.h"

#include <functional>
#include <memory>
#include <string>

namespace luciole {
  // Requests source-owned extraction through the advertised KDE D-Bus contract.
  // A successful reply is handoff, not extraction completion. The source owns progress/conflicts.
  class ExtractionLauncher {
  public:
    ExtractionLauncher();
    ~ExtractionLauncher();
    ExtractionLauncher(const ExtractionLauncher&) = delete;
    ExtractionLauncher& operator=(const ExtractionLauncher&) = delete;
    bool request(ExtractionOffer offer, Location destination, std::function<void(std::string)> completed);
    void cancel();
    bool busy() const;
    // Bounds and validates the paired MIME data, including a unique bus name and object path.
    static bool valid(const ExtractionOffer& offer);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
