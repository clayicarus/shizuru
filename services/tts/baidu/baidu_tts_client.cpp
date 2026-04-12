#include "baidu_tts_client.h"

#include <stdexcept>

#include "async_logger.h"
#include "services/utils/curl_helper.h"

namespace shizuru::services {

BaiduTtsClient::BaiduTtsClient(BaiduConfig config,
                               std::shared_ptr<BaiduTokenManager> token_mgr)
    : config_(std::move(config)), token_mgr_(std::move(token_mgr)) {}

std::string BaiduTtsClient::Synthesize(const std::string& text,
                                       std::string& mime_type) {
  LOG_INFO("[{}] Synthesize text_len={}", MODULE_NAME, text.size());

  std::string token = token_mgr_->GetToken();

  std::string url = config_.tts_host + config_.tts_path;

  // Build POST body (URL-encoded form).
  std::string body;
  body += "tex=" + CurlEscape(text);
  body += "&tok=" + token;
  body += "&cuid=" + config_.cuid;
  body += "&ctp=1";
  body += "&lan=zh";
  body += "&spd=" + std::to_string(config_.spd);
  body += "&pit=" + std::to_string(config_.pit);
  body += "&vol=" + std::to_string(config_.vol);
  body += "&per=" + std::to_string(config_.per);
  body += "&aue=" + std::to_string(config_.aue);

  auto res = CurlPost(
      url,
      {"Content-Type: application/x-www-form-urlencoded"},
      body,
      config_.connect_timeout,
      config_.read_timeout);

  if (res.status_code != 200) {
    LOG_ERROR("[{}] TTS status {}: {}", MODULE_NAME, res.status_code, res.body);
    throw std::runtime_error("Baidu TTS API returned status " +
                             std::to_string(res.status_code));
  }

  // Check Content-Type: audio/* means success; application/json means error.
  if (res.content_type.find("audio/") != std::string::npos) {
    // Determine MIME type from aue parameter.
    switch (config_.aue) {
      case 3:  mime_type = "audio/mp3"; break;
      case 4:  // fallthrough
      case 5:  mime_type = "audio/pcm"; break;
      case 6:  mime_type = "audio/wav"; break;
      default: mime_type = "audio/mp3"; break;
    }
    LOG_INFO("[{}] TTS success, audio_len={} mime={}", MODULE_NAME,
             res.body.size(), mime_type);
    return res.body;
  }

  // Error response is JSON.
  LOG_ERROR("[{}] TTS error response: {}", MODULE_NAME, res.body);
  throw std::runtime_error("Baidu TTS returned error: " + res.body);
}

}  // namespace shizuru::services
