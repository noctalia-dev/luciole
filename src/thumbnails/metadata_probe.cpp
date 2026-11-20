#include "thumbnails/metadata_probe.h"

#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "imaging/image_limits.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_store.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <format>
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
} // namespace

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
      "," G_FILE_ATTRIBUTE_UNIX_DEVICE "," G_FILE_ATTRIBUTE_UNIX_INODE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr
  ));
  if (!current || !matches(entry, current.get())) {
    return {};
  }
  auto fields = commonFields(current.get());
  if (entry.type != FileType::Regular
      || entry.symlink
      || entry.access.read == false
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
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{temporary.get()};
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
    args = {
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
        "format=format_long_name,duration,bit_rate:format_tags=title,artist,album,track,date:stream=codec_type,codec_"
        "name,width,height,avg_frame_rate,pix_fmt,color_space,nb_frames,duration,sample_rate,channels,channel_layout,"
        "bits_per_sample,bit_rate:stream_disposition=attached_pic:stream_side_data=rotation",
        "/proc/self/fd/3"
    };
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
