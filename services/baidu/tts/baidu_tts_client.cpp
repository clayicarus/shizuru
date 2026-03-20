#include "baidu/tts/baidu_tts_client.h"

#include <cstdio>
#include <stdexcept>

#include <httplib.h>

#include "async_logger.h"

namespace shizuru::services {

namespace {

// Percent-encode a UTF-8 string for use in application/x-www-form-urlencoded.
std::string UrlEncode(const std::string& s) {
  std::string result;
  result.reserve(s.size() * 3);  // worst case
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      result.push_back(static_cast<char>(c));
    } else {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      result.append(buf);
    }
  }
  return result;
}

}  // namespace

BaiduTtsClient::BaiduTtsClient(BaiduConfig config,
                               std::shared_ptr<BaiduTokenManager> token_mgr)
    : config_(std::move(config)), token_mgr_(std::move(token_mgr)) {}

std::string BaiduTtsClient::Synthesize(const std::string& text,
                                       std::string& mime_type) {
  LOG_INFO("[{}] Synthesize text_len={}", MODULE_NAME, text.size());

  std::string token = token_mgr_->GetToken();

  httplib::Client cli(config_.tts_host);
  cli.set_connection_timeout(config_.connect_timeout);
  cli.set_read_timeout(config_.read_timeout);

  // Build POST body (URL-encoded form).
  std::string body;
  body += "tex=" + UrlEncode(text);
  body += "&tok=" + token;
  body += "&cuid=" + config_.cuid;
  body += "&ctp=1";
  body += "&lan=zh";
  body += "&spd=" + std::to_string(config_.spd);
  body += "&pit=" + std::to_string(config_.pit);
  body += "&vol=" + std::to_string(config_.vol);
  body += "&per=" + std::to_string(config_.per);
  body += "&aue=" + std::to_string(config_.aue);

  auto res = cli.Post(config_.tts_path, body,
                      "application/x-www-form-urlencoded");

  if (!res) {
    LOG_ERROR("[{}] TTS request failed: no response", MODULE_NAME);
    throw std::runtime_error("Baidu TTS request failed: " +
                             httplib::to_string(res.error()));
  }

  if (res->status != 200) {
    LOG_ERROR("[{}] TTS status {}: {}", MODULE_NAME, res->status, res->body);
    throw std::runtime_error("Baidu TTS API returned status " +
                             std::to_string(res->status));
  }

  // Check Content-Type: audio/* means success; application/json means error.
  std::string content_type;
  if (res->has_header("Content-Type")) {
    content_type = res->get_header_value("Content-Type");
  }

  if (content_type.find("audio/") != std::string::npos) {
    // Determine MIME type from aue parameter.
    switch (config_.aue) {
      case 3:
        mime_type = "audio/mp3";
        break;
      case 4:
        mime_type = "audio/pcm";
        break;
      case 5:
        mime_type = "audio/pcm";
        break;
      case 6:
        mime_type = "audio/wav";
        break;
      default:
        mime_type = "audio/mp3";
        break;
    }
    LOG_INFO("[{}] TTS success, audio_len={} mime={}", MODULE_NAME,
             res->body.size(), mime_type);
    return res->body;
  }

  // Error response is JSON.
  LOG_ERROR("[{}] TTS error response: {}", MODULE_NAME, res->body);
  throw std::runtime_error("Baidu TTS returned error: " + res->body);
}

}  // namespace shizuru::services
