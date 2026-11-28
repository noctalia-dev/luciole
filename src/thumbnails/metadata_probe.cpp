#include "thumbnails/metadata_probe.h"

#include "fs/entry_access.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "imaging/image_limits.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_store.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::string stamp(const struct stat& info) {
    return std::format(
        "{}:{}:{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_size, info.st_mtim.tv_sec, info.st_mtim.tv_nsec,
        info.st_ctim.tv_sec, info.st_ctim.tv_nsec
    );
  }
  bool matches(const luciole::DirectoryEntry& source, GFileInfo* info) {
    if (source.size
        && (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) != G_FILE_ATTRIBUTE_TYPE_UINT64
            || g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) != *source.size)) {
      return false;
    }
    if (source.modifiedSeconds
        && (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_TIME_MODIFIED) != G_FILE_ATTRIBUTE_TYPE_UINT64
            || g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED) != *source.modifiedSeconds)) {
      return false;
    }
    if (const auto identity = source.identity(); identity && luciole::readFileIdentity(info) != identity) {
      return false;
    }
    if (const auto etag = source.etag(); etag && (!g_file_info_get_etag(info) || *etag != g_file_info_get_etag(info))) {
      return false;
    }
    if (source.contentRevision && luciole::readChangeRevision(info) != source.contentRevision) {
      return false;
    }
    return true;
  }
  bool sameDescriptor(const struct stat& descriptor, GFileInfo* info) {
    if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) != G_FILE_ATTRIBUTE_TYPE_UINT32
        || g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_UNIX_INODE) != G_FILE_ATTRIBUTE_TYPE_UINT64) {
      return false;
    }
    const auto nanos = g_file_info_has_attribute(info, "time::changed-nsec")
        ? g_file_info_get_attribute_uint32(info, "time::changed-nsec")
        : g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC) * 1000U;
    return descriptor.st_dev == g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE)
        && descriptor.st_ino == g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE)
        && descriptor.st_ctim.tv_sec >= 0
        && static_cast<std::uint64_t>(descriptor.st_ctim.tv_sec)
        == g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CHANGED)
        && descriptor.st_ctim.tv_nsec >= 0
        && static_cast<std::uint64_t>(descriptor.st_ctim.tv_nsec) == nanos;
  }
  luciole::MetadataFields symbolicLinkFields(const std::string& path, GFileInfo* info, GCancellable* cancel) {
    luciole::MetadataFields fields;
    if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)
        == G_FILE_ATTRIBUTE_TYPE_BYTE_STRING) {
      const luciole::GString target(g_filename_display_name(g_file_info_get_symlink_target(info)));
      fields.push_back({"Link Target", target.get()});
    }
    std::error_code error;
    // Resolve the actual pathname before presenting it, preserving symlink/.. semantics.
    const auto resolved = std::filesystem::canonical(path, error);
    if (error) {
      fields.push_back({"Target Status", luciole::errnoError(error.value()).message});
    } else {
      const luciole::GString target(g_filename_display_name(resolved.c_str()));
      fields.push_back({"Resolved Target", target.get()});
    }
    struct stat named{};
    if (g_cancellable_is_cancelled(cancel) || ::lstat(path.c_str(), &named) < 0 || !sameDescriptor(named, info)) {
      return {};
    }
    return fields;
  }
  luciole::MetadataFields commonFields(GFileInfo* info) {
    luciole::MetadataFields fields;
    for (const auto& [attribute, label] :
         {std::pair{G_FILE_ATTRIBUTE_OWNER_USER, "Owner"}, std::pair{G_FILE_ATTRIBUTE_OWNER_GROUP, "Group"}}) {
      if (g_file_info_get_attribute_type(info, attribute) == G_FILE_ATTRIBUTE_TYPE_STRING) {
        const char* text = g_file_info_get_attribute_string(info, attribute);
        if (text && *text) {
          const luciole::GString valid(g_utf8_make_valid(text, -1));
          fields.push_back({label, valid.get(), false});
        }
      }
    }
    if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_UNIX_MODE) == G_FILE_ATTRIBUTE_TYPE_UINT32) {
      fields.push_back(
          {"Permissions",
           std::format("{:04o}", g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 07777U), false}
      );
    }
    for (const auto& [attribute, label] :
         {std::pair{G_FILE_ATTRIBUTE_TIME_CREATED, "Created"}, std::pair{G_FILE_ATTRIBUTE_TIME_ACCESS, "Accessed"}}) {
      if (g_file_info_get_attribute_type(info, attribute) != G_FILE_ATTRIBUTE_TYPE_UINT64) {
        continue;
      }
      const auto seconds = g_file_info_get_attribute_uint64(info, attribute);
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
          seconds <= G_MAXINT64 ? g_date_time_new_from_unix_local(static_cast<gint64>(seconds)) : nullptr,
          &g_date_time_unref
      );
      if (date) {
        const luciole::GString value(g_date_time_format(date.get(), "%Y-%m-%d %H:%M"));
        fields.push_back({label, value.get(), false});
      }
    }
    return fields;
  }
  luciole::MetadataFields launcherFields(int fd, GCancellable* cancel) {
    constexpr std::size_t limit = 256U * 1024U;
    std::array<char, 4096> buffer{};
    std::string bytes;
    while (bytes.size() <= limit && !g_cancellable_is_cancelled(cancel)) {
      const auto count = ::read(fd, buffer.data(), std::min(buffer.size(), limit + 1 - bytes.size()));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        return {};
      }
      if (count == 0) {
        break;
      }
      bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (bytes.size() > limit || bytes.contains('\0') || g_cancellable_is_cancelled(cancel)) {
      return {};
    }
    const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> file(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_data(file.get(), bytes.data(), bytes.size(), G_KEY_FILE_NONE, nullptr)) {
      return {};
    }
    const luciole::GString type(g_key_file_get_string(file.get(), "Desktop Entry", "Type", nullptr));
    if (!type || (std::string_view(type.get()) != "Application" && std::string_view(type.get()) != "Link")) {
      return {};
    }
    luciole::MetadataFields fields;
    for (const auto& [key, label] :
         {std::pair{"Name", std::string_view(type.get()) == "Application" ? "Application" : "Link name"},
          std::pair{"Comment", "Description"}}) {
      const luciole::GString value(g_key_file_get_locale_string(file.get(), "Desktop Entry", key, nullptr, nullptr));
      if (value
          && *value
          && std::string_view(value.get()).size() <= 2048
          && g_utf8_validate(value.get(), -1, nullptr)) {
        fields.push_back({label, value.get()});
      }
    }
    return fields;
  }
  luciole::MetadataFields imageFields(std::string_view data) {
    const auto root = nlohmann::json::parse(data, nullptr, false);
    if (!root.is_array() || root.size() > 24) {
      return {};
    }
    luciole::MetadataFields fields;
    for (const auto& row : root) {
      if (!row.is_object()
          || !row.contains("label")
          || !row.contains("value")
          || !row["label"].is_string()
          || !row["value"].is_string()
          || (row.contains("preview") && !row["preview"].is_boolean())) {
        return {};
      }
      const auto label = row["label"].get<std::string>(), value = row["value"].get<std::string>();
      if (label.size() > 64
          || value.size() > 2048
          || !g_utf8_validate(label.c_str(), -1, nullptr)
          || !g_utf8_validate(value.c_str(), -1, nullptr)) {
        return {};
      }
      fields.push_back({label, value, row.value("preview", true)});
    }
    return fields;
  }

  // Shared with probeEntryMediaFacts below.
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  };

  // The bounded ffprobe invocation; preview metadata and list view's media columns ask for the same fields.
  constexpr std::array<std::string_view, 20> kMediaProbeArguments{
      "ffprobe",
      "-v",
      "error",
      "-max_alloc",
      "67108864",
      "-probesize",
      "8388608",
      "-analyzeduration",
      "2000000",
      "-max_streams",
      "32",
      "-threads",
      "1",
      "-protocol_whitelist",
      "file,pipe",
      "-of",
      "json",
      "-show_entries",
      "format=format_long_name,duration,bit_rate:format_tags=title,artist,album,track,date,genre:stream=codec_type,"
      "codec_name,width,height,avg_frame_rate,pix_fmt,color_space,nb_frames,duration,sample_rate,channels,channel_"
      "layout,bits_per_sample,bit_rate,display_aspect_ratio:stream_disposition=attached_pic:stream_side_data=rotation",
      "/proc/self/fd/3"
  };

  luciole::InternedText boundedText(const std::string& text) {
    if (text.empty() || text.size() > 256 || !g_utf8_validate(text.c_str(), -1, nullptr)) {
      return {};
    }
    return luciole::InternedText(text);
  }

  std::optional<std::uint64_t> probeNumber(const nlohmann::json& value, std::uint64_t maximum) {
    if (value.is_number_unsigned()) {
      const auto parsed = value.get<std::uint64_t>();
      return parsed <= maximum ? std::optional(parsed) : std::nullopt;
    }
    if (value.is_number_integer() && value.get<int>() > 0) {
      const auto parsed = static_cast<std::uint64_t>(value.get<int>());
      return parsed <= maximum ? std::optional(parsed) : std::nullopt;
    }
    if (!value.is_string()) {
      return {};
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > 20) {
      return {};
    }
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || parsed > maximum) {
      return {};
    }
    return parsed;
  }

  std::optional<double> probeSeconds(const nlohmann::json& value) {
    if (value.is_number()) {
      const double seconds = value.get<double>();
      return seconds > 0 && seconds < 10'000'000.0 ? std::optional(seconds) : std::nullopt;
    }
    if (!value.is_string()) {
      return {};
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 24) {
      return {};
    }
    double seconds = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || seconds <= 0 || seconds >= 10'000'000.0) {
      return {};
    }
    return seconds;
  }

  // "25/1" for rates and "16:9" for ratios; scaled by one thousand, rounded, capped at one million per second.
  std::optional<std::uint64_t> probeRatioMilli(const nlohmann::json& value) {
    if (!value.is_string()) {
      return {};
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 24) {
      return {};
    }
    const auto separator = text.find_first_of("/:");
    std::uint64_t numerator = 0, denominator = 1;
    const auto [numberEnd, numberError] = std::from_chars(
        text.data(), text.data() + (separator == std::string_view::npos ? text.size() : separator), numerator
    );
    if (numberError != std::errc{}) {
      return {};
    }
    if (separator != std::string_view::npos) {
      const auto [denominatorEnd, denominatorError] =
          std::from_chars(text.data() + separator + 1, text.data() + text.size(), denominator);
      if (denominatorError != std::errc{} || denominator == 0) {
        return {};
      }
    }
    if (numerator > (std::numeric_limits<std::uint64_t>::max() - denominator / 2U) / 1000U) {
      return {};
    }
    const auto milli = (numerator * 1000U + denominator / 2U) / denominator;
    if (milli > 1'000'000'000U) {
      return {};
    }
    return milli;
  }
} // namespace

// Structured facts for list view's columns; parallel to parseMediaMetadata's presentation parser. Both walk the same
// bounded ffprobe JSON, so their field selection and size bounds must change together.
luciole::EntryMedia luciole::parseMediaFacts(std::string_view data) {
  const auto root = nlohmann::json::parse(data, nullptr, false);
  if (!root.is_object() || root.size() > 8) {
    return {};
  }

  EntryMedia facts;
  const auto format = root.find("format");
  if (format != root.end() && format->is_object() && format->size() <= 16) {
    if (const auto duration = format->find("duration"); duration != format->end()) {
      if (const auto seconds = probeSeconds(*duration)) {
        facts.durationMicros = static_cast<std::uint64_t>(*seconds * 1'000'000.0);
      }
    }
    if (const auto rate = format->find("bit_rate"); rate != format->end()) {
      facts.bitsPerSecond = probeNumber(*rate, 10'000'000'000U);
    }
    if (const auto tags = format->find("tags"); tags != format->end() && tags->is_object() && tags->size() <= 16) {
      const auto text = [&tags](const std::string& key) {
        const auto found = tags->find(key);
        return found != tags->end() && found->is_string() ? boundedText(found->get_ref<const std::string&>())
                                                          : InternedText();
      };
      facts.artist = text("artist");
      facts.album = text("album");
      facts.genre = text("genre");
      if (const auto track = tags->find("track"); track != tags->end()) {
        facts.track = probeNumber(*track, 100'000U);
      }
      if (const auto date = tags->find("date"); date != tags->end() && date->is_string()) {
        const auto& value = date->get_ref<const std::string&>();
        if (value.size() >= 4) {
          std::uint64_t year = 0;
          const auto [end, error] = std::from_chars(value.data(), value.data() + 4, year);
          if (error == std::errc{} && year >= 1000 && year <= 3000) {
            facts.releaseYear = year;
          }
        }
      }
    }
  }
  const auto streams = root.find("streams");
  if (streams == root.end() || !streams->is_array() || streams->size() > 32) {
    return facts;
  }
  for (const auto& stream : *streams) {
    if (!stream.is_object() || stream.size() > 24) {
      continue;
    }
    const auto type = stream.find("codec_type");
    if (type == stream.end() || !type->is_string()) {
      continue;
    }
    const auto codec = stream.find("codec_name");
    const std::string name = codec != stream.end() && codec->is_string() ? codec->get<std::string>() : std::string();
    if (name.size() > 64 || (!name.empty() && !g_utf8_validate(name.c_str(), -1, nullptr))) {
      continue;
    }
    if (type->get_ref<const std::string&>() == "video") {
      const auto disposition = stream.find("disposition");
      const bool cover = disposition != stream.end()
          && disposition->is_object()
          && disposition->contains("attached_pic")
          && disposition->at("attached_pic").is_number_integer()
          && disposition->at("attached_pic").get<int>() != 0;
      if (cover) {
        continue;
      }
      if (!name.empty() && !facts.videoCodec) {
        facts.videoCodec = InternedText(name);
      }
      const auto width = stream.find("width"), height = stream.find("height");
      const auto parseWidth = width != stream.end() ? probeNumber(*width, 1'000'000U) : std::nullopt;
      const auto parseHeight = height != stream.end() ? probeNumber(*height, 1'000'000U) : std::nullopt;
      if (parseWidth && *parseWidth > 0 && parseHeight && *parseHeight > 0) {
        facts.width = static_cast<std::uint32_t>(*parseWidth);
        facts.height = static_cast<std::uint32_t>(*parseHeight);
      }
      if (!facts.frameRateMilli) {
        if (const auto rate = stream.find("avg_frame_rate"); rate != stream.end()) {
          facts.frameRateMilli = probeRatioMilli(*rate);
        }
      }
      if (const auto ratio = stream.find("display_aspect_ratio"); ratio != stream.end()) {
        facts.aspectRatioPerMille = probeRatioMilli(*ratio);
        if (ratio->is_string()) {
          facts.aspectRatio = boundedText(ratio->get_ref<const std::string&>());
        }
      }
      const auto pixel = stream.find("pix_fmt");
      if (pixel != stream.end() && pixel->is_string()) {
        facts.pixelFormat = boundedText(pixel->get_ref<const std::string&>());
      }
      const auto space = stream.find("color_space");
      if (space != stream.end() && space->is_string()) {
        facts.colorSpace = boundedText(space->get_ref<const std::string&>());
      }
    } else if (type->get_ref<const std::string&>() == "audio" && !name.empty() && !facts.audioCodec) {
      facts.audioCodec = InternedText(name);
    }
  }
  return facts;
}

luciole::MetadataFields
luciole::probePreviewMetadata(const DirectoryEntry& entry, ThumbnailOptions options, GCancellable* cancel) {
  const auto location = entry.location();
  const auto path = location.nativePath();
  const std::string mime(entry.contentType.value_or(""));
  const bool image = mime.starts_with("image/"), media = mime.starts_with("video/") || mime.starts_with("audio/");
  const bool launcher = g_content_type_is_a(mime.c_str(), "application/x-desktop");
  if (!path || g_cancellable_is_cancelled(cancel)) {
    return {};
  }
  const GioPtr<GFileInfo> current(g_file_query_info(
      gioFile(location).get(),
      G_FILE_ATTRIBUTE_ETAG_VALUE
      "," G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_TIME_CHANGED
      ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "," G_FILE_ATTRIBUTE_STANDARD_SIZE
      "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_OWNER_GROUP
      "," G_FILE_ATTRIBUTE_UNIX_MODE "," G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_TIME_ACCESS
      "," G_FILE_ATTRIBUTE_UNIX_DEVICE "," G_FILE_ATTRIBUTE_UNIX_INODE "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK
      "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr
  ));
  if (!current || !matches(entry, current.get())) {
    return {};
  }
  auto fields = commonFields(current.get());
  if (readIsSymlink(current.get())) {
    auto links = symbolicLinkFields(*path, current.get(), cancel);
    if (links.empty()) {
      return {};
    }
    links.insert(links.end(), std::make_move_iterator(fields.begin()), std::make_move_iterator(fields.end()));
    return links;
  }
  if (entry.type != FileType::Regular
      || entry.symlink
      || entryAccess(entry).read == false
      || (!image && !media && !launcher && mime != "application/pdf")) {
    return fields;
  }
  const GioPtr<GFileInfo> filesystem(
      g_file_query_filesystem_info(gioFile(location).get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, cancel, nullptr)
  );
  if (!filesystem
      || !g_file_info_has_attribute(filesystem.get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE)
      || g_file_info_get_attribute_boolean(filesystem.get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE)) {
    return fields;
  }
  const Fd input(::open(path->c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  struct stat before{};
  if (!input.valid()
      || ::fstat(input.get(), &before) < 0
      || !S_ISREG(before.st_mode)
      || before.st_size <= 0
      || (entry.size && *entry.size != static_cast<std::uint64_t>(before.st_size))
      || (entry.modifiedSeconds && *entry.modifiedSeconds != static_cast<std::uint64_t>(before.st_mtim.tv_sec))) {
    return fields;
  }
  if (!sameDescriptor(before, current.get())) {
    return {};
  }
  if (launcher) {
    auto extra = launcherFields(input.get(), cancel);
    struct stat after{}, named{};
    if (g_cancellable_is_cancelled(cancel)
        || ::fstat(input.get(), &after) < 0
        || ::lstat(path->c_str(), &named) < 0
        || stamp(before) != stamp(after)
        || stamp(before) != stamp(named)) {
      return {};
    }
    extra.insert(extra.end(), std::make_move_iterator(fields.begin()), std::make_move_iterator(fields.end()));
    return extra;
  }
  const GString temporary(g_dir_make_tmp("luciole-metadata-XXXXXX", nullptr));
  if (!temporary) {
    return fields;
  }
  const Cleanup cleanup{temporary.get()};
  const auto output = cleanup.path / "result";
  const Fd capture(::open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (!capture.valid()) {
    return fields;
  }
  std::vector<std::string> args;
  std::uint64_t fileLimit = 65536;
  if (image) {
    const auto maximum =
        std::min(kMaxEncodedImageBytes, static_cast<std::size_t>(std::max(1, options.maxFileMiB)) * 1024U * 1024U);
    if (static_cast<std::uint64_t>(before.st_size) > maximum) {
      return fields;
    }
    args = {"/proc/self/exe", "--preview-metadata-worker", mime, std::to_string(maximum)};
    fileLimit = std::max<std::uint64_t>(maximum, kProviderMemoryBytes);
  } else if (media) {
    args.assign(kMediaProbeArguments.begin(), kMediaProbeArguments.end());
  } else {
    args = {"pdfinfo", "-enc", "UTF-8", "/proc/self/fd/3"};
  }
  const auto success = runProviderProcess(std::move(args), input.get(), cancel, fileLimit, 2, capture.get());
  struct stat after{}, named{};
  if (g_cancellable_is_cancelled(cancel)
      || ::fstat(input.get(), &after) < 0
      || ::lstat(path->c_str(), &named) < 0
      || stamp(before) != stamp(after)
      || stamp(before) != stamp(named)) {
    return {};
  }
  if (!success) {
    return fields;
  }
  const auto bytes = readThumbnailFile(output, 65536);
  if (!bytes) {
    return fields;
  }
  const std::string_view data(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  auto extra = image ? imageFields(data) : media ? parseMediaMetadata(data) : parsePdfMetadata(data);
  extra.insert(extra.end(), std::make_move_iterator(fields.begin()), std::make_move_iterator(fields.end()));
  return extra;
}

luciole::EntryMedia
luciole::probeEntryMediaFacts(const std::string& path, const std::string& contentType, GCancellable* cancel) {
  const bool image = contentType.starts_with("image/"),
             media = contentType.starts_with("video/") || contentType.starts_with("audio/");
  if ((!image && !media) || g_cancellable_is_cancelled(cancel)) {
    return {};
  }
  const Fd input(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  struct stat before;
  if (!input.valid() || ::fstat(input.get(), &before) < 0 || !S_ISREG(before.st_mode) || before.st_size <= 0) {
    return {};
  }
  // The facts only describe the file that was read; a changed file answers nothing.
  const auto unchanged = [&] {
    struct stat after{}, named{};
    return !g_cancellable_is_cancelled(cancel)
        && ::fstat(input.get(), &after) == 0
        && ::lstat(path.c_str(), &named) == 0
        && stamp(before) == stamp(after)
        && stamp(before) == stamp(named);
  };
  if (image) {
    int width = 0, height = 0;
    const std::string descriptor = std::format("/proc/self/fd/{}", input.get());
    // GdkPixbuf owns the returned format (transfer-none); freeing it corrupts the loader registry.
    const auto* info = gdk_pixbuf_get_file_info(descriptor.c_str(), &width, &height);
    EntryMedia facts;
    if (info && width > 0 && height > 0) {
      facts.width = static_cast<std::uint32_t>(width);
      facts.height = static_cast<std::uint32_t>(height);
    }
    const auto exif = readExifFacts(input.get());
    facts.orientation = exif.orientation;
    facts.photoDateSeconds = exif.dateSeconds;
    if (!unchanged() || facts == EntryMedia{}) {
      return {};
    }
    return facts;
  }
  const GString temporary(g_dir_make_tmp("luciole-media-facts-XXXXXXX", nullptr));
  if (!temporary) {
    return {};
  }
  const Cleanup cleanup{temporary.get()};
  const auto output = cleanup.path / "result";
  const Fd capture(::open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (!capture.valid()) {
    return {};
  }
  std::vector<std::string> args(kMediaProbeArguments.begin(), kMediaProbeArguments.end());
  const auto success = runProviderProcess(std::move(args), input.get(), cancel, 65536, 2, capture.get());
  if (!unchanged() || !success) {
    return {};
  }
  const auto bytes = readThumbnailFile(output, 65536);
  if (!bytes) {
    return {};
  }
  return parseMediaFacts(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
}
