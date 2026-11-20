#include "core/log.h"
#include "directory_fixture.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "fs/network_info.h"
#include "fs/network_job.h"
#include "imaging/pixbuf_image_decoder.h"
#include "imaging/png_decoder.h"
#include "imaging/raster_decoder.h"
#include "imaging/svg_rasterizer.h"
#include "render/core/image_orientation.h"
#include "render/core/image_resize.h"
#include "test_check.h"
#include "thumbnails/thumbnail_provider.h"
#include "thumbnails/thumbnail_service.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <latch>
#include <png.h>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <webp/encode.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using luciole::GioPtr;
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto limit = std::chrono::steady_clock::now() + 20s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < limit);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    void drain() {
      const auto untilTime = std::chrono::steady_clock::now() + 150ms;
      until([&] { return std::chrono::steady_clock::now() >= untilTime; });
    }

  private:
    GlibPollSource m_source;
  };
  struct Fixture {
    fs::path root;
    Fixture() {
      const luciole::GString temporary(g_dir_make_tmp("luciole-thumbnails-test-XXXXXX", nullptr));
      TEST_CHECK(temporary != nullptr);
      root = temporary.get();
      fs::create_directory(root / "home");
      ::setenv("HOME", (root / "home").c_str(), 1);
      ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
      ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
      ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
      ::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/luciole-thumbnail-test-no-session", 1);
      ::setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/luciole-thumbnail-test-no-system", 1);
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  };
  LoadedImageFile picture(int width = 32, int height = 16) {
    LoadedImageFile image{
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4),
        .width = width,
        .height = height
    };
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
        image.rgba[offset] = x < width / 2 ? 240 : 20;
        image.rgba[offset + 1] = x < width / 2 ? 20 : 240;
        image.rgba[offset + 2] = 40;
        image.rgba[offset + 3] = y < height / 2 ? 128 : 255;
      }
    }
    return image;
  }
  void write(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    TEST_CHECK(static_cast<bool>(stream));
  }
  std::vector<std::uint8_t> png(const LoadedImageFile& image) {
    auto bytes = luciole::writeThumbnailPng({.image = image, .text = {}});
    TEST_CHECK(bytes.has_value());
    return std::move(*bytes);
  }
  luciole::DirectoryEntry source(const fs::path& path, Pump& pump) {
    luciole::DirectorySession session;
    session.open(luciole::Location::fromPath(path.parent_path().native()));
    pump.until([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);
    for (const auto& entry : session.snapshot().entries) {
      if (entry.name == path.filename().native()) {
        return entry;
      }
    }
    throw std::runtime_error("missing thumbnail fixture");
  }
  std::vector<std::uint8_t> sharedPng(luciole::ThumbnailPng thumbnail, int compression) {
    std::vector<std::uint8_t> bytes;
    auto* writer = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    TEST_CHECK(writer != nullptr);
    auto* info = png_create_info_struct(writer);
    TEST_CHECK(info != nullptr);
    if (setjmp(png_jmpbuf(writer)) != 0) {
      png_destroy_write_struct(&writer, &info);
      throw std::runtime_error("cannot write shared-cache PNG fixture");
    }
    png_set_write_fn(
        writer, &bytes,
        [](png_structp stream, png_bytep data, png_size_t length) {
          auto& result = *static_cast<std::vector<std::uint8_t>*>(png_get_io_ptr(stream));
          result.insert(result.end(), data, data + length);
        },
        nullptr
    );
    png_set_IHDR(
        writer, info, thumbnail.image.width, thumbnail.image.height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
    );
    std::vector<png_text> fields;
    for (auto& [key, value] : thumbnail.text) {
      png_text field{};
      field.compression = compression;
      field.key = const_cast<char*>(key.c_str());
      field.text = value.data();
      field.text_length = value.size();
      field.itxt_length = value.size();
      fields.push_back(field);
    }
    png_set_text(writer, info, fields.data(), static_cast<int>(fields.size()));
    png_write_info(writer, info);
    for (int y = 0; y < thumbnail.image.height; ++y) {
      png_write_row(writer, thumbnail.image.rgba.data() + static_cast<std::size_t>(y) * thumbnail.image.width * 4);
    }
    png_write_end(writer, info);
    png_destroy_write_struct(&writer, &info);
    return bytes;
  }
  void testSharedCacheInterop(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "long-path-to-an-existing-third-party-thumbnail.png";
    write(path, png(picture()));
    const auto entry = source(path, pump);
    luciole::ThumbnailStore store;
    fs::create_directories(store.root() / "normal");
    const auto target = store.root() / "normal" / luciole::ThumbnailStore::filename(entry.location().uri());
    luciole::ThumbnailPng foreign{
        .image = picture(),
        .text = {
            {"Thumb::URI", entry.location().uri()},
            {"Thumb::MTime", std::to_string(*entry.modifiedSeconds)},
            {"Thumb::Size", std::to_string(*entry.size)},
            {"Software", "another file manager"}
        }
    };
    std::ranges::fill(foreign.image.rgba, 220);
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    for (const int compression : {PNG_TEXT_COMPRESSION_zTXt, PNG_ITXT_COMPRESSION_NONE, PNG_ITXT_COMPRESSION_zTXt}) {
      const auto bytes = sharedPng(foreign, compression);
      const auto decoded = luciole::readThumbnailPng(bytes, 128);
      TEST_CHECK(decoded && decoded->text == foreign.text && decoded->image.rgba == foreign.image.rgba);
      write(target, bytes);
      struct stat before{}, after{};
      TEST_CHECK(::lstat(target.c_str(), &before) == 0);
      luciole::ThumbnailService service;
      const auto asset = service.request(entry, 256);
      pump.until([&] { return asset && asset->ready; });
      TEST_CHECK(asset->pixels && asset->pixels->rgba == foreign.image.rgba);
      TEST_CHECK(!store.read(entry.location().uri(), 256, *entry.modifiedSeconds, entry.size));
      auto replacement = foreign;
      replacement.image = picture();
      store.write(std::move(replacement), 128, 16U * 1024U * 1024U, cancel.get());
      TEST_CHECK(luciole::readThumbnailFile(target, 8U * 1024U * 1024U) == bytes);
      TEST_CHECK(
          ::lstat(target.c_str(), &after) == 0
          && before.st_ino == after.st_ino
          && before.st_mtim.tv_sec == after.st_mtim.tv_sec
          && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
      );
      TEST_CHECK(store.clear().empty() && fs::exists(target));
    }
    {
      luciole::ThumbnailService service;
      const auto asset = service.request(entry, 256, true, luciole::ThumbnailPurpose::Preview);
      pump.until([&] { return asset && asset->ready; });
      TEST_CHECK(asset->pixels && asset->pixels->rgba != foreign.image.rgba);
      pump.until([&] {
        return store.read(entry.location().uri(), 256, *entry.modifiedSeconds, entry.size).has_value();
      });
      // PNG publication precedes the worker's ledger commit; clearing can still see its write lock.
      pump.until([&] {
        const auto error = store.clear();
        TEST_CHECK(fs::exists(target));
        TEST_CHECK(error.empty() || error.starts_with("private file is busy; retry later"));
        return error.empty();
      });
    }
    // A stale ownership record must not authorize replacing another application's newer file.
    fs::remove(target);
    store.write(foreign, 128, 16U * 1024U * 1024U, cancel.get());
    fs::rename(target, target.string() + ".old");
    const auto replacement = sharedPng(foreign, PNG_TEXT_COMPRESSION_zTXt);
    write(target, replacement);
    store.write(foreign, 128, 16U * 1024U * 1024U, cancel.get());
    TEST_CHECK(luciole::readThumbnailFile(target, 8U * 1024U * 1024U) == replacement);
    TEST_CHECK(store.clear().empty() && fs::exists(target));
    auto oversized = foreign;
    oversized.text["Description"] = std::string(131073, 'x');
    TEST_CHECK(!luciole::readThumbnailPng(sharedPng(std::move(oversized), PNG_TEXT_COMPRESSION_zTXt), 128));
  }

  void testCodecs() {
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto original = picture();
    auto data = png(original);
    auto decoded = luciole::decodeRasterImage(data, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->rgba == original.rgba && decoded->width == 32 && decoded->height == 16);
    TEST_CHECK(!luciole::decodeRasterImage(data, 128, 100, cancel.get()));
    auto small = luciole::decodeRasterImage(data, 8, 1000000, cancel.get());
    TEST_CHECK(small && small->width == 8 && small->height == 4 && small->rgba[3] >= 127 && small->rgba[3] <= 129);
    // Exercise the full-size output allowed by system loaders, independently of their scaling support.
    const GioPtr<GdkPixbuf> fullSize(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1600, 900));
    gdk_pixbuf_fill(fullSize.get(), 0x20408080U);
    auto fitted = luciole::fitRasterPixels(fullSize.get(), 512, 2'000'000);
    TEST_CHECK(
        fitted && fitted->width == 512 && fitted->height == 288 && fitted->rgba[0] == 32 && fitted->rgba[3] == 128
    );
    TEST_CHECK(!luciole::fitRasterPixels(fullSize.get(), 512, 1'000'000));
    const std::string svg =
        "<svg xmlns='http://www.w3.org/2000/svg'><image href='https://invalid.example/a.png'/></svg>";
    TEST_CHECK(!luciole::decodeRasterImage(
        {reinterpret_cast<const std::uint8_t*>(svg.data()), svg.size()}, 128, 1000000, cancel.get()
    ));
    std::uint8_t* webp = nullptr;
    const auto length =
        WebPEncodeLosslessRGBA(original.rgba.data(), original.width, original.height, original.width * 4, &webp);
    TEST_CHECK(length > 0);
    decoded = luciole::decodeRasterImage({webp, length}, 128, 1000000, cancel.get());
    WebPFree(webp);
    // Non-Glycin WebP loaders round-trip through premultiplied alpha, so translucent channels may drift by one.
    TEST_CHECK(decoded && std::ranges::equal(decoded->rgba, original.rgba, [](std::uint8_t a, std::uint8_t b) {
                 return std::abs(a - b) <= 1;
               }));
    const GioPtr<GdkPixbuf> pixbuf(gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 32, 16));
    gdk_pixbuf_fill(pixbuf.get(), 0xff3020ffU);
    gchar* jpeg = nullptr;
    gsize jpegSize = 0;
    TEST_CHECK(gdk_pixbuf_save_to_buffer(pixbuf.get(), &jpeg, &jpegSize, "jpeg", nullptr, "quality", "100", nullptr));
    std::vector<std::uint8_t> oriented(
        reinterpret_cast<std::uint8_t*>(jpeg), reinterpret_cast<std::uint8_t*>(jpeg) + jpegSize
    );
    g_free(jpeg);
    const std::array<std::uint8_t, 36> exif{0xff, 0xe1, 0, 34, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1,
                                            0,    0x12, 1, 3,  0,   1,   0,   0,   0, 6, 0,   0,   0,  0, 0, 0, 0};
    oriented.insert(oriented.begin() + 2, exif.begin(), exif.end());
    decoded = luciole::decodeRasterImage(oriented, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->width == 16 && decoded->height == 32);
    TEST_CHECK(!luciole::readThumbnailPng(std::span(data).first(20), 128));
    data[20] ^= 1;
    TEST_CHECK(!luciole::readThumbnailPng(data, 128));
    g_cancellable_cancel(cancel.get());
    TEST_CHECK(!luciole::decodeRasterImage(png(original), 128, 1000000, cancel.get()));
  }
  void testRasterFormats() {
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const std::vector<std::uint8_t> gif{'G', 'I',  'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0,    0, 0, 255, 255,
                                        255, 0x2c, 0,   0,   0,   0,   1, 0, 1, 0, 0,    2, 2, 0x44, 1, 0, 0x3b};
    auto decoded = luciole::decodeRasterImage(gif, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->width == 1 && decoded->height == 1 && decoded->rgba[3] == 255);
    const std::vector<std::uint8_t> pnm{'P', '6', '\n', '2', ' ', '1', '\n', '2', '5', '5', '\n', 255, 0, 0, 0, 255, 0};
    decoded = luciole::decodeRasterImage(pnm, 128, 1000000, cancel.get());
    TEST_CHECK(
        decoded && decoded->width == 2 && decoded->height == 1 && decoded->rgba[0] == 255 && decoded->rgba[5] == 255
    );
    std::vector<std::uint8_t> bmp(62);
    const auto put32 = [](auto& data, std::size_t offset, std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
      }
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(bmp, 2, 62);
    put32(bmp, 10, 54);
    put32(bmp, 14, 40);
    put32(bmp, 18, 2);
    put32(bmp, 22, 1);
    bmp[26] = 1;
    bmp[28] = 24;
    bmp[56] = 255;
    bmp[58] = 255;
    decoded = luciole::decodeRasterImage(bmp, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->width == 2 && decoded->rgba[0] == 255 && decoded->rgba[5] == 255);
    const auto pngData = png(picture());
    std::vector<std::uint8_t> ico(22);
    ico[2] = 1;
    ico[4] = 1;
    ico[6] = 32;
    ico[7] = 16;
    ico[10] = 1;
    ico[12] = 32;
    put32(ico, 14, static_cast<std::uint32_t>(pngData.size()));
    put32(ico, 18, 22);
    ico.insert(ico.end(), pngData.begin(), pngData.end());
    decoded = luciole::decodeRasterImage(ico, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->rgba == picture().rgba);
    // Verify both PNG-backed and 32-bit DIB icon transparency.
    ico.resize(22);
    ico[6] = 2;
    ico[7] = 1;
    put32(ico, 14, 52);
    std::vector<std::uint8_t> dib(52);
    put32(dib, 0, 40);
    put32(dib, 4, 2);
    put32(dib, 8, 2);
    dib[12] = 1;
    dib[14] = 32;
    dib[42] = 255;
    dib[43] = 128;
    dib[45] = 255;
    dib[47] = 255;
    ico.insert(ico.end(), dib.begin(), dib.end());
    decoded = luciole::decodeRasterImage(ico, 128, 1000000, cancel.get());
    TEST_CHECK(
        decoded
        && decoded->width == 2
        && decoded->height == 1
        && decoded->rgba[0] == 255
        && decoded->rgba[3] == 128
        && decoded->rgba[5] == 255
    );
    TEST_CHECK(!luciole::decodeRasterImage(ico, 128, 1, cancel.get()));
    put32(ico, 26, 0x80000000U);
    TEST_CHECK(!luciole::decodeRasterImage(ico, 128, 1000000, cancel.get()));
    const std::array<std::uint8_t, 8> transparentRedBlue{255, 0, 0, 0, 0, 0, 255, 255};
    const auto resized = resizeToRgba(transparentRedBlue.data(), 2, 1, ResizeSourceLayout::Rgba, 1, 1);
    TEST_CHECK(resized.size() == 4 && resized[0] < 5 && resized[2] > 250 && resized[3] >= 125 && resized[3] <= 130);
    auto corrupt = pngData;
    corrupt[20] ^= 1;
    TEST_CHECK(!luciole::decodeRasterImage(corrupt, 128, 1000000, cancel.get()));
    TEST_CHECK(!luciole::decodePng(pngData, 100));
    const auto theme = luciole::decodePng(pngData);
    TEST_CHECK(theme && theme->pixels == picture().rgba);
    luciole::installPixbufImageDecoder();
    const auto providerBytes = png(picture(128, 64));
    const luciole::GString base64(g_base64_encode(providerBytes.data(), providerBytes.size()));
    const std::string uri = "data:image/png;base64," + std::string(base64.get());
    const auto cropped = loadImageFile(uri, 32, true);
    TEST_CHECK(cropped && cropped->width == 32 && cropped->height == 32);
    TEST_CHECK(cropped->sourceWidth == 128 && cropped->sourceHeight == 64);
    const auto bounded = loadImageFile(uri, 32, false);
    TEST_CHECK(bounded && bounded->width == 32 && bounded->height == 16);
    const auto original = decodeRasterImage(providerBytes.data(), providerBytes.size());
    TEST_CHECK(original && original->width == 128 && original->height == 64);
    TEST_CHECK(!loadImageFile("data:image/png;base64,invalid!", 32));
    TEST_CHECK(!decodeRasterImage(nullptr, 0));
  }
  void benchmarkThumbnails(const fs::path& directory, std::size_t limit, bool videos) {
    TEST_CHECK(limit >= 1 && limit <= 256);
    Pump pump;
    luciole::DirectorySession session;
    session.open(luciole::Location::fromPath(fs::absolute(directory).native()));
    pump.until([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);
    std::vector<luciole::DirectoryEntry> entries(session.snapshot().entries.begin(), session.snapshot().entries.end());
    std::erase_if(entries, [videos](const auto& entry) {
      return entry.type != luciole::FileType::Regular
          || !entry.contentType
          || !entry.contentType->starts_with(videos ? "video/" : "image/")
          || entry.symlink;
    });
    std::ranges::sort(entries, {}, &luciole::DirectoryEntry::name);
    if (entries.size() > limit) {
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(limit), entries.end());
    }
    TEST_CHECK(!entries.empty());
    auto service = std::make_unique<luciole::ThumbnailService>();
    const auto phase = [&](const char* name) {
      std::vector<std::shared_ptr<const luciole::FileIconImage>> assets;
      assets.reserve(entries.size());
      std::vector<double> elapsed(entries.size(), -1);
      const auto started = std::chrono::steady_clock::now();
      for (const auto& entry : entries) {
        assets.push_back(service->request(entry, 128));
      }
      pump.until([&] {
        bool done = true;
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < assets.size(); ++i) {
          TEST_CHECK(assets[i] != nullptr);
          if (!assets[i]->ready) {
            done = false;
          } else if (elapsed[i] < 0) {
            elapsed[i] = std::chrono::duration<double, std::milli>(now - started).count();
          }
        }
        return done;
      });
      TEST_CHECK(std::ranges::all_of(assets, [](const auto& image) { return static_cast<bool>(image->pixels); }));
      std::ranges::sort(elapsed);
      std::println(
          "{} count={} first={:.2f} median={:.2f} last={:.2f} ms", name, entries.size(), elapsed.front(),
          elapsed[elapsed.size() / 2], elapsed.back()
      );
    };
    phase("cold-thumbnail-cache");
    const auto cache = luciole::ThumbnailStore{}.root() / "normal";
    pump.until([&] {
      return std::ranges::all_of(entries, [&](const auto& entry) {
        return fs::exists(cache / luciole::ThumbnailStore::filename(entry.location().uri()));
      });
    });
    // A fresh service starts with an empty memory cache.
    service = std::make_unique<luciole::ThumbnailService>();
    phase("warm-disk");
    phase("warm-memory");
  }
  void testWorkerTransport() {
    std::vector<std::uint8_t> valid{'N', 'F', 'R', '1', 0, 0, 0, 1, 0, 0, 0, 1, 240, 20, 40, 128};
    const auto result = luciole::readThumbnailWorkerResult(valid, 128);
    TEST_CHECK(
        result
        && result->width == 1
        && result->height == 1
        && result->rgba == std::vector<std::uint8_t>({240, 20, 40, 128})
    );
    auto malformed = valid;
    malformed[0] = 'X';
    TEST_CHECK(!luciole::readThumbnailWorkerResult(malformed, 128));
    malformed = valid;
    malformed.pop_back();
    TEST_CHECK(!luciole::readThumbnailWorkerResult(malformed, 128));
    malformed = valid;
    malformed.push_back(0);
    TEST_CHECK(!luciole::readThumbnailWorkerResult(malformed, 128));
    malformed = valid;
    malformed[4] = 1;
    TEST_CHECK(!luciole::readThumbnailWorkerResult(malformed, 128));
    TEST_CHECK(!luciole::readThumbnailWorkerResult(valid, 0));
    const std::string diagnostic = "NFE1GdkPixbuf: decoder unavailable";
    const auto failed = luciole::readThumbnailWorkerResult({diagnostic.begin(), diagnostic.end()}, 128);
    TEST_CHECK(!failed && failed.error() == diagnostic.substr(4));
    auto oversized = std::vector<std::uint8_t>(1025, 'a');
    std::copy_n(diagnostic.begin(), 4, oversized.begin());
    TEST_CHECK(luciole::readThumbnailWorkerResult(oversized, 128).error() != std::string(1021, 'a'));
  }
  void testBatchAndLargerCache() {
    luciole::ThumbnailStore store;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto uri = luciole::Location::fromPath("/tmp/cache-larger-only.png").uri();
    const auto second = luciole::Location::fromPath("/tmp/cache-batch-second.png").uri();
    const auto make = [](const std::string& key) {
      return luciole::ThumbnailPng{
          .image = picture(256, 128),
          .text = {{"Thumb::URI", key}, {"Thumb::MTime", "123"}, {"Thumb::Size", "42"}, {"X-Luciole::Stamp", "same"}}
      };
    };
    const std::array writes{
        luciole::ThumbnailCacheWrite{make(uri), 256}, luciole::ThumbnailCacheWrite{make(second), 512}
    };
    store.writeBatch(writes, 16U * 1024U * 1024U, cancel.get());
    const auto cached = store.read(uri, 128, 123, 42, "same");
    TEST_CHECK(
        cached
        && cached->image.width == 128
        && cached->image.height == 64
        && cached->image.rgba[0] == 240
        && cached->image.rgba[3] == 128
    );
    TEST_CHECK(store.read(second, 128, 123, 42, "same").has_value() && !store.read(uri, 128, 123, 42, "changed"));
    TEST_CHECK(store.clear().empty());
  }
  void testCache() {
    luciole::ThumbnailStore store;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto uri = luciole::Location::fromPath("/tmp/literal space\nimage.png").uri();
    const auto make = [&](const std::string& key) {
      return luciole::ThumbnailPng{
          .image = picture(), .text = {{"Thumb::URI", key}, {"Thumb::MTime", "123"}, {"Thumb::Size", "42"}}
      };
    };
    store.write(make(uri), 128, 16U * 1024U * 1024U, cancel.get());
    const auto path = store.root() / "normal" / luciole::ThumbnailStore::filename(uri);
    TEST_CHECK(fs::exists(path) && luciole::ThumbnailStore::filename("abc") == "900150983cd24fb0d6963f7d28e17f72.png");
    const auto cached = store.read(uri, 128, 123, 42);
    TEST_CHECK(cached && cached->image.rgba == picture().rgba);
    const GioPtr<GdkPixbuf> independent(gdk_pixbuf_new_from_file(path.c_str(), nullptr));
    const char* tag = independent ? gdk_pixbuf_get_option(independent.get(), "tEXt::Thumb::URI") : nullptr;
    TEST_CHECK(tag && uri == tag);
    TEST_CHECK(!store.read(uri, 128, 124, 42) && !store.read(uri, 128, 122, 42) && !store.read(uri, 128, 123, 43));
    struct stat mode{};
    TEST_CHECK(::stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600);
    auto foreign = make("sftp://example.invalid/photo.png");
    auto bytes = luciole::writeThumbnailPng(foreign);
    TEST_CHECK(bytes.has_value());
    const auto foreignPath = store.root() / "normal" / luciole::ThumbnailStore::filename(foreign.text.at("Thumb::URI"));
    write(foreignPath, *bytes);
    TEST_CHECK(store.clear().empty() && !fs::exists(path) && fs::exists(foreignPath));
    store.write(make(uri), 256, 16U * 1024U * 1024U, cancel.get());
    const auto large = store.root() / "large" / luciole::ThumbnailStore::filename(uri);
    TEST_CHECK(fs::exists(large));
    fs::rename(large, large.string() + ".old");
    write(large, *bytes);
    TEST_CHECK(store.clear().empty() && fs::exists(large));
    auto bad = make(uri);
    bad.text.erase("Thumb::MTime");
    bytes = luciole::writeThumbnailPng(bad);
    TEST_CHECK(bytes.has_value());
    write(path, *bytes);
    TEST_CHECK(!store.read(uri, 128, 123, 42));
  }
  void testCacheLimits() {
    luciole::ThumbnailStore store;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto make = [](const std::string& uri, const std::string& stamp) {
      return luciole::ThumbnailPng{
          .image = picture(),
          .text = {{"Thumb::URI", uri}, {"Thumb::MTime", "123"}, {"Thumb::Size", "42"}, {"X-Luciole::Stamp", stamp}}
      };
    };
    const auto first = luciole::Location::fromPath("/tmp/cache-one.png").uri();
    const auto second = luciole::Location::fromPath("/tmp/cache-two.png").uri();
    store.write(make(first, "source-one"), 128, 1000000, cancel.get());
    TEST_CHECK(
        store.read(first, 128, 123, 42, "source-one").has_value() && !store.read(first, 128, 123, 42, "source-two")
    );
    const auto one = store.root() / "normal" / luciole::ThumbnailStore::filename(first);
    const auto budget = fs::file_size(one) + 32;
    store.write(make(second, "source-two"), 128, budget, cancel.get());
    const auto two = store.root() / "normal" / luciole::ThumbnailStore::filename(second);
    TEST_CHECK(!fs::exists(one) && fs::exists(two) && fs::file_size(two) <= budget);
    TEST_CHECK(store.clear().empty());
  }
  void command(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    const GioPtr<GSubprocess> process(g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr
    ));
    TEST_CHECK(process && g_subprocess_wait_check(process.get(), nullptr, nullptr));
  }
  void testProviders(const Fixture& fixture) {
    const auto pngPath = fixture.root / "artwork.png";
    write(pngPath, png(picture()));
    const auto video = fixture.root / "clip.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=96x48:d=0.2", "-threads", "1", "-c:v",
         "mpeg4", video.native()}
    );
    const auto shortVideo = fixture.root / "single-frame.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=96x48:r=30", "-frames:v", "1", "-c:v",
         "libx264", "-threads", "1", "-preset", "ultrafast", shortVideo.native()}
    );
    const auto longGop = fixture.root / "long-gop.mp4";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "color=c=blue:s=192x96:r=30",
         "-frames:v",
         "60",
         "-c:v",
         "libx264",
         "-threads",
         "1",
         "-preset",
         "veryfast",
         "-g",
         "120",
         "-keyint_min",
         "120",
         "-bf",
         "3",
         "-sc_threshold",
         "0",
         longGop.native()}
    );
    const auto rotatedVideo = fixture.root / "rotated.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-display_rotation:v:0", "90", "-i", longGop.native(), "-c", "copy",
         rotatedVideo.native()}
    );
    const auto intraVideo = fixture.root / "intra.avi";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=blue:s=192x96:d=0.12", "-c:v", "mjpeg",
         "-threads", "1", intraVideo.native()}
    );
    const auto coveredVideo = fixture.root / "covered.mp4";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-i",
         pngPath.native(),
         "-i",
         longGop.native(),
         "-map",
         "0:v",
         "-map",
         "1:v",
         "-c:v:0",
         "png",
         "-disposition:v:0",
         "attached_pic",
         "-c:v:1",
         "copy",
         "-disposition:v:1",
         "default",
         "-threads",
         "1",
         coveredVideo.native()}
    );
    const auto audio = fixture.root / "artwork.mp3";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "sine=duration=0.2",
         "-i",
         pngPath.native(),
         "-map",
         "0:a",
         "-map",
         "1:v",
         "-c:a",
         "libmp3lame",
         "-c:v",
         "png",
         "-threads",
         "1",
         "-disposition:v",
         "attached_pic",
         audio.native()}
    );
    const auto plain = fixture.root / "plain.wav";
    command({"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "sine=duration=0.1", plain.native()});
    const auto document = fixture.root / "page.pdf";
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets{0};
    const std::string drawing = "1 0 0 rg 0 0 160 80 re f\n";
    const std::array objects{
        std::string("<< /Type /Catalog /Pages 2 0 R >>"), std::string("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        std::string("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 160 80] /Contents 4 0 R >>"),
        "<< /Length " + std::to_string(drawing.size()) + " >>\nstream\n" + drawing + "endstream"
    };
    for (const auto& object : objects) {
      offsets.push_back(pdf.size());
      pdf += std::to_string(offsets.size() - 1) + " 0 obj\n" + object + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (std::size_t i = 1; i < offsets.size(); ++i) {
      const auto value = std::to_string(offsets[i]);
      pdf += std::string(10 - value.size(), '0') + value + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    std::ofstream(document, std::ios::binary) << pdf;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto render = [&](const fs::path& path, const std::string& mime) {
      const int input = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      TEST_CHECK(input >= 0);
      auto result = luciole::renderThumbnailProvider(input, mime, 128, {}, cancel.get());
      ::close(input);
      return result;
    };
    const auto corrupt = fixture.root / "broken.png";
    std::ofstream(corrupt, std::ios::binary) << "not an image";
    const auto failedImage = render(corrupt, "image/png");
    TEST_CHECK(
        !failedImage && failedImage.error().contains("GdkPixbuf") && !failedImage.error().contains(corrupt.native())
    );
    const auto rasterSource = fixture.root / "format-source.png";
    write(rasterSource, png(picture(128, 64)));
    const auto jxl = fixture.root / "format.jxl";
    // Reuse Noctalia's synthetic lossless JPEG XL fixture.
    constexpr std::array<std::uint8_t, 100> jxlBytes = {
        0xFF, 0x0A, 0x18, 0x10, 0xB0, 0x12, 0x08, 0x10, 0x10, 0x00, 0x60, 0x01, 0x4B, 0x28, 0x24, 0xC6, 0x15,
        0x0A, 0x0A, 0x12, 0x4E, 0x60, 0x4C, 0x19, 0x7B, 0x62, 0x72, 0x01, 0x81, 0x00, 0x24, 0xE7, 0x87, 0x96,
        0x2F, 0xBF, 0x63, 0x00, 0xF7, 0x27, 0x80, 0xF3, 0x53, 0x3F, 0xF9, 0x23, 0x39, 0x92, 0x24, 0x39, 0xF7,
        0x27, 0x00, 0xEA, 0x27, 0x7F, 0x40, 0x72, 0x24, 0x49, 0x3A, 0xF6, 0xFD, 0xF3, 0xAF, 0x1F, 0xF2, 0xAF,
        0x3F, 0xFF, 0xFA, 0xF3, 0xDF, 0x9D, 0x7F, 0xFD, 0xBB, 0x77, 0x1F, 0x3C, 0x00, 0x02, 0xE0, 0xFC, 0xE4,
        0x4F, 0xFE, 0x48, 0x1F, 0x93, 0xF1, 0xF3, 0x8C, 0xDD, 0x09, 0x13, 0x61, 0xC6, 0xF3, 0x05,
    };
    constexpr std::array<std::uint8_t, 64> jxlRgba = {
        255, 0,   0,   255, 0,   255, 0,  255, 0,  0,   255, 255, 255, 255, 0,  255, 255, 0,   255, 255, 0,   255,
        255, 255, 255, 255, 255, 255, 0,  0,   0,  255, 128, 0,   0,   128, 0,  128, 0,   128, 0,   0,   128, 128,
        128, 128, 0,   128, 10,  20,  30, 0,   40, 50,  60,  0,   70,  80,  90, 0,   100, 110, 120, 0,
    };
    write(jxl, jxlBytes);
    auto richImage = render(jxl, "image/jxl");
    if (!richImage) {
      std::println(stderr, "JPEG XL fixture error: {}", richImage.error());
    }
    TEST_CHECK(
        richImage
        && richImage->width == 4
        && richImage->height == 4
        && richImage->rgba == std::vector<std::uint8_t>(jxlRgba.begin(), jxlRgba.end())
    );
    const auto avif = fixture.root / "format.avif";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-i", rasterSource.native(), "-frames:v", "1", "-c:v", "libaom-av1",
         "-cpu-used", "8", "-still-picture", "1", "-pix_fmt", "yuv444p", "-threads", "1", avif.native()}
    );
    richImage = render(avif, "image/avif");
    if (!richImage) {
      std::println(stderr, "AVIF fixture error: {}", richImage.error());
    }
    TEST_CHECK(richImage && richImage->width == 128 && richImage->height == 64 && richImage->rgba[0] > 180);
    const auto animated = fixture.root / "animated.webp";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "color=c=red:s=64x32:r=2:d=0.5",
         "-f",
         "lavfi",
         "-i",
         "color=c=blue:s=64x32:r=2:d=0.5",
         "-filter_complex",
         "[0:v][1:v]concat=n=2:v=1:a=0",
         "-filter_complex_threads",
         "1",
         "-c:v",
         "libwebp_anim",
         "-lossless",
         "1",
         "-loop",
         "0",
         "-threads",
         "1",
         animated.native()}
    );
    richImage = render(animated, "image/webp");
    TEST_CHECK(
        richImage
        && richImage->width == 64
        && richImage->height == 32
        && richImage->rgba[0] > 220
        && richImage->rgba[2] < 30
    );
    auto still = render(video, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba.front() > 220);
    still = render(shortVideo, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[0] > 220);
    still = render(longGop, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[2] > 220);
    still = render(rotatedVideo, "video/mp4");
    TEST_CHECK(still && still->width == 64 && still->height == 128 && still->rgba[2] > 220);
    still = render(intraVideo, "video/x-msvideo");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[2] > 220);
    still = render(coveredVideo, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[2] > 220 && still->rgba[0] < 30);
    still = render(document, "application/pdf");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba.front() > 220);
    still = render(audio, "audio/mpeg");
    TEST_CHECK(still && still->width == 128 && still->height == 64);
    TEST_CHECK(!render(plain, "audio/x-wav"));
    // Seekable media must not inherit the whole-file image/PDF input cap.
    // Sparse padding gives these real fixtures a source size above the default 64 MiB limit.
    Pump pump;
    luciole::ThumbnailService service;
    auto options = luciole::ThumbnailOptions{};
    options.policy = luciole::ThumbnailPolicy::All;
    options.maxFileMiB = 1;
    service.setOptions(options);
    for (const auto& original : {video, audio}) {
      const auto padded = fixture.root / ("padded-" + original.filename().native());
      fs::copy_file(original, padded);
      fs::resize_file(padded, 65U * 1024U * 1024U);
      const auto entry = source(padded, pump);
      TEST_CHECK(entry.size && *entry.size > 64U * 1024U * 1024U);
      const auto thumbnail = service.request(entry, 128);
      pump.until([&] { return thumbnail && thumbnail->ready; });
      TEST_CHECK(thumbnail->pixels && thumbnail->pixels->width > 0 && thumbnail->pixels->height > 0);
    }
    const auto oversizedImage = fixture.root / "byte-limited.png";
    write(oversizedImage, png(picture()));
    fs::resize_file(oversizedImage, 2U * 1024U * 1024U);
    const auto limitedImage = service.request(source(oversizedImage, pump), 128);
    pump.until([&] { return limitedImage && limitedImage->ready; });
    TEST_CHECK(!limitedImage->pixels);

    const std::string originalPath = std::getenv("PATH");
    ::setenv("PATH", "/luciole-thumbnail-test-no-tools", 1);
    TEST_CHECK(!render(document, "application/pdf") && !render(video, "video/mp4"));
    ::setenv("PATH", originalPath.c_str(), 1);
    const auto bin = fixture.root / "slow-bin";
    fs::create_directory(bin);
    const luciole::GString sleep(g_find_program_in_path("sleep"));
    TEST_CHECK(sleep != nullptr);
    const luciole::GString shell(g_find_program_in_path("sh"));
    TEST_CHECK(shell != nullptr);
    std::ofstream(bin / "pdftoppm") << "#!" << shell.get() << "\nexec " << sleep.get() << " 30\n";
    TEST_CHECK(::chmod((bin / "pdftoppm").c_str(), 0700) == 0);
    ::setenv("PATH", bin.c_str(), 1);
    const auto started = std::chrono::steady_clock::now();
    std::thread cancelling([&] {
      std::this_thread::sleep_for(100ms);
      g_cancellable_cancel(cancel.get());
    });
    still = render(document, "application/pdf");
    cancelling.join();
    g_cancellable_reset(cancel.get());
    const auto timeoutStarted = std::chrono::steady_clock::now();
    still = render(document, "application/pdf");
    ::setenv("PATH", originalPath.c_str(), 1);
    TEST_CHECK(timeoutStarted - started < 2s);
    const auto elapsed = std::chrono::steady_clock::now() - timeoutStarted;
    TEST_CHECK(!still && elapsed >= 7s && elapsed < 12s);
  }
  void testVideoFilmstrips(const Fixture& fixture) {
    const luciole::GString helper(g_find_program_in_path("ffmpegthumbnailer"));
    TEST_CHECK(helper);
    const auto video = fixture.root / "filmstrip café\n.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=128x64:r=10:d=1", "-c:v", "mpeg4",
         "-threads", "1", video.native()}
    );
    const int input = ::open(video.c_str(), O_RDONLY | O_CLOEXEC);
    TEST_CHECK(input >= 0);
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto render = [&](luciole::ThumbnailPurpose purpose) {
      return luciole::renderThumbnailProvider(input, "video/mp4", 128, {}, cancel.get(), purpose);
    };
    const auto plain = render(luciole::ThumbnailPurpose::Preview);
    const auto framed = render(luciole::ThumbnailPurpose::Thumbnail);
    TEST_CHECK(plain && framed && plain->width == 128 && plain->height == 64);
    const auto pngInput = fixture.root / "selected-frame.png";
    const auto referencePath = fixture.root / "reference-filmstrip.png";
    write(pngInput, png(*plain));
    command(
        {helper.get(), "-i", pngInput.native(), "-o", referencePath.native(), "-s", "128", "-t", "0", "-f", "-c", "png"}
    );
    const auto bytes = luciole::readThumbnailFile(referencePath, 8U * 1024U * 1024U);
    TEST_CHECK(bytes);
    const auto reference = luciole::readThumbnailPng(*bytes, 128);
    TEST_CHECK(reference && framed->width == reference->image.width && framed->height == reference->image.height);
    TEST_CHECK(framed->rgba == reference->image.rgba && framed->rgba != plain->rgba);
    const auto center = (static_cast<std::size_t>(framed->height / 2) * framed->width + framed->width / 2) * 4;
    TEST_CHECK(framed->rgba[center] > 200 && framed->rgba[center + 1] < 20);

    // Missing and failing optional helpers keep the already selected frame.
    const auto bin = fixture.root / "filmstrip-bin";
    fs::create_directory(bin);
    for (const auto* program : {"ffmpeg", "ffprobe"}) {
      const luciole::GString executable(g_find_program_in_path(program));
      TEST_CHECK(executable);
      fs::create_symlink(executable.get(), bin / program);
    }
    const std::string previousPath = ::getenv("PATH");
    ::setenv("PATH", bin.c_str(), 1);
    const auto missing = render(luciole::ThumbnailPurpose::Thumbnail);
    ::setenv("PATH", previousPath.c_str(), 1);
    TEST_CHECK(missing && missing->rgba == plain->rgba);
    const luciole::GString failing(g_find_program_in_path("false"));
    TEST_CHECK(failing);
    fs::create_symlink(failing.get(), bin / "ffmpegthumbnailer");
    ::setenv("PATH", bin.c_str(), 1);
    const auto failed = render(luciole::ThumbnailPurpose::Thumbnail);
    ::setenv("PATH", previousPath.c_str(), 1);
    TEST_CHECK(failed && failed->rgba == plain->rgba);
    ::close(input);

    Pump pump;
    const auto entry = source(video, pump);
    luciole::ThumbnailService thumbnails;
    const auto thumbnail = thumbnails.request(entry, 128);
    pump.until([&] { return thumbnail && thumbnail->ready; });
    TEST_CHECK(thumbnail->pixels && thumbnail->pixels->rgba == framed->rgba);
    luciole::ThumbnailStore store;
    const auto cachePath = store.root() / "normal" / luciole::ThumbnailStore::filename(entry.location().uri());
    pump.until([&] { return fs::exists(cachePath); });
    const auto cached = luciole::readThumbnailFile(cachePath, 8U * 1024U * 1024U);
    TEST_CHECK(cached);
    const auto preview = thumbnails.request(entry, 128, true, luciole::ThumbnailPurpose::Preview);
    pump.until([&] { return preview && preview->ready; });
    TEST_CHECK(preview->pixels && preview->pixels->rgba == plain->rgba);
    pump.drain();
    TEST_CHECK(luciole::readThumbnailFile(cachePath, 8U * 1024U * 1024U) == cached);
  }

  void testVideoFrameSelection(const Fixture& fixture) {
    const auto intro = fixture.root / "black-intro.mp4";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "color=c=black:s=96x48:r=10:d=2",
         "-f",
         "lavfi",
         "-i",
         "color=c=red:s=96x48:r=10:d=4",
         "-filter_complex",
         "[0:v][1:v]concat=n=2:v=1:a=0[v]",
         "-map",
         "[v]",
         "-c:v",
         "mpeg4",
         "-g",
         "600",
         "-threads",
         "1",
         intro.native()}
    );
    const auto black = fixture.root / "all-black.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=black:s=96x48:r=10:d=2", "-c:v", "mpeg4",
         "-threads", "1", black.native()}
    );
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const int descriptor = ::open(black.c_str(), O_RDONLY | O_CLOEXEC);
    TEST_CHECK(descriptor >= 0);
    auto dark = luciole::renderThumbnailProvider(descriptor, "video/mp4", 128, {}, cancel.get());
    ::close(descriptor);
    TEST_CHECK(dark && dark->rgba[0] < 12 && dark->rgba[1] < 12 && dark->rgba[2] < 12);
    Pump pump;
    const auto entry = source(intro, pump);
    luciole::ThumbnailStore store;
    auto old = picture();
    std::ranges::fill(old.rgba, 0);
    for (std::size_t i = 3; i < old.rgba.size(); i += 4) {
      old.rgba[i] = 255;
    }
    store.write(
        {.image = old,
         .text =
             {{"Thumb::URI", entry.location().uri()},
              {"Thumb::MTime", std::to_string(*entry.modifiedSeconds)},
              {"Thumb::Size", std::to_string(*entry.size)}}},
        128, 16U * 1024U * 1024U, cancel.get()
    );
    luciole::ThumbnailService service;
    const auto asset = service.request(entry, 128);
    pump.until([&] { return asset && asset->ready; });
    TEST_CHECK(asset->pixels);
    const auto center =
        (static_cast<std::size_t>(asset->pixels->height / 2) * asset->pixels->width + asset->pixels->width / 2) * 4;
    TEST_CHECK(asset->pixels->rgba[center] > 200 && asset->pixels->rgba[center + 1] < 20);
    pump.until([&] {
      const auto cached = store.read(entry.location().uri(), 128, *entry.modifiedSeconds, entry.size);
      return cached
          && cached->text.contains("X-Luciole::VideoVersion")
          && cached->text.at("X-Luciole::VideoVersion") == luciole::kVideoThumbnailVersion;
    });
    const auto target = store.root() / "normal" / luciole::ThumbnailStore::filename(entry.location().uri());
    fs::rename(target, target.string() + ".old");
    luciole::ThumbnailPng foreign{
        .image = picture(),
        .text = {
            {"Thumb::URI", entry.location().uri()},
            {"Thumb::MTime", std::to_string(*entry.modifiedSeconds)},
            {"Thumb::Size", std::to_string(*entry.size)},
            {"Software", "ffmpegthumbnailer"}
        }
    };
    const auto bytes = sharedPng(foreign, PNG_TEXT_COMPRESSION_zTXt);
    write(target, bytes);
    luciole::ThumbnailService fresh;
    const auto reused = fresh.request(entry, 128);
    pump.until([&] { return reused && reused->ready; });
    TEST_CHECK(reused->pixels && reused->pixels->rgba == foreign.image.rgba);
    pump.drain();
    TEST_CHECK(luciole::readThumbnailFile(target, 8U * 1024U * 1024U) == bytes);
  }

  void testSvgThumbnails(const Fixture& fixture) {
    const auto path = fixture.root / "vector café\n.svg";
    const auto linked = fixture.root / "linked.png";
    write(linked, png(picture()));
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 400 200'>"
                            "<rect width='200' height='200' fill='red' fill-opacity='.5'/>"
                            "<image x='200' width='200' height='200' href='"
        + luciole::Location::fromPath(linked.native()).uri()
        + "'/><image x='200' width='200' height='200' href='linked.png'/></svg>";
    std::ofstream(path, std::ios::binary) << svg;
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(svg.data()), svg.size());
    TEST_CHECK(!luciole::rasterizeSvg(bytes, 128, false, 79999));
    auto decoded = luciole::rasterizeSvg(bytes, 128, false, 80000);
    TEST_CHECK(decoded && decoded->width == 128 && decoded->height == 64);
    TEST_CHECK(decoded->sourceWidth == 400 && decoded->sourceHeight == 200);
    TEST_CHECK(decoded->rgba[0] == 255 && decoded->rgba[3] == 128);
    TEST_CHECK(decoded->rgba[(32 * 128 + 96) * 4 + 3] == 0);

    const GioPtr<GZlibCompressor> compressor(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6));
    const GioPtr<GOutputStream> buffer(g_memory_output_stream_new_resizable());
    const GioPtr<GOutputStream> compressed(g_converter_output_stream_new(buffer.get(), G_CONVERTER(compressor.get())));
    TEST_CHECK(g_output_stream_write_all(compressed.get(), svg.data(), svg.size(), nullptr, nullptr, nullptr));
    TEST_CHECK(g_output_stream_close(compressed.get(), nullptr, nullptr));
    const auto zipped = fixture.root / "vector.svgz";
    write(
        zipped,
        {static_cast<const std::uint8_t*>(g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(buffer.get()))),
         g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(buffer.get()))}
    );
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto render = [&](const fs::path& file, const std::string& mime, int side) {
      const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
      TEST_CHECK(fd >= 0);
      auto result = luciole::renderThumbnailProvider(fd, mime, side, {}, cancel.get());
      ::close(fd);
      return result;
    };
    auto image = render(path, "image/svg+xml", 2048);
    TEST_CHECK(image && image->width == 2048 && image->height == 1024);
    TEST_CHECK(image->rgba[0] == 255 && image->rgba[3] == 128);
    image = render(zipped, "image/svg+xml-compressed", 128);
    TEST_CHECK(image && image->rgba == decoded->rgba);
    const auto broken = fixture.root / "broken.svg";
    std::ofstream(broken) << "<svg not valid";
    TEST_CHECK(!render(broken, "image/svg+xml", 128));
    g_cancellable_cancel(cancel.get());
    TEST_CHECK(!render(path, "image/svg+xml", 128));
    g_cancellable_reset(cancel.get());

    Pump pump;
    luciole::ThumbnailService service;
    const auto entry = source(zipped, pump);
    TEST_CHECK(entry.contentType == "image/svg+xml-compressed");
    const auto thumbnail = service.request(entry, 128);
    pump.until([&] { return thumbnail && thumbnail->ready; });
    TEST_CHECK(thumbnail->pixels && thumbnail->pixels->rgba == decoded->rgba);
    luciole::ThumbnailStore store;
    pump.until([&] {
      const auto cached = store.read(entry.location().uri(), 128, *entry.modifiedSeconds, entry.size);
      return cached && cached->image.rgba == decoded->rgba;
    });
    const auto fallback = service.request(source(broken, pump), 128);
    pump.until([&] { return fallback && fallback->ready; });
    TEST_CHECK(!fallback->pixels);
    auto unsupported = entry;
    unsupported.contentType = "application/octet-stream";
    TEST_CHECK(!service.request(unsupported, 128));
  }
  void testCacheLanesAndCancellation(const Fixture& fixture) {
    Pump pump;
    // Every blocked copy of the first PDF shares one folder, so releasing it reaches the jobs started later.
    const auto blocked = fixture.root / "blocked";
    fs::create_directory(blocked);
    const auto firstPath = blocked / "slow-one.pdf";
    const auto secondPath = fixture.root / "slow-two.pdf";
    fs::copy_file(fixture.root / "page.pdf", firstPath);
    fs::copy_file(fixture.root / "page.pdf", secondPath);
    const auto first = source(firstPath, pump), second = source(secondPath, pump);
    const auto warm = source(fixture.root / "artwork.png", pump);
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    luciole::ThumbnailStore store;
    store.write(
        {.image = picture(),
         .text =
             {{"Thumb::URI", warm.location().uri()},
              {"Thumb::MTime", std::to_string(*warm.modifiedSeconds)},
              {"Thumb::Size", std::to_string(*warm.size)}}},
        128, 16U * 1024U * 1024U, cancel.get()
    );
    const auto bin = fixture.root / "blocked-bin";
    fs::create_directory(bin);
    const luciole::GString shell(g_find_program_in_path("sh")), sleep(g_find_program_in_path("sleep"));
    TEST_CHECK(shell && sleep);
    const auto outputs = fixture.root / "provider-outputs";
    ::setenv("LUCIOLE_TEST_PROVIDER_OUTPUTS", outputs.c_str(), 1);
    std::ofstream(bin / "pdftoppm")
        << "#!"
        << shell.get()
        << "\nfor output do :; done\nprintf '%s\\n' \"$output\" >> \"$LUCIOLE_TEST_PROVIDER_OUTPUTS\"\nexec "
        << sleep.get()
        << " 30\n";
    TEST_CHECK(::chmod((bin / "pdftoppm").c_str(), 0700) == 0);
    const std::string path = std::getenv("PATH");
    // The blocking pdftoppm shadows the real one; glycin still needs bwrap from the system PATH.
    ::setenv("PATH", (bin.native() + ":" + path).c_str(), 1);
    luciole::ThumbnailService service;
    // Occupy every decoder with a blocked helper.
    const auto decoders = luciole::ThumbnailService::decoders();
    std::vector<std::shared_ptr<const luciole::FileIconImage>> busy{
        service.request(first, 128), service.request(second, 128)
    };
    for (std::uint64_t id = 200; busy.size() < decoders; ++id) {
      const auto copy = blocked / std::format("slow-{}.pdf", id);
      fs::copy_file(firstPath, copy);
      busy.push_back(service.request(source(copy, pump), 128));
    }
    TEST_CHECK(std::ranges::all_of(busy, [](const auto& image) { return image != nullptr; }));
    pump.until([&] { return service.activeJobs() == decoders; });
    auto cached = service.request(warm, 128);
    TEST_CHECK(cached != nullptr);
    pump.until([&] { return cached->ready; });
    TEST_CHECK(cached->pixels && service.activeJobs() == decoders && std::ranges::none_of(busy, [](const auto& image) {
                 return image->ready;
               }));
    // A preview takes the next free decoder ahead of thumbnails that queued before it.
    std::size_t completions = 0;
    service.setChangedCallback([&] { ++completions; });
    std::vector<std::shared_ptr<const luciole::FileIconImage>> queued;
    for (std::uint64_t id = 100; id < 103; ++id) {
      const auto copy = blocked / std::format("slow-{}.pdf", id);
      fs::copy_file(firstPath, copy);
      queued.push_back(service.request(source(copy, pump), 128));
    }
    const auto preview = service.request(warm, 512, true, luciole::ThumbnailPurpose::Preview);
    TEST_CHECK(preview != nullptr);
    pump.until([&] { return completions == queued.size() + 1; });
    pump.until([&] {
      std::ifstream stream(outputs);
      return static_cast<std::size_t>(std::count(std::istreambuf_iterator<char>(stream), {}, '\n')) >= decoders;
    });
    busy.erase(busy.begin());
    service.endFrame();
    pump.until([&] { return preview->ready; });
    TEST_CHECK(preview->pixels && std::ranges::none_of(queued, [](const auto& image) { return image->ready; }));
    // Explicit removal cancels even borrowed images and waits for their actual helper completion. Reads from
    // another location and their leases remain intact.
    const auto blockedLocation = luciole::Location::fromPath(blocked.native());
    service.releaseLocation(blockedLocation);
    TEST_CHECK(service.readingLocation(blockedLocation));
    TEST_CHECK(service.readingLocation(second.location()) && service.touch(busy.front(), true));
    pump.until([&] { return !service.readingLocation(blockedLocation); });
    TEST_CHECK(service.readingLocation(second.location()) && service.touch(busy.front(), true));
    TEST_CHECK(cached->pixels && preview->pixels);
    service.setChangedCallback({});
    queued.clear();
    busy.clear();
    service.endFrame();
    pump.until([&] { return service.activeJobs() == 0; });
    std::ifstream recorded(outputs);
    for (std::string output; std::getline(recorded, output);) {
      TEST_CHECK(!fs::exists(fs::path(output).parent_path()));
    }
    ::unsetenv("LUCIOLE_TEST_PROVIDER_OUTPUTS");
    ::setenv("PATH", path.c_str(), 1);
    service.clearCache();
    pump.until([&] { return !service.clearing(); });
  }
  void testPublicationAndSourceChanges(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "publication.png";
    std::vector<std::uint8_t> original, alternate;
    std::map<std::size_t, std::vector<std::uint8_t>> sizes;
    int color = 0;
    for (int value = 1; value < 240; ++value) {
      auto changed = picture();
      for (std::size_t offset = 0; offset < changed.rgba.size(); offset += 4) {
        changed.rgba[offset] = static_cast<std::uint8_t>(value);
      }
      auto candidate = png(changed);
      const auto found = sizes.find(candidate.size());
      if (found != sizes.end()) {
        original = found->second;
        alternate = std::move(candidate);
        color = value;
        break;
      }
      sizes.emplace(candidate.size(), std::move(candidate));
    }
    TEST_CHECK(!alternate.empty());
    write(path, original);
    auto entry = source(path, pump);
    luciole::ThumbnailService service;
    auto asset = service.request(entry, 128);
    TEST_CHECK(asset != nullptr);
    pump.until([&] { return asset->ready; });
    const auto cachePath =
        luciole::ThumbnailStore{}.root() / "normal" / luciole::ThumbnailStore::filename(entry.location().uri());
    TEST_CHECK(asset->pixels && !fs::exists(cachePath));
    pump.until([&] { return fs::exists(cachePath); });
    luciole::DirectorySnapshot snapshot;
    snapshot.generation = 100;
    snapshot.location = luciole::Location::fromPath(fixture.root.native());
    snapshot.status = luciole::DirectoryStatus::Ready;
    test::editEntries(snapshot, [&](auto& entries) { entries.push_back(entry); });
    service.synchronizeSources(snapshot);
    ++snapshot.generation;
    service.synchronizeSources(snapshot);
    TEST_CHECK(service.touch(asset, true) && service.request(entry, 128) == asset && asset->pixels);
    const auto oldMtime = fs::last_write_time(path);
    write(path, alternate);
    fs::last_write_time(path, oldMtime);
    auto updated = source(path, pump);
    TEST_CHECK(
        updated.size == entry.size
        && updated.modifiedSeconds == entry.modifiedSeconds
        && updated.contentRevision != entry.contentRevision
    );
    snapshot.entries = {updated};
    ++snapshot.generation;
    service.synchronizeSources(snapshot);
    TEST_CHECK(!service.touch(asset, true) && !asset->pixels);
    auto replacement = service.request(updated, 128);
    pump.until([&] { return replacement->ready; });
    TEST_CHECK(replacement != asset && replacement->pixels && replacement->pixels->rgba[0] == color);
  }
  void testService(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "café\nimage \xff.png";
    write(path, png(picture()));
    auto entry = source(path, pump);
    luciole::ThumbnailService thumbnails;
    auto asset = thumbnails.request(entry, 48);
    TEST_CHECK(asset != nullptr);
    ++entry.id;
    TEST_CHECK(thumbnails.request(entry, 48) == asset);
    pump.until([&] { return asset->ready; });
    TEST_CHECK(asset->pixels && asset->pixels->width == 32 && asset->pixels->rgba[3] == 128);
    // A published image is charged its real size, not its size class's full square.
    TEST_CHECK(
        thumbnails.activeJobs() <= luciole::ThumbnailService::decoders()
        && thumbnails.reservedBytes() == asset->pixels->rgba.size()
    );
    // A released thumbnail stays resident: scrolling back to it must not wait for the disk or a decoder.
    asset.reset();
    thumbnails.endFrame();
    pump.drain();
    ++entry.id;
    asset = thumbnails.request(entry, 48);
    TEST_CHECK(asset && asset->ready && asset->pixels);
    TEST_CHECK(thumbnails.prepared(entry) == asset && thumbnails.queuedJobs() == 0);
    const auto stampBefore = fs::last_write_time(path);
    auto replacement = picture();
    std::ranges::fill(replacement.rgba, 255);
    write(path, png(replacement));
    fs::last_write_time(path, stampBefore);
    entry = source(path, pump);
    asset = thumbnails.request(entry, 48);
    pump.until([&] { return asset->ready; });
    TEST_CHECK(asset->pixels && asset->pixels->rgba.front() == 255);
    auto options = luciole::ThumbnailOptions{};
    options.policy = luciole::ThumbnailPolicy::Never;
    thumbnails.setOptions(options);
    TEST_CHECK(!thumbnails.request(entry, 48));
    options.policy = luciole::ThumbnailPolicy::Local;
    thumbnails.setOptions(options);
    std::vector<std::shared_ptr<const luciole::FileIconImage>> held;
    for (std::uint64_t id = 1; id <= luciole::ThumbnailService::kMaxEntries + 1; ++id) {
      auto distinct = entry;
      distinct.place = luciole::Location::fromPath((fixture.root / std::format("budget-{}.png", id)).native());
      if (auto image = thumbnails.request(distinct, 48)) {
        held.push_back(std::move(image));
      }
    }
    TEST_CHECK(
        thumbnails.reservedBytes() <= luciole::ThumbnailService::kResidentBudget
        && held.size() <= luciole::ThumbnailService::kMaxEntries
        && thumbnails.queuedJobs() <= luciole::ThumbnailService::kMaxEntries
    );
    asset.reset();
    held.clear();
    thumbnails.endFrame();
    pump.drain();
    constexpr std::size_t kLarge = luciole::ThumbnailService::kResidentBudget / (1024U * 1024U * 4U);
    std::vector<luciole::DirectoryEntry> largeEntries;
    for (std::size_t i = 0; i <= kLarge; ++i) {
      const auto copy = fixture.root / std::format("overscan-{}.png", i);
      fs::copy_file(path, copy);
      largeEntries.push_back(source(copy, pump));
    }
    std::vector<std::shared_ptr<const luciole::FileIconImage>> overscan;
    for (std::size_t i = 0; i < kLarge; ++i) {
      overscan.push_back(thumbnails.request(largeEntries[i], 1024, false));
      TEST_CHECK(overscan.back() != nullptr);
    }
    auto visible = thumbnails.request(largeEntries.back(), 1024, true);
    TEST_CHECK(visible && thumbnails.reservedBytes() == luciole::ThumbnailService::kResidentBudget);
    TEST_CHECK(!thumbnails.request(largeEntries.front(), 1024, false));
    pump.until([&] { return thumbnails.activeJobs() == luciole::ThumbnailService::decoders(); });
    TEST_CHECK(thumbnails.queuedJobs() == kLarge - luciole::ThumbnailService::decoders());
    visible.reset();
    overscan.clear();
    thumbnails.endFrame();
    pump.drain();
    TEST_CHECK(thumbnails.queuedJobs() == 0 && thumbnails.activeJobs() == 0);
    const auto uncached = fixture.root / "uncached.png";
    write(uncached, png(picture()));
    entry = source(uncached, pump);
    asset = thumbnails.request(entry, 48);
    pump.until([&] { return thumbnails.activeJobs() > 0; });
    thumbnails.clearCache();
    TEST_CHECK(!thumbnails.request(entry, 48));
    pump.until([&] { return !thumbnails.clearing() && thumbnails.activeJobs() == 0; });
    TEST_CHECK(thumbnails.error().empty() && !asset->pixels);
    const auto remote = luciole::Location::fromUri("sftp://example.invalid/photo.png");
    luciole::DirectoryEntry remoteEntry{
        .id = 99,
        .place = remote,
        .name = "photo.png",
        .type = luciole::FileType::Regular,
        .size = 42,
        .modifiedSeconds = 123,
        .contentType = "image/png"
    };
    TEST_CHECK(!thumbnails.request(remoteEntry, 48));
    options.policy = luciole::ThumbnailPolicy::All;
    thumbnails.setOptions(options);
    auto remoteAsset = thumbnails.request(remoteEntry, 48);
    pump.until([&] { return remoteAsset->ready; });
    TEST_CHECK(static_cast<bool>(remoteAsset->pixels));
    auto owner = std::make_unique<luciole::ThumbnailService>();
    auto retiring = owner->request(entry, 48);
    owner->setChangedCallback([&] { owner.reset(); });
    pump.until([&] { return !owner; });
    pump.drain();
    TEST_CHECK(!retiring->pixels);
  }
  void testLocalArtworkDuringNetworkStall(const Fixture& fixture) {
    using namespace luciole;
    Pump pump;
    const auto remote = fixture.root / "share.png", local = fixture.root / "local-art.png";
    write(remote, png(picture()));
    fs::copy_file(remote, local);
    const auto remoteEntry = source(remote, pump), localEntry = source(local, pump);
    registerNetworkFolder(remoteEntry.location());
    std::latch reconnect(1);
    int finished = 0;
    const auto workers = workerLimit(IoLane::NetworkPreviewProbe);
    for (int i = 0; i < workers; ++i) {
      runIoJob<bool>(
          IoLane::NetworkPreviewProbe,
          [&] {
            reconnect.wait();
            return true;
          },
          [&](auto result) {
            TEST_CHECK(result && *result);
            ++finished;
          }
      );
    }
    ThumbnailService thumbnails;
    thumbnails.setOptions({.policy = ThumbnailPolicy::All});
    FileIconCache icons;
    std::vector<std::shared_ptr<const FileIconImage>> waiting;
    for (int size : {128, 256, 512, 1024}) {
      waiting.push_back(thumbnails.request(remoteEntry, size));
      waiting.push_back(icons.request({.names = {}, .filePath = remote.native(), .desktopFile = {}}, size));
    }
    // Dispatch the queued remote probes before the user opens the local folder.
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    const auto thumbnail = thumbnails.request(localEntry, 128);
    const auto icon = icons.request({.names = {}, .filePath = local.native(), .desktopFile = {}}, 48);
    pump.until([&] { return thumbnail->ready && icon->ready; });
    TEST_CHECK(thumbnail->pixels && icon->pixels);
    TEST_CHECK(std::ranges::none_of(waiting, [](const auto& image) { return image->ready; }));
    reconnect.count_down();
    pump.until([&] {
      return finished == workers && std::ranges::all_of(waiting, [](const auto& image) { return image->ready; });
    });
  }
  void testNetworkFilesystemPolicy(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "network.png";
    write(path, png(picture()));
    const auto entry = source(path, pump);
    luciole::ThumbnailService thumbnails([](const std::string&) { return true; });
    auto local = thumbnails.request(entry, 48);
    pump.until([&] { return local->ready; });
    TEST_CHECK(!local->pixels);
    auto options = luciole::ThumbnailOptions{};
    options.policy = luciole::ThumbnailPolicy::All;
    thumbnails.setOptions(options);
    auto all = thumbnails.request(entry, 48);
    pump.until([&] { return all->ready; });
    TEST_CHECK(static_cast<bool>(all->pixels));
  }
} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(args).subspan(1));
  }
  initLogLevelFromEnvironment();
  try {
    Fixture fixture;
    if (!args.empty() && (args.front() == "--benchmark" || args.front() == "--benchmark-videos")) {
      const bool videos = args.front() == "--benchmark-videos";
      TEST_CHECK(args.size() == 2 || args.size() == 3);
      benchmarkThumbnails(std::string(args[1]), args.size() == 3 ? std::stoul(std::string(args[2])) : 48, videos);
      return 0;
    }
    testSharedCacheInterop(fixture);
    testCodecs();
    testRasterFormats();
    testWorkerTransport();
    testCache();
    testCacheLimits();
    testBatchAndLargerCache();
    testProviders(fixture);
    testVideoFilmstrips(fixture);
    testVideoFrameSelection(fixture);
    testSvgThumbnails(fixture);
    testCacheLanesAndCancellation(fixture);
    testPublicationAndSourceChanges(fixture);
    testService(fixture);
    testNetworkFilesystemPolicy(fixture);
    testLocalArtworkDuringNetworkStall(fixture);
    std::println("thumbnail checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "thumbnail test: {}", error.what());
    return 1;
  }
}
