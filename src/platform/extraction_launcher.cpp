#include "platform/extraction_launcher.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"

#include <utility>

namespace {
  void checkError(GError* error) {
    if (error) {
      const luciole::GErrorPtr owned(error);
      throw luciole::FileErrorException(luciole::fileError(*error));
    }
  }
  bool fieldValid(const std::string& text) {
    return !text.empty()
        && text.size() <= 4096
        && !text.contains('\0')
        && g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
  }
} // namespace

struct luciole::ExtractionLauncher::State {
  CancelToken token;
  bool busy = false;
};

luciole::ExtractionLauncher::ExtractionLauncher() : m_state(std::make_shared<State>()) {}
luciole::ExtractionLauncher::~ExtractionLauncher() { cancel(); }
void luciole::ExtractionLauncher::cancel() { m_state->token.cancel(); }
bool luciole::ExtractionLauncher::busy() const { return m_state->busy; }
bool luciole::ExtractionLauncher::valid(const ExtractionOffer& offer) {
  return fieldValid(offer.service)
      && fieldValid(offer.path)
      && g_dbus_is_unique_name(offer.service.c_str())
      && g_variant_is_object_path(offer.path.c_str());
}

bool luciole::ExtractionLauncher::request(
    ExtractionOffer offer, Location destination, std::function<void(std::string)> completed
) {
  if (busy()) {
    return false;
  }
  m_state->busy = true;
  m_state->token = CancelToken{};
  runJob<bool>(
      m_state->token,
      [offer = std::move(offer), destination = std::move(destination), token = m_state->token] {
        if (!valid(offer)) {
          throwFileError(FileErrorKind::InvalidArgument, "Invalid archive extraction offer.");
        }
        const auto path = destination.nativePath();
        if (!path || !fieldValid(*path)) {
          throwFileError(FileErrorKind::Unsupported, "This extraction offer requires a local UTF-8 folder path.");
        }
        GError* error = nullptr;
        const GioPtr<GFileInfo> info(g_file_query_info(
            gioFile(destination).get(), G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
            G_FILE_QUERY_INFO_NONE, gioCancellable(token), &error
        ));
        checkError(error);
        if (!info || g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY) {
          throwFileError(FileErrorKind::NotDirectory, "The extraction destination is not a folder.");
        }
        if (g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)
            && !g_file_info_get_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)) {
          throwFileError(FileErrorKind::Denied, "Writing to the extraction destination is not permitted.");
        }
        const GioPtr<GDBusConnection> connection(g_bus_get_sync(G_BUS_TYPE_SESSION, gioCancellable(token), &error));
        checkError(error);
        auto* reply = g_dbus_connection_call_sync(
            connection.get(), offer.service.c_str(), offer.path.c_str(), "org.kde.ark.DndExtract",
            "extractSelectedFilesTo", g_variant_new("(s)", path->c_str()), G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, gioCancellable(token), &error
        );
        checkError(error);
        if (!reply) {
          throwFileError(FileErrorKind::Unsupported, "The source did not acknowledge the extraction request.");
        }
        g_variant_unref(reply);
        return true;
      },
      [owner = m_state, completed = std::move(completed)](JobResult<bool> result) {
        owner->busy = false;
        // Always retire the request, even after cancellation/destruction. UI callers lifetime-gate their observers.
        completed(result ? std::string{} : result.error().message);
      }
  );
  return true;
}
