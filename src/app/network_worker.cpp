#include "core/application_identity.h"
#include "fs/network_info.h"
#include "imaging/pixbuf_image_decoder.h"
#include "thumbnails/network_preview.h"
#include "thumbnails/thumbnail_worker.h"

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
    const auto action = request.at("action").get<std::string>();
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
