#include "fs/trash_availability.h"

#include "fs/gio_interop.h"
#include "fs/network_info.h"
#include "fs/network_job.h"

#include <utility>

struct luciole::TrashAvailability::State {
  GioPtr<GCancellable> cancellable;
  std::uint64_t revision = 0;
  bool active = true;
};

luciole::TrashAvailability::TrashAvailability() : m_state(std::make_shared<State>()) {}
luciole::TrashAvailability::~TrashAvailability() {
  m_state->active = false;
  cancel();
}
void luciole::TrashAvailability::cancel() {
  ++m_state->revision;
  if (m_state->cancellable) {
    g_cancellable_cancel(m_state->cancellable.get());
    m_state->cancellable.reset();
  }
}
void luciole::TrashAvailability::query(std::function<void(std::optional<std::uint32_t>)> completed) {
  cancel();
  m_state->cancellable.reset(g_cancellable_new());
  const auto state = m_state;
  const auto token = cancellationToken(state->cancellable.get());
  runIoJob<std::optional<std::uint32_t>>(
      IoLane::Network, token,
      [token] -> std::optional<std::uint32_t> {
        const auto info =
            isolatedInfo(Location::trash(), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT, G_FILE_QUERY_INFO_NONE, token);
        if (g_file_info_get_attribute_type(info.get(), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT)
            == G_FILE_ATTRIBUTE_TYPE_UINT32) {
          return g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT);
        }
        return std::nullopt;
      },
      [state, revision = state->revision, completed = std::move(completed)](auto result) {
        if (!state->active || revision != state->revision) {
          return;
        }
        state->cancellable.reset();
        completed(result ? *result : std::nullopt);
      }
  );
}
