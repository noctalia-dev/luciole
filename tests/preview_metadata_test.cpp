#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "test_check.h"
#include "thumbnails/metadata_probe.h"
#include "thumbnails/preview_metadata.h"
#include "thumbnails/preview_service.h"
#include "thumbnails/thumbnail_worker.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <print>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::Fd;
  using luciole::GioPtr;
  void wait(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void command(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    const GioPtr<GSubprocess> process(g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_STDOUT_SILENCE, nullptr));
    TEST_CHECK(process && g_subprocess_wait_check(process.get(), nullptr, nullptr));
  }
  std::string value(const luciole::MetadataFields& fields, const std::string& key) {
    for (const auto& field : fields) {
      if (field.label == key) {
        return field.value;
      }
    }
    return {};
  }
  luciole::DirectoryEntry entry(const fs::path& path, const std::string& mime) {
    const auto location = luciole::Location::fromPath(path.native());
    const GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(location).get(),
        "standard::size,time::modified,id::file,id::filesystem,etag::value,time::changed,time::changed-nsec,time::"
        "changed-usec",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info != nullptr);
    luciole::DirectoryEntry result{
        .id = 1,
        .place = location,
        .name = path.filename().native(),
        .type = luciole::FileType::Regular,
        .size = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE),
        .modifiedSeconds = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED),
        .contentType = mime,
        .contentRevision = luciole::readChangeRevision(info.get()),
    };
    result.setEtag(g_file_info_get_etag(info.get()));
    result.setIdentity(luciole::readFileIdentity(info.get()));
    return result;
  }
  luciole::MetadataFields inspect(const fs::path& path, const std::string& mime) {
    luciole::PreviewMetadataService metadata;
    metadata.show(entry(path, mime), {});
    wait([&] { return !metadata.busy(); });
    return metadata.fields();
  }
  void parserChecks() {
    const auto fields = luciole::parseMediaMetadata(
        R"({"format":{"format_long_name":"Test","duration":"1.25","bit_rate":"128000","tags":{"title":"<b>literal café","artist":"Artist\nName"}},"streams":[{"codec_type":"video","codec_name":"png","width":1,"height":1,"disposition":{"attached_pic":1}},{"codec_type":"video","codec_name":"h264","width":1920,"height":1080,"avg_frame_rate":"30000/1001","nb_frames":"N/A","side_data_list":[{"rotation":-90}]},{"codec_type":"audio","codec_name":"aac","sample_rate":"48000","channels":2,"channel_layout":"stereo"}]})"
    );
    TEST_CHECK(value(fields, "Duration") == "0:00:01.250");
    TEST_CHECK(value(fields, "Video codec") == "h264" && value(fields, "Frame size") == "1920 × 1080 px");
    TEST_CHECK(value(fields, "Frame rate") == "29.970 fps" && value(fields, "Frames").empty());
    TEST_CHECK(value(fields, "Rotation") == "-90°" && value(fields, "Channels") == "stereo");
    TEST_CHECK(value(fields, "Title") == "<b>literal café" && value(fields, "Artist") == "Artist Name");
    TEST_CHECK(luciole::parseMediaMetadata("broken").empty());
    const auto unknown = luciole::parseMediaMetadata(
        R"({"format":{"duration":"NaN","bit_rate":"-1"},"streams":[{"codec_type":"video","avg_frame_rate":"0/0","width":0}]})"
    );
    TEST_CHECK(
        value(unknown, "Duration").empty()
        && value(unknown, "Frame size").empty()
        && value(unknown, "Frame rate").empty()
    );
    TEST_CHECK(luciole::parseMediaMetadata(std::string(70000, 'x')).empty());
    const auto pdf =
        luciole::parsePdfMetadata("Title: <literal> café\nPages: 3\nPDF version: 1.7\nPage size: 612 x 792 pts\n");
    TEST_CHECK(value(pdf, "Pages") == "3" && value(pdf, "Title") == "<literal> café");
  }
  void writePdf(const fs::path& path) {
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets{0};
    const std::array objects{
        std::string("<< /Type /Catalog /Pages 2 0 R >>"), std::string("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        std::string("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 160 80] >>")
    };
    for (const auto& object : objects) {
      offsets.push_back(pdf.size());
      pdf += std::to_string(offsets.size() - 1) + " 0 obj\n" + object + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 4\n0000000000 65535 f \n";
    for (std::size_t i = 1; i < offsets.size(); ++i) {
      pdf += std::format("{:010} 00000 n \n", offsets[i]);
    }
    pdf += std::format("trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n{}\n%%EOF\n", xref);
    std::ofstream(path, std::ios::binary) << pdf;
  }
  void launcherChecks(const fs::path& root) {
    const auto path = root / "chrome-profile.desktop";
    const auto sentinel = root / "SHOULD_NEVER_RUN";
    std::ofstream(path)
        << "[Desktop Entry]\nType=Application\nName=Mail <literal> café\n"
           "Comment=Read your mail\nExec=touch "
        << sentinel.native()
        << "\nMimeType=text/html;\n";
    auto fields = inspect(path, "application/x-desktop");
    TEST_CHECK(value(fields, "Application") == "Mail <literal> café");
    TEST_CHECK(value(fields, "Description") == "Read your mail");
    TEST_CHECK(std::ranges::any_of(fields, [](const auto& field) {
      return field.label == "Application" && field.preview;
    }));
    TEST_CHECK(!fs::exists(sentinel));
    auto stale = entry(path, "application/x-desktop");
    std::ofstream(path) << "[Desktop Entry]\nType=Application\nName=Changed launcher\nExec=false\n";
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    TEST_CHECK(luciole::probePreviewMetadata(stale, {}, cancel.get()).empty());
    TEST_CHECK(value(inspect(path, "application/x-desktop"), "Application") == "Changed launcher");
    const auto link = root / "launcher-link.desktop";
    fs::create_symlink(path, link);
    TEST_CHECK(value(inspect(link, "application/x-desktop"), "Application").empty());
    std::ofstream(path) << "[Desktop Entry]\nType=Application\nName=Too large\nComment=" << std::string(270000, 'x');
    TEST_CHECK(value(inspect(path, "application/x-desktop"), "Application").empty());
    std::ofstream(path) << "not a desktop entry";
    TEST_CHECK(value(inspect(path, "application/x-desktop"), "Application").empty());
    std::ofstream(path) << "[Desktop Entry]\nType=Link\nName=Example website\nURL=https://example.org/\n";
    fields = inspect(path, "application/x-desktop");
    TEST_CHECK(value(fields, "Link name") == "Example website" && value(fields, "Application").empty());
    g_cancellable_cancel(cancel.get());
    TEST_CHECK(luciole::probePreviewMetadata(entry(path, "application/x-desktop"), {}, cancel.get()).empty());
  }
  void imageChecks(const fs::path& root) {
    const auto path = root / "image <literal> café\n.png";
    const GioPtr<GdkPixbuf> image(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 640, 360));
    gdk_pixbuf_fill(image.get(), 0x20804080U);
    TEST_CHECK(gdk_pixbuf_save(image.get(), path.c_str(), "png", nullptr, nullptr));
    TEST_CHECK(::chmod(path.c_str(), 0640) == 0);
    auto fields = inspect(path, "image/png");
    TEST_CHECK(value(fields, "Pixel size") == "640 × 360 px");
    TEST_CHECK(
        value(fields, "Format").contains("PNG")
        && value(fields, "Bit depth") == "8-bit"
        && value(fields, "Color") == "RGBA"
    );
    TEST_CHECK(value(fields, "Permissions") == "0640");
    TEST_CHECK(std::ranges::any_of(fields, [](const auto& field) {
      return field.label == "Pixel size" && field.preview;
    }));
    TEST_CHECK(std::ranges::none_of(fields, [](const auto& field) {
      return field.preview && (field.label == "Permissions" || field.label == "Bit depth" || field.label == "Color");
    }));
    gchar* jpeg = nullptr;
    gsize length = 0;
    const GioPtr<GdkPixbuf> rgb(gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 640, 360));
    gdk_pixbuf_fill(rgb.get(), 0x208040ffU);
    TEST_CHECK(gdk_pixbuf_save_to_buffer(rgb.get(), &jpeg, &length, "jpeg", nullptr, "quality", "100", nullptr));
    std::vector<std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(jpeg), reinterpret_cast<const std::uint8_t*>(jpeg) + length
    );
    g_free(jpeg);
    const std::array<std::uint8_t, 36> exif{0xff, 0xe1, 0, 34, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1,
                                            0,    0x12, 1, 3,  0,   1,   0,   0,   0, 6, 0,   0,   0,  0, 0, 0, 0};
    bytes.insert(bytes.begin() + 2, exif.begin(), exif.end());
    const auto rotated = root / "oriented.jpg";
    std::ofstream jpegFile(rotated, std::ios::binary);
    jpegFile.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    jpegFile.close();
    fields = inspect(rotated, "image/jpeg");
    TEST_CHECK(value(fields, "Pixel size") == "640 × 360 px" && value(fields, "Orientation") == "90° clockwise");
    const auto svg = root / "vector.svg";
    std::ofstream(
        svg
    ) << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 400 200'><rect width='400' height='200'/></svg>";
    fields = inspect(svg, "image/svg+xml");
    TEST_CHECK(value(fields, "Pixel size") == "400 × 200 px" && value(fields, "Format") == "SVG");
    const GioPtr<GZlibCompressor> compressor(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6));
    const GioPtr<GOutputStream> buffer(g_memory_output_stream_new_resizable());
    const GioPtr<GOutputStream> compressed(g_converter_output_stream_new(buffer.get(), G_CONVERTER(compressor.get())));
    const std::string vector = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 400 200'/>";
    TEST_CHECK(g_output_stream_write_all(compressed.get(), vector.data(), vector.size(), nullptr, nullptr, nullptr));
    TEST_CHECK(g_output_stream_close(compressed.get(), nullptr, nullptr));
    const auto svgz = root / "vector.svgz";
    std::ofstream zipFile(svgz, std::ios::binary);
    zipFile.write(
        static_cast<const char*>(g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(buffer.get()))),
        static_cast<std::streamsize>(g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(buffer.get())))
    );
    zipFile.close();
    fields = inspect(svgz, "image/svg+xml-compressed");
    TEST_CHECK(value(fields, "Pixel size") == "400 × 200 px" && value(fields, "Compression") == "Gzip");
    luciole::ThumbnailService thumbnails;
    luciole::PreviewService preview(thumbnails);
    thumbnails.setChangedCallback([&] { preview.thumbnailsChanged(); });
    const auto source = entry(path, "image/png");
    preview.show(source, 128);
    wait([&] { return !preview.busy(); });
    TEST_CHECK(value(preview.snapshot().metadata, "Pixel size") == "640 × 360 px");
    TEST_CHECK(preview.snapshot().image && preview.snapshot().image->pixels->width == 128);
    preview.show(source, 512);
    wait([&] { return !preview.busy(); });
    TEST_CHECK(value(preview.snapshot().metadata, "Pixel size") == "640 × 360 px");
    const auto document = root / "page.pdf";
    writePdf(document);
    fields = inspect(document, "application/pdf");
    TEST_CHECK(value(fields, "Pages") == "1" && value(fields, "PDF version") == "1.4");
    TEST_CHECK(value(fields, "Page size").contains("160 x 80"));
    const auto corrupt = root / "corrupt.png";
    std::ofstream(corrupt) << "not an image";
    TEST_CHECK(value(inspect(corrupt, "image/png"), "Pixel size").empty());
    auto stale = source;
    stale.size = 1;
    luciole::PreviewMetadataService metadata;
    metadata.show(stale, {});
    wait([&] { return !metadata.busy(); });
    TEST_CHECK(metadata.fields().empty());
    const auto notes = root / "notes.txt";
    std::ofstream(notes) << "UTF-8 café";
    TEST_CHECK(::chmod(notes.c_str(), 0600) == 0);
    preview.show(entry(notes, "text/plain"), 128);
    wait([&] { return !preview.busy(); });
    TEST_CHECK(value(preview.snapshot().metadata, "Permissions") == "0600");
    TEST_CHECK(std::ranges::none_of(preview.snapshot().metadata, [](const auto& field) {
      return field.preview
          && (field.label == "Owner"
              || field.label == "Group"
              || field.label == "Permissions"
              || field.label == "Created");
    }));
    TEST_CHECK(preview.snapshot().text == "UTF-8 café");
    stale = source;
    stale.setIdentity(luciole::FileIdentity{"old", "old"});
    metadata.show(stale, {});
    wait([&] { return !metadata.busy(); });
    TEST_CHECK(metadata.fields().empty());
  }
  void nativeFactsChecks(const fs::path& root) {
    const auto directory = root / "folder";
    fs::create_directory(directory);
    std::ofstream(directory / "untouched.txt") << "child";
    auto source = entry(directory, "inode/directory");
    source.type = luciole::FileType::Directory;
    luciole::PreviewMetadataService service;
    service.show(source, {});
    wait([&] { return !service.busy(); });
    TEST_CHECK(!value(service.fields(), "Permissions").empty());
    TEST_CHECK(value(service.fields(), "Pixel size").empty());
    const auto link = root / "broken-link";
    fs::create_symlink("missing.png", link);
    source = entry(link, "image/png");
    source.symlink = true;
    service.show(source, {});
    wait([&] { return !service.busy(); });
    TEST_CHECK(!value(service.fields(), "Permissions").empty());
    TEST_CHECK(value(service.fields(), "Pixel size").empty());
    TEST_CHECK(value(service.fields(), "Link Target") == "missing.png");
    TEST_CHECK(!value(service.fields(), "Target Status").empty());
    TEST_CHECK(value(service.fields(), "Resolved Target").empty());
    const auto fifo = root / "fifo.png";
    TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    source = entry(fifo, "image/png");
    source.type = luciole::FileType::Special;
    service.show(source, {});
    wait([&] { return !service.busy(); });
    TEST_CHECK(value(service.fields(), "Permissions") == "0600");
    TEST_CHECK(value(service.fields(), "Pixel size").empty());
  }
  void symbolicLinkChecks(const fs::path& root) {
    const auto folder = root / "link-facts";
    fs::create_directories(folder / "actual" / "nested");
    const auto target = folder / "actual" / "document <literal> café.txt";
    std::ofstream(target) << "target contents";
    fs::create_directory_symlink("actual/nested", folder / "alias");
    const auto link = folder / "relative-link";
    fs::create_symlink("alias/../document <literal> café.txt", link);
    fs::create_symlink(link, folder / "chain");
    fs::create_symlink("actual", folder / "folder-link");
    fs::create_symlink("loop-b", folder / "loop-a");
    fs::create_symlink("loop-a", folder / "loop-b");
    for (const auto& name : {"relative-link", "chain", "folder-link", "loop-a"}) {
      auto source = entry(folder / name, "inode/symlink");
      source.type = luciole::FileType::Symlink;
      source.symlink = true;
      luciole::PreviewMetadataService service;
      service.show(source, {});
      wait([&] { return !service.busy(); });
      const auto& fields = service.fields();
      TEST_CHECK(!value(fields, "Link Target").empty());
      TEST_CHECK(!value(fields, "Permissions").empty());
      TEST_CHECK(value(fields, "Pixel size").empty());
      if (std::string_view(name) == "loop-a") {
        TEST_CHECK(value(fields, "Resolved Target").empty());
        TEST_CHECK(!value(fields, "Target Status").empty());
      } else {
        const auto resolved = std::string_view(name) == "folder-link" ? folder / "actual" : target;
        TEST_CHECK(value(fields, "Resolved Target") == resolved.native());
        TEST_CHECK(value(fields, "Target Status").empty());
      }
      if (std::string_view(name) == "relative-link") {
        TEST_CHECK(value(fields, "Link Target") == "alias/../document <literal> café.txt");
        const luciole::GioPtr<GCancellable> cancel(g_cancellable_new());
        fs::remove(link);
        fs::create_symlink("missing", link);
        TEST_CHECK(luciole::probePreviewMetadata(source, {}, cancel.get()).empty());
        fs::remove(link);
        fs::create_symlink("alias/../document <literal> café.txt", link);
        g_cancellable_cancel(cancel.get());
        TEST_CHECK(luciole::probePreviewMetadata(source, {}, cancel.get()).empty());
      }
    }
  }
  void mediaChecks(const fs::path& root) {
    const auto clip = root / "clip café\n.mp4";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "color=c=red:s=160x90:r=25:d=0.4",
         "-f",
         "lavfi",
         "-i",
         "sine=duration=0.4:sample_rate=48000",
         "-shortest",
         "-c:v",
         "libx264",
         "-preset",
         "ultrafast",
         "-pix_fmt",
         "yuv420p",
         "-colorspace",
         "bt709",
         "-color_primaries",
         "bt709",
         "-color_trc",
         "bt709",
         "-threads",
         "1",
         "-c:a",
         "aac",
         "-metadata",
         "title=<literal> café",
         clip.native()}
    );
    auto fields = inspect(clip, "video/mp4");
    TEST_CHECK(value(fields, "Video codec") == "h264" && value(fields, "Audio codec") == "aac");
    TEST_CHECK(value(fields, "Frame size") == "160 × 90 px" && value(fields, "Frame rate") == "25.000 fps");
    TEST_CHECK(value(fields, "Title") == "<literal> café" && value(fields, "Sample rate") == "48000 Hz");
    TEST_CHECK(!value(fields, "Duration").empty());
    const auto wav = root / "plain.wav";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "sine=duration=0.2:sample_rate=44100", wav.native()}
    );
    luciole::ThumbnailService thumbnails;
    luciole::PreviewService preview(thumbnails);
    thumbnails.setChangedCallback([&] { preview.thumbnailsChanged(); });
    preview.show(entry(wav, "audio/x-wav"), 128);
    wait([&] { return !preview.busy(); });
    TEST_CHECK(value(preview.snapshot().metadata, "Audio codec") == "pcm_s16le");
    TEST_CHECK(
        value(preview.snapshot().metadata, "Sample rate") == "44100 Hz"
        && value(preview.snapshot().metadata, "Audio depth") == "16-bit"
    );
    TEST_CHECK(!preview.snapshot().image && preview.snapshot().status == luciole::PreviewStatus::Unavailable);
    const auto rotated = root / "rotated.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-display_rotation:v:0", "90", "-i", clip.native(), "-c", "copy",
         rotated.native()}
    );
    fields = inspect(rotated, "video/mp4");
    TEST_CHECK(value(fields, "Frame size") == "160 × 90 px" && !value(fields, "Rotation").empty());
  }

  void columnChecks(const fs::path& root) {
    const auto image = root / "image <literal> café\n.png";
    auto facts = luciole::probeEntryMediaFacts(image.native(), "image/png", nullptr);
    TEST_CHECK(facts.width == 640U && facts.height == 360U);
    TEST_CHECK(!facts.durationMicros && !facts.bitsPerSecond && !facts.audioCodec && !facts.videoCodec);
    TEST_CHECK(!facts.orientation && !facts.photoDateSeconds); // PNG carries no EXIF.
    const auto clip = root / "clip café\n.mp4";
    facts = luciole::probeEntryMediaFacts(clip.native(), "video/mp4", nullptr);
    TEST_CHECK(facts.width == 160U && facts.height == 90U);
    TEST_CHECK(facts.videoCodec == luciole::InternedText("h264") && facts.audioCodec == luciole::InternedText("aac"));
    TEST_CHECK(facts.frameRateMilli == 25'000U && facts.aspectRatioPerMille == 1'777U);
    TEST_CHECK(facts.pixelFormat == luciole::InternedText("yuv420p") && facts.colorSpace);
    TEST_CHECK(facts.durationMicros && facts.bitsPerSecond);
    // The parser accepts the fields ffprobe emits, including tags, and rejects anything else.
    const auto parsed = luciole::parseMediaFacts(
        R"({"format": {"duration": "5.5", "bit_rate": "128000",)"
        R"("tags": {"artist": "Ada", "album": "Notes", "genre": "Test", "track": "3/12", "date": "2021-03-04"}},)"
        R"("streams": [)"
        R"({"codec_type": "video", "codec_name": "h264", "width": "320", "height": "240",)"
        R"("avg_frame_rate": "30000/1001", "display_aspect_ratio": "4:3", "pix_fmt": "yuv420p", "color_space": "bt709"},)"
        R"({"codec_type": "audio", "codec_name": "aac"}]})"
    );
    TEST_CHECK(parsed.durationMicros == 5'500'000U && parsed.bitsPerSecond == 128'000U);
    TEST_CHECK(parsed.artist == luciole::InternedText("Ada") && parsed.album == luciole::InternedText("Notes"));
    TEST_CHECK(parsed.genre == luciole::InternedText("Test") && parsed.track == 3U && parsed.releaseYear == 2021U);
    TEST_CHECK(
        luciole::parseMediaFacts(
            R"({"format": {"bit_rate": "5000000"}, "streams": [{"codec_type": "audio", "codec_name": "aac"}]})"
        )
            .bitsPerSecond
        == 5'000'000U
    );
    TEST_CHECK(luciole::parseMediaFacts("not json") == luciole::EntryMedia{});
    // Hand-built JPEGs whose EXIF carries orientation 6 and a DateTimeOriginal. EXIF carries no zone, so the stored
    // wall time must come back exactly; pinning Paris makes July observe DST and March standard time.
    const auto exifJpeg = [](const std::string& datetime) {
      std::vector<std::uint8_t> tiff;
      const auto push16 = [&tiff](std::uint16_t value) {
        tiff.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        tiff.push_back(static_cast<std::uint8_t>(value >> 8));
      };
      const auto push32 = [&tiff](std::uint32_t value) {
        for (auto shift = 0U; shift < 32U; shift += 8U) {
          tiff.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
      };
      const auto exifIfd = 8 + 2 + 2 * 12 + 4; // IFD0 with two entries and its next-IFD word.
      tiff.insert(tiff.end(), {'I', 'I'});
      push16(42);
      push32(8);
      push16(2); // Two IFD0 entries.
      push16(0x0112);
      push16(3);
      push32(1);
      push16(6);
      push16(0); // Orientation 6, inline SHORT.
      push16(0x8769);
      push16(4);
      push32(1);
      push32(exifIfd);
      push32(0); // No further IFD.
      push16(1); // One EXIF IFD entry.
      push16(0x9003);
      push16(2);
      push32(datetime.size() + 1);
      push32(exifIfd + 2 + 12);
      for (const auto character : datetime) {
        tiff.push_back(static_cast<std::uint8_t>(character));
      }
      tiff.push_back(0);
      std::vector<std::uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xE1};
      const std::size_t segment = 2 + 6 + tiff.size();
      jpeg.push_back(static_cast<std::uint8_t>(segment >> 8));
      jpeg.push_back(static_cast<std::uint8_t>(segment & 0xFFU));
      jpeg.insert(jpeg.end(), {'E', 'x', 'i', 'f', 0, 0});
      jpeg.insert(jpeg.end(), tiff.begin(), tiff.end());
      return jpeg;
    };
    const auto writeExif = [&root](const std::string& name, const std::vector<std::uint8_t>& jpeg) {
      const auto encoded = root / name;
      std::ofstream(encoded, std::ios::binary)
          .write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
      TEST_CHECK(
          static_cast<std::size_t>(std::ifstream(encoded, std::ios::binary | std::ios::ate).tellg()) == jpeg.size()
      );
      Fd opened(::open(encoded.c_str(), O_RDONLY));
      TEST_CHECK(opened.valid());
      return opened;
    };
    // The column renders through g_date_time_new_from_unix_local, so the reader must agree with GLib's local view.
    const auto localEpoch = [](int year, int month, int day, int hour, int minute, int second) {
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
          g_date_time_new_local(year, month, day, hour, minute, second), &g_date_time_unref
      );
      return date ? static_cast<std::uint64_t>(g_date_time_to_unix(date.get())) : 0U;
    };
    g_setenv("TZ", "Europe/Paris", 1);
    ::tzset();
    const auto summer = writeExif("exif-summer.jpg", exifJpeg("2021:07:01 14:30:07"));
    const auto summerFacts = luciole::readExifFacts(summer.get());
    TEST_CHECK(summerFacts.orientation == 6U);
    TEST_CHECK(summerFacts.dateSeconds == localEpoch(2021, 7, 1, 14, 30, 7)); // Fails while standard time is forced.
    const auto winter = writeExif("exif.jpg", exifJpeg("2021:03:04 05:06:07"));
    const auto exif = luciole::readExifFacts(winter.get());
    TEST_CHECK(exif.orientation == 6U);
    TEST_CHECK(exif.dateSeconds == localEpoch(2021, 3, 4, 5, 6, 7));
    const Fd plain(::open((root / "plain.jpg").c_str(), O_RDONLY | O_CREAT | O_WRONLY | O_TRUNC, 0600));
    TEST_CHECK(::write(plain.get(), "FFD8 not enough", 15) == 15);
    TEST_CHECK(luciole::readExifFacts(plain.get()).orientation == std::nullopt);
  }

  struct PathOverride {
    std::string original = std::getenv("PATH");
    ~PathOverride() { ::setenv("PATH", original.c_str(), 1); }
  };
  void lifecycleChecks(const fs::path& root) {
    const auto path = root / "clip café\n.mp4";
    const auto video = entry(path, "video/mp4");
    const auto svg = entry(root / "vector.svg", "image/svg+xml");
    const auto bin = root / "bin";
    fs::create_directory(bin);
    const luciole::GString shell(g_find_program_in_path("sh")), sleep(g_find_program_in_path("sleep"));
    TEST_CHECK(shell && sleep);
    PathOverride restore;
    ::setenv("PATH", "/luciole-no-provider", 1);
    TEST_CHECK(value(inspect(path, "video/mp4"), "Video codec").empty());
    ::setenv("PATH", bin.c_str(), 1);
    const auto started = root / "started";
    std::ofstream(bin / "ffprobe")
        << "#!"
        << shell.get()
        << "\n: > "
        << started.native()
        << "\nexec "
        << sleep.get()
        << " 30\n";
    TEST_CHECK(::chmod((bin / "ffprobe").c_str(), 0700) == 0);
    int replies = 0;
    luciole::PreviewMetadataService service;
    service.setChangedCallback([&] { ++replies; });
    service.show(video, {});
    wait([&] { return fs::exists(started); });
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
      service.show(i % 2 ? video : svg, {});
    }
    service.show(svg, {});
    wait([&] { return !service.busy(); });
    TEST_CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(3));
    TEST_CHECK(replies == 1 && value(service.fields(), "Format") == "SVG");
    fs::remove(started);
    service.show(video, {});
    wait([&] { return fs::exists(started); });
    service.clear();
    wait([&] { return !service.busy(); });
    TEST_CHECK(service.fields().empty() && replies == 1);
    fs::remove(started);
    {
      luciole::PreviewMetadataService retired;
      retired.setChangedCallback([] { TEST_CHECK(false); });
      retired.show(video, {});
      wait([&] { return fs::exists(started); });
    }
    const auto drain = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    wait([&] { return std::chrono::steady_clock::now() >= drain; });
    const auto gate = root / "gate";
    fs::remove(started);
    std::ofstream(bin / "ffprobe")
        << "#!"
        << shell.get()
        << "\n: > "
        << started.native()
        << "\nwhile [ ! -f "
        << gate.native()
        << " ]; do "
        << sleep.get()
        << " 0.01; done\nprintf '%s' '{\"format\":{\"duration\":\"42\"}}'\n";
    service.show(video, {});
    wait([&] { return fs::exists(started); });
    fs::rename(path, root / "original.mp4");
    fs::copy_file(root / "original.mp4", path);
    std::ofstream(gate) << '1';
    wait([&] { return !service.busy(); });
    TEST_CHECK(service.fields().empty());
  }
} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  if (!args.empty() && args.front() == "--preview-metadata-worker") {
    return luciole::runPreviewMetadataWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(args).subspan(1));
  }
  const luciole::GString temporary(g_dir_make_tmp("luciole-metadata-test-XXXXXX", nullptr));
  TEST_CHECK(temporary != nullptr);
  const fs::path root(temporary.get());
  ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
  parserChecks();
  imageChecks(root);
  launcherChecks(root);
  nativeFactsChecks(root);
  symbolicLinkChecks(root);
  mediaChecks(root);
  columnChecks(root);
  lifecycleChecks(root);
  fs::remove_all(root);
  std::println("preview metadata checks passed");
}
