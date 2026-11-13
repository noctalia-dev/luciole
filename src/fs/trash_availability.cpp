#include "fs/trash_availability.h"

#include "fs/gio_interop.h"

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
  struct Request {
    std::shared_ptr<State> state;
    std::uint64_t revision;
    std::function<void(std::optional<std::uint32_t>)> completed;
  };
  const auto file = gioFile(Location::trash());
  g_file_query_info_async(
      file.get(), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
      m_state->cancellable.get(),
      [](GObject* source, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<Request> request(static_cast<Request*>(data));
        const GioPtr<GFileInfo> info(g_file_query_info_finish(G_FILE(source), result, nullptr));
        if (!request->state->active || request->revision != request->state->revision) {
          return;
        }
        request->state->cancellable.reset();
        std::optional<std::uint32_t> count;
        if (info
            && g_file_info_get_attribute_type(info.get(), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT)
                == G_FILE_ATTRIBUTE_TYPE_UINT32) {
          count = g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT);
        }
        request->completed(count);
      },
      new Request{m_state, m_state->revision, std::move(completed)}
  );
}
