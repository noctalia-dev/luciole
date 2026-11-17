#include "browser/browser_controller.h"

void luciole::BrowserController::sessionChanged() {
  if (m_switchingSources) {
    return;
  }
  const auto& snapshot = m_session.snapshot();
  if (!m_mountAttempted
      && snapshot.location
      && !snapshot.location->isNative()
      && snapshot.status == DirectoryStatus::Error
      && snapshot.error
      && snapshot.error->kind == FileErrorKind::NotMounted) {
    m_mountAttempted = true;
    mountLocation(*snapshot.location);
    return;
  }
  if (m_query.empty() || !m_searchRecursive) {
    publish(snapshot);
  }
}
luciole::MountInteraction* luciole::BrowserController::mountInteraction() const {
  return m_mount ? &m_mount->interaction() : nullptr;
}
void luciole::BrowserController::activateMountable(const Location& location) {
  if (waitForVisitMetadata([this, location] { activateMountable(location); })) {
    return;
  }
  open(location);
  m_switchingSources = true;
  m_session.cancel();
  m_switchingSources = false;
  m_mountAttempted = true;
  mountLocation(location, true);
}
void luciole::BrowserController::mountLocation(const Location& location, bool mountable) {
  if (!m_mount) {
    m_mount = std::make_unique<LocationMount>();
    m_mount->setChangedCallback([this] { notify(); });
  }
  m_mount->start(
      location,
      [this, weak = std::weak_ptr(m_lifetime), context = m_contextSerial](std::expected<Location, FileError> result) {
        if (weak.expired() || context != m_contextSerial) {
          return;
        }
        if (!result) {
          auto snapshot = m_session.snapshot();
          snapshot.status =
              result.error().kind == FileErrorKind::Cancelled ? DirectoryStatus::Cancelled : DirectoryStatus::Error;
          snapshot.error = std::move(result.error());
          publish(std::move(snapshot));
        } else {
          if (m_history[m_historyIndex].location != *result) {
            m_history[m_historyIndex].location = *result;
            m_session.open(*result);
          } else {
            m_session.refresh();
          }
          if (!m_query.empty() && m_searchRecursive) {
            startSearch();
          }
        }
      },
      mountable
  );
  auto snapshot = m_session.snapshot();
  snapshot.status = DirectoryStatus::Loading;
  snapshot.error.reset();
  publish(std::move(snapshot));
}
