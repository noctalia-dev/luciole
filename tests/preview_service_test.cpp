#include "core/log.h"
#include "fs/gio_interop.h"
#include "test_check.h"
#include "thumbnails/preview_service.h"
#include "thumbnails/thumbnail_worker.h"
#include "ui/browser/preview_layout.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <tuple>

namespace {
  void wait(auto predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("preview timed out");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  luciole::DirectoryEntry entry(const std::filesystem::path& path, std::string mime = "text/plain") {
    luciole::DirectoryEntry result{
        .id = 1,
        .location = luciole::Location::fromPath(path.native()),
        .name = path.filename().native(),
        .displayName = path.filename().native(),
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt,
        .etag = std::nullopt,
        .parentDisplayName = {}
    };
    result.id = 1;
    result.location = luciole::Location::fromPath(path.native());
    result.name = path.filename().native();
    result.displayName = result.name;
    result.type = luciole::FileType::Regular;
    result.contentType = std::move(mime);
    struct stat stat{};
    if (::lstat(path.c_str(), &stat) == 0) {
      result.size = static_cast<std::uint64_t>(stat.st_size);
      result.modifiedSeconds = static_cast<std::uint64_t>(stat.st_mtim.tv_sec);
    }
    return result;
  }
  void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
  }
  void svgChecks(const std::filesystem::path& root) {
    const auto path = root / "preview.svg";
    write(
        path,
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 400 200'>"
        "<rect width='400' height='200' fill='#ff2244'/></svg>"
    );
    const auto source = entry(path, "image/svg+xml");
    luciole::ThumbnailService grid;
    const auto seed = grid.request(source, 128);
    wait([&] { return seed && seed->ready; });
    TEST_CHECK(seed->pixels && seed->pixels->width == 128);
    luciole::PreviewService preview;
    preview.show(source, 2048, {}, seed);
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Ready && preview.snapshot().image == seed);
    wait([&] { return !preview.busy(); });
    TEST_CHECK(preview.snapshot().image && preview.snapshot().image->pixels);
    const auto& image = *preview.snapshot().image->pixels;
    TEST_CHECK(image.width == 2048 && image.height == 1024);
    TEST_CHECK(image.rgba[0] == 255 && image.rgba[1] == 34 && image.rgba[2] == 68 && image.rgba[3] == 255);
    preview.clear();
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Empty);
  }
  void inheritedTextChecks(const std::filesystem::path& root) {
    luciole::PreviewService preview;
    for (const auto& [name, mime, text] :
         {std::tuple{"record.yaml", "application/yaml", "message: hello\n"},
          std::tuple{"record.yml", "application/x-yaml", "message: hello again\n"},
          std::tuple{
              "launcher.desktop", "application/x-desktop",
              "[Desktop Entry]\nType=Application\nName=Mail App\nExec=false\n"
          },
          std::tuple{"data.json", "application/json", "{\"message\": \"hello\"}\n"},
          std::tuple{"flake.nix", "application/x-nix", "{ description = \"Example\"; }\n"}}) {
      const auto path = root / name;
      write(path, text);
      preview.show(entry(path, mime), 512, {});
      wait([&] { return !preview.busy(); });
      TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Ready);
      TEST_CHECK(preview.snapshot().text == text);
      TEST_CHECK(!preview.snapshot().image);
    }
    const auto path = root / "binary.yaml";
    write(path, std::string("binary\0text", 11));
    preview.show(entry(path, "application/yaml"), 512, {});
    wait([&] { return !preview.busy(); });
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Unavailable && preview.snapshot().text.empty());
    preview.clear();
  }
  void checks(const std::filesystem::path& root) {
    const auto portraitLayout = luciole::previewImageLayout(1000, 3000, 320, 1300, 1);
    TEST_CHECK(portraitLayout.pixels >= 846);
    const auto landscapeLayout = luciole::previewImageLayout(3840, 2160, 320, 1300, 1);
    TEST_CHECK(landscapeLayout.pixels <= 320);
    const auto retinaLayout = luciole::previewImageLayout(3840, 2160, 800, 720, 2);
    TEST_CHECK(retinaLayout.pixels > 1024 && retinaLayout.pixels <= 2048);
    luciole::PreviewService preview;
    const auto a = root / "space ' café\n.txt", b = root / "new.txt";
    const auto finish = [&] { wait([&] { return !preview.busy(); }); };
    write(a, "<b>literal</b>\n日本語\n");
    write(b, "current file");
    preview.show(entry(a), 512, {});
    finish();
    TEST_CHECK(
        preview.snapshot().status == luciole::PreviewStatus::Ready
        && preview.snapshot().text == "<b>literal</b>\n日本語\n"
    );
    for (int i = 0; i < 100; ++i) {
      preview.show(entry(i % 2 ? a : b), 512, {});
    }
    preview.show(entry(b), 512, {});
    finish();
    TEST_CHECK(preview.snapshot().text == "current file" && preview.snapshot().entry->location == entry(b).location);
    write(a, std::string(1000000, 'a'));
    preview.show(entry(a), 512, {});
    finish();
    TEST_CHECK(
        preview.snapshot().status == luciole::PreviewStatus::Ready
        && preview.snapshot().text.size() < 9000
        && preview.snapshot().message == "Preview truncated"
    );
    write(b, std::string("binary\0text", 11));
    preview.show(entry(b), 512, {});
    finish();
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Unavailable);
    auto stale = entry(a);
    stale.size = 3;
    preview.show(stale, 512, {});
    finish();
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Error);
    const auto link = root / "link.txt";
    std::filesystem::create_symlink(a, link);
    preview.show(entry(link), 512, {});
    finish();
    TEST_CHECK(preview.snapshot().text.empty());
    const auto fifo = root / "fifo.txt";
    TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    preview.show(entry(fifo), 512, {});
    finish();
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Unavailable);
    auto remote = entry(b);
    remote.location = luciole::Location::fromUri("sftp://example.invalid/file.txt");
    preview.show(remote, 512, {});
    finish();
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Unavailable);
    preview.show(entry(a), 512, {});
    preview.clear();
    finish();
    TEST_CHECK(
        preview.snapshot().status == luciole::PreviewStatus::Empty
        && !preview.snapshot().entry
        && preview.snapshot().text.empty()
    );
    const auto corrupt = root / "corrupt.png";
    write(corrupt, "not an image");
    auto image = entry(corrupt, "image/png");
    preview.show(image, 512, {});
    wait([&] { return preview.snapshot().status != luciole::PreviewStatus::Loading; });
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Unavailable);
    preview.clear();
    finish();
    const auto good = root / "good.png";
    const luciole::GioPtr<GdkPixbuf> pixels(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 8, 8));
    gdk_pixbuf_fill(pixels.get(), 0xff2244ffU);
    TEST_CHECK(gdk_pixbuf_save(pixels.get(), good.c_str(), "png", nullptr, nullptr));
    preview.show(entry(good, "image/png"), 1024, {});
    wait([&] { return preview.snapshot().status != luciole::PreviewStatus::Loading; });
    TEST_CHECK(
        preview.snapshot().status == luciole::PreviewStatus::Ready
        && preview.snapshot().image
        && preview.snapshot().image->pixels
    );
    preview.clear();
    finish();
    luciole::ThumbnailService grid;
    const auto source = entry(good, "image/png");
    const auto loaded = grid.request(source, 128);
    wait([&] { return loaded && loaded->ready; });
    const auto seed = grid.prepared(source);
    TEST_CHECK(seed && seed->pixels);
    preview.show(source, 1024, {}, seed);
    TEST_CHECK(preview.snapshot().status == luciole::PreviewStatus::Ready && preview.snapshot().image == seed);
    auto obsolete = source;
    obsolete.contentRevision = "changed";
    TEST_CHECK(!grid.prepared(obsolete));
    preview.clear();
    finish();
    const auto detailed = root / "detailed.png";
    const luciole::GioPtr<GdkPixbuf> detailPixels(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 2560, 1600));
    gdk_pixbuf_fill(detailPixels.get(), 0x2233ffffU);
    TEST_CHECK(gdk_pixbuf_save(detailPixels.get(), detailed.c_str(), "png", nullptr, nullptr));
    const auto detailSource = entry(detailed, "image/png");
    const auto small = grid.request(detailSource, 128);
    wait([&] { return small && small->ready; });
    TEST_CHECK(small->pixels && small->pixels->width == 128);
    preview.show(detailSource, 512, {}, small);
    TEST_CHECK(preview.snapshot().image == small);
    wait([&] {
      return preview.snapshot().image
          && preview.snapshot().image->pixels
          && preview.snapshot().image->pixels->width >= 512;
    });
    preview.show(detailSource, 1024, {}, small);
    finish();
    TEST_CHECK(
        preview.snapshot().image && preview.snapshot().image->pixels && preview.snapshot().image->pixels->width == 1024
    );
    TEST_CHECK(!preview.snapshot().imageUpgradeFailed);
    preview.show(detailSource, 1536, {}, small);
    finish();
    TEST_CHECK(
        preview.snapshot().image && preview.snapshot().image->pixels && preview.snapshot().image->pixels->width == 2048
    );
    const auto highResolution = preview.snapshot().image;
    preview.show(detailSource, 1400, {}, small);
    TEST_CHECK(preview.snapshot().image == highResolution);
    preview.show(detailSource, 512, {}, small);
    TEST_CHECK(preview.snapshot().image == highResolution);
    const auto ordinary = grid.request(detailSource, 1536);
    wait([&] { return ordinary && ordinary->ready; });
    TEST_CHECK(ordinary->pixels && ordinary->pixels->width == 1024);
    preview.clear();
    finish();
    const auto limitedPath = root / "limited.png";
    std::filesystem::copy_file(detailed, limitedPath);
    const auto limitedSource = entry(limitedPath, "image/png");
    const auto fallback = grid.request(limitedSource, 128);
    wait([&] { return fallback && fallback->ready; });
    luciole::ThumbnailOptions limited;
    limited.maxMegapixels = 1;
    preview.show(limitedSource, 512, limited, fallback);
    finish();
    TEST_CHECK(
        preview.snapshot().status == luciole::PreviewStatus::Ready
        && preview.snapshot().image == fallback
        && preview.snapshot().image->pixels
        && preview.snapshot().image->pixels->width == 128
        && preview.snapshot().message == "Larger preview unavailable."
        && preview.snapshot().imageUpgradeFailed
    );
    preview.clear();
    finish();
    {
      luciole::PreviewService retired;
      retired.show(entry(a), 512, {});
    }
    write(b, "latest revision");
    preview.show(entry(b), 512, {});
    finish();
    TEST_CHECK(preview.snapshot().text == "latest revision");
  }
} // namespace
int main(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  if (!arguments.empty() && arguments.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(arguments).subspan(1));
  }
  if (!arguments.empty() && arguments.front() == "--preview-metadata-worker") {
    return luciole::runPreviewMetadataWorker(std::span(arguments).subspan(1));
  }
  initLogLevelFromEnvironment();
  const luciole::GString directory(g_dir_make_tmp("luciole-preview-test-XXXXXX", nullptr));
  if (!directory) {
    return 1;
  }
  const std::filesystem::path root(directory.get());
  g_setenv("XDG_CACHE_HOME", (root / "cache").c_str(), true);
  int status = 0;
  try {
    checks(root);
    svgChecks(root);
    inheritedTextChecks(root);
    std::println("preview checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "preview: {}", error.what());
    status = 1;
  }
  std::filesystem::remove_all(root);
  return status;
}
