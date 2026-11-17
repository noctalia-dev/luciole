#include "fs/ws_discovery_metadata.h"

#include "fs/gio_interop.h"

#include <chrono>
#include <curl/curl.h>
#include <memory>

namespace {
  constexpr std::size_t kHeaderLimit = 65536;
  struct Response {
    const luciole::CancelToken& token;
    std::string body;
    std::size_t headers = 0;
  };
  std::size_t body(char* bytes, std::size_t size, std::size_t count, void* data) noexcept {
    auto& response = *static_cast<Response*>(data);
    if (response.token.cancelled() || (size && count > luciole::wsd::kMetadataLimit / size)) {
      return 0;
    }
    const auto length = size * count;
    if (length > luciole::wsd::kMetadataLimit - response.body.size()) {
      return 0;
    }
    try {
      response.body.append(bytes, length);
    } catch (...) {
      // Exceptions must not cross libcurl's C callback boundary.
      return 0;
    }
    return length;
  }
  std::size_t headers(char*, std::size_t size, std::size_t count, void* data) noexcept {
    auto& response = *static_cast<Response*>(data);
    if (response.token.cancelled() || (size && count > kHeaderLimit / size)) {
      return 0;
    }
    const auto length = size * count;
    if (length > kHeaderLimit - response.headers) {
      return 0;
    }
    response.headers += length;
    return length;
  }
  struct Attached {
    CURLM* multi;
    CURL* easy;
    ~Attached() { (void)curl_multi_remove_handle(multi, easy); }
  };
} // namespace

std::string luciole::wsd::metadata(
    const MetadataTarget& target, const std::string& endpoint, bool modern, const CancelToken& token
) {
  if (token.cancelled()) {
    return {};
  }
  // Process-wide state outlives detached discovery workers; never tear it down from a view.
  static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (initialized != CURLE_OK) {
    return {};
  }
  const GString id(g_uuid_string_random());
  const auto request = metadataRequest("urn:uuid:" + std::string(id.get()), endpoint, modern);
  const auto host = "Host: " + target.host;
  curl_slist* rawHeaders = curl_slist_append(nullptr, host.c_str());
  if (!rawHeaders) {
    return {};
  }
  const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> ownedHeaders(rawHeaders, curl_slist_free_all);
  if (!curl_slist_append(rawHeaders, "Content-Type: application/soap+xml; charset=utf-8")
      || !curl_slist_append(rawHeaders, "Expect:")) {
    return {};
  }
  Response response{.token = token, .body = {}};
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
  const std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
  if (!easy || !multi) {
    return {};
  }
  const auto set = [&]<typename T>(CURLoption option, T value) {
    return curl_easy_setopt(easy.get(), option, value) == CURLE_OK;
  };
  const long httpVersion = CURL_HTTP_VERSION_1_1;
  if (!set(CURLOPT_URL, target.uri.c_str())
      || !set(CURLOPT_PROTOCOLS_STR, "http,https")
      || !set(CURLOPT_PROXY, "")
      || !set(CURLOPT_NOPROXY, "*")
      || !set(CURLOPT_FOLLOWLOCATION, 0L)
      || !set(CURLOPT_FAILONERROR, 1L)
      || !set(CURLOPT_NOSIGNAL, 1L)
      || !set(CURLOPT_ACCEPT_ENCODING, "")
      || !set(CURLOPT_TIMEOUT_MS, 8000L)
      || !set(CURLOPT_HTTP_VERSION, httpVersion)
      || !set(CURLOPT_HTTPHEADER, rawHeaders)
      || !set(CURLOPT_POSTFIELDS, request.data())
      || !set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.size()))
      || !set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMetadataLimit))
      || !set(CURLOPT_WRITEFUNCTION, &body)
      || !set(CURLOPT_WRITEDATA, &response)
      || !set(CURLOPT_HEADERFUNCTION, &headers)
      || !set(CURLOPT_HEADERDATA, &response)
      || curl_multi_add_handle(multi.get(), easy.get()) != CURLM_OK) {
    return {};
  }
  const Attached attached{multi.get(), easy.get()};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  int running = 0;
  do {
    if (token.cancelled()
        || std::chrono::steady_clock::now() >= deadline
        || curl_multi_perform(multi.get(), &running) != CURLM_OK) {
      return {};
    }
    if (running && curl_multi_poll(multi.get(), nullptr, 0, 50, nullptr) != CURLM_OK) {
      return {};
    }
  } while (running);
  int remaining = 0;
  const auto* completion = curl_multi_info_read(multi.get(), &remaining);
  long status = 0;
  if (token.cancelled()
      || !completion
      || completion->msg != CURLMSG_DONE
      || completion->data.result != CURLE_OK
      || curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK
      || status != 200) {
    return {};
  }
  return metadataName(response.body);
}
