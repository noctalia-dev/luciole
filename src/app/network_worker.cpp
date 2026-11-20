#include "operations/network_worker.h"

#include "core/application_identity.h"
#include "fs/network_info.h"
#include "imaging/pixbuf_image_decoder.h"
#include "operations/copy_receipt.h"
#include "operations/move_receipt.h"
#include "operations/network_types.h"
#include "platform/network_program.h"
#include "thumbnails/network_preview.h"
#include "thumbnails/thumbnail_worker.h"

#include <thread>

int main(int argc, char** argv) {
  setApplicationIdentity({.name = "luciole", .envPrefix = "LUCIOLE"});
  std::vector<std::string_view> args;
  for (int i = 2; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  if (argc > 1 && std::string_view(argv[1]) == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(args);
  }
  if (argc > 1 && std::string_view(argv[1]) == "--preview-metadata-worker") {
    return luciole::runPreviewMetadataWorker(args);
  }
  luciole::installPixbufImageDecoder();
  return luciole::runNetworkWorker([](const luciole::NetworkMessage& request, const luciole::NetworkEvent& event,
                                      int descriptor) {
    luciole::releaseWorkerPrograms(request);
    luciole::releaseWorkerObjects(request);
    const auto action = request.at("action").get<std::string>();
    if (action == "noop") {
      while (luciole::CopyReceipt::cleanupPending()
             || luciole::CreateReceipt::cleanupPending()
             || luciole::MoveReceipt::cleanupPending()) {
        g_main_context_iteration(nullptr, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      return luciole::NetworkMessage{{"cleanupErrors", luciole::CopyReceipt::takeCleanupErrors()}};
    }
    if (action == "operation" || action == "reverse" || action == "historyMount") {
      return luciole::networkOperationRequest(request, event, descriptor);
    }
    if (action == "templates") {
      const auto value = luciole::readTemplates(luciole::CancelToken{});
      return luciole::NetworkMessage{{"items", value.items}, {"error", value.error}, {"truncated", value.truncated}};
    }
    if (action == "program") {
      return luciole::networkProgramRequest(request);
    }
    if (action == "icon") {
      return luciole::networkIconRequest(request);
    }
    if (action == "thumbnail") {
      return luciole::networkThumbnailRequest(request);
    }
    if (action == "text") {
      return luciole::networkTextRequest(request);
    }
    if (action == "metadata") {
      return luciole::networkMetadataRequest(request);
    }
    return luciole::networkFilesystemRequest(request, event, descriptor);
  });
}
