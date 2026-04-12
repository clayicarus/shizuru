#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "async_logger.h"

namespace shizuru::services {

// ---------------------------------------------------------------------------
// Global curl initialization
// ---------------------------------------------------------------------------

// One-time global curl init (thread-safe via static init).
inline void EnsureCurlInit() {
  static bool initialized = [] {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
  }();
  (void)initialized;
}

// ---------------------------------------------------------------------------
// RAII wrappers
// ---------------------------------------------------------------------------

// RAII wrapper for a CURL easy handle.
class CurlHandle {
 public:
  CurlHandle() : handle_(curl_easy_init()) {
    if (!handle_) {
      throw std::runtime_error("curl_easy_init() failed");
    }
  }

  ~CurlHandle() {
    if (handle_) { curl_easy_cleanup(handle_); }
  }

  CurlHandle(const CurlHandle&) = delete;
  CurlHandle& operator=(const CurlHandle&) = delete;

  CurlHandle(CurlHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  CURL* get() const { return handle_; }

 private:
  CURL* handle_;
};

// RAII wrapper for curl_slist.
class CurlHeaders {
 public:
  CurlHeaders() = default;
  ~CurlHeaders() {
    if (list_) { curl_slist_free_all(list_); }
  }

  CurlHeaders(const CurlHeaders&) = delete;
  CurlHeaders& operator=(const CurlHeaders&) = delete;

  void Append(const std::string& header) {
    list_ = curl_slist_append(list_, header.c_str());
  }

  curl_slist* get() const { return list_; }

 private:
  curl_slist* list_ = nullptr;
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Result of a non-streaming HTTP request.
struct CurlResponse {
  long status_code = 0;
  std::string body;
  std::string content_type;
};

// Callback type for streaming data.  Return false to abort the transfer.
using StreamWriteCallback = std::function<bool(const char* data, size_t len)>;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

inline size_t WriteToString(char* ptr, size_t size, size_t nmemb,
                            void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  size_t bytes = size * nmemb;
  out->append(ptr, bytes);
  return bytes;
}

struct HeaderCapture {
  std::string content_type;
};

inline size_t HeaderCallback(char* buffer, size_t size, size_t nitems,
                             void* userdata) {
  auto* cap = static_cast<HeaderCapture*>(userdata);
  size_t bytes = size * nitems;
  std::string line(buffer, bytes);
  if (line.size() > 14) {
    std::string prefix = line.substr(0, 13);
    for (auto& c : prefix) { c = static_cast<char>(std::tolower(c)); }
    if (prefix == "content-type:") {
      std::string val = line.substr(13);
      size_t start = val.find_first_not_of(" \t");
      if (start != std::string::npos) { val = val.substr(start); }
      while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) {
        val.pop_back();
      }
      cap->content_type = val;
    }
  }
  return bytes;
}

struct StreamCtx {
  const StreamWriteCallback* callback;
  const std::atomic<bool>* cancel;
};

inline size_t StreamWriteFn(char* ptr, size_t size, size_t nmemb,
                            void* userdata) {
  auto* ctx = static_cast<StreamCtx*>(userdata);
  if (ctx->cancel->load()) { return 0; }
  size_t bytes = size * nmemb;
  if (!(*ctx->callback)(ptr, bytes)) { return 0; }
  return bytes;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// SSL and timeout configuration
// ---------------------------------------------------------------------------

// Configure SSL on a curl handle.  Verification is always enabled.
// On Android, points curl to the system CA certificate directory so that
// BoringSSL (which has no built-in CA store) can verify server certificates.
inline void ConfigureSsl(CURL* curl) {
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef __ANDROID__
  // Android stores system CA certs as individual PEM files here.
  curl_easy_setopt(curl, CURLOPT_CAPATH, "/system/etc/security/cacerts");
#endif
  // Don't proxy localhost connections (test mock servers).
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "localhost,127.0.0.1");
}

inline void ConfigureTimeouts(CURL* curl,
                              std::chrono::seconds connect_timeout,
                              std::chrono::seconds read_timeout) {
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                   static_cast<long>(connect_timeout.count()));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                   static_cast<long>(read_timeout.count()));
}

// ---------------------------------------------------------------------------
// HTTP operations
// ---------------------------------------------------------------------------

// Perform a POST request and return the full response.
inline CurlResponse CurlPost(const std::string& url,
                              const std::vector<std::string>& headers,
                              const std::string& body,
                              std::chrono::seconds connect_timeout,
                              std::chrono::seconds read_timeout) {
  EnsureCurlInit();
  CurlHandle curl;
  CurlHeaders hdr;
  for (const auto& h : headers) { hdr.Append(h); }

  CurlResponse response;
  detail::HeaderCapture hcap;

  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, hdr.get());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, detail::WriteToString);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION,
                   detail::HeaderCallback);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &hcap);

  ConfigureSsl(curl.get());
  ConfigureTimeouts(curl.get(), connect_timeout, read_timeout);

  CURLcode res = curl_easy_perform(curl.get());
  if (res != CURLE_OK) {
    throw std::runtime_error(std::string("HTTP request failed: ") +
                             curl_easy_strerror(res));
  }

  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status_code);
  response.content_type = hcap.content_type;
  return response;
}

// Streaming POST: calls on_data for each chunk received.
// Returns the HTTP status code.  Throws on network error.
inline long CurlPostStreaming(
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::string& body,
    std::chrono::seconds connect_timeout,
    std::chrono::seconds read_timeout,
    const StreamWriteCallback& on_data,
    const std::atomic<bool>& cancel_flag) {
  EnsureCurlInit();
  CurlHandle curl;
  CurlHeaders hdr;
  for (const auto& h : headers) { hdr.Append(h); }

  detail::StreamCtx ctx{&on_data, &cancel_flag};

  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, hdr.get());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, detail::StreamWriteFn);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);

  ConfigureSsl(curl.get());
  // For streaming: only set connect timeout, no overall timeout.
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT,
                   static_cast<long>(connect_timeout.count()));
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 0L);

  CURLcode res = curl_easy_perform(curl.get());

  if (cancel_flag.load()) {
    return 0;  // Cancelled.
  }

  if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
    throw std::runtime_error(std::string("HTTP streaming request failed: ") +
                             curl_easy_strerror(res));
  }

  long status_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status_code);
  return status_code;
}

// URL-encode a string using curl.
inline std::string CurlEscape(const std::string& s) {
  EnsureCurlInit();
  CurlHandle handle;
  char* encoded = curl_easy_escape(handle.get(), s.c_str(),
                                   static_cast<int>(s.size()));
  std::string result(encoded);
  curl_free(encoded);
  return result;
}

}  // namespace shizuru::services
