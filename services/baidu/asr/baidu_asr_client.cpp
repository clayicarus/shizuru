#include "baidu/asr/baidu_asr_client.h"

#include <stdexcept>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "async_logger.h"

namespace shizuru::services {

namespace {

// Base64 encoding table.
static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string& input) {
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);

  const auto* data = reinterpret_cast<const unsigned char*>(input.data());
  size_t len = input.size();

  for (size_t i = 0; i < len; i += 3) {
    unsigned int n = static_cast<unsigned int>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
    if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);

    output.push_back(kBase64Chars[(n >> 18) & 0x3F]);
    output.push_back(kBase64Chars[(n >> 12) & 0x3F]);
    output.push_back((i + 1 < len) ? kBase64Chars[(n >> 6) & 0x3F] : '=');
    output.push_back((i + 2 < len) ? kBase64Chars[n & 0x3F] : '=');
  }

  return output;
}

}  // namespace

BaiduAsrClient::BaiduAsrClient(BaiduConfig config,
                               std::shared_ptr<BaiduTokenManager> token_mgr)
    : config_(std::move(config)), token_mgr_(std::move(token_mgr)) {}

std::string BaiduAsrClient::DetectFormat(const std::string& mime_type) const {
  if (mime_type.find("wav") != std::string::npos) return "wav";
  if (mime_type.find("pcm") != std::string::npos) return "pcm";
  if (mime_type.find("amr") != std::string::npos) return "amr";
  if (mime_type.find("m4a") != std::string::npos) return "m4a";
  return config_.asr_format;
}

std::string BaiduAsrClient::Transcribe(const std::string& audio_data,
                                       const std::string& mime_type) {
  LOG_INFO("[{}] Transcribe audio_len={} mime={}", MODULE_NAME,
           audio_data.size(), mime_type);

  if (audio_data.empty()) {
    LOG_WARN("[{}] Empty audio data", MODULE_NAME);
    return "";
  }

  std::string token = token_mgr_->GetToken();
  std::string format = DetectFormat(mime_type);

  // Build JSON request body.
  nlohmann::json body;
  body["format"] = format;
  body["rate"] = config_.asr_rate;
  body["channel"] = 1;
  body["cuid"] = config_.cuid;
  body["token"] = token;
  body["dev_pid"] = config_.asr_dev_pid;
  body["speech"] = Base64Encode(audio_data);
  body["len"] = static_cast<int>(audio_data.size());

  std::string body_str = body.dump();

  httplib::Client cli(config_.asr_host);
  cli.set_connection_timeout(config_.connect_timeout);
  cli.set_read_timeout(config_.read_timeout);

  auto res = cli.Post(config_.asr_path, body_str, "application/json");

  if (!res) {
    LOG_ERROR("[{}] ASR request failed: no response", MODULE_NAME);
    return "";
  }

  if (res->status != 200) {
    LOG_ERROR("[{}] ASR status {}: {}", MODULE_NAME, res->status, res->body);
    return "";
  }

  LOG_DEBUG("[{}] ASR response: {}", MODULE_NAME, res->body);

  try {
    nlohmann::json j = nlohmann::json::parse(res->body);

    int err_no = j.value("err_no", -1);
    if (err_no != 0) {
      std::string err_msg = j.value("err_msg", "unknown error");
      LOG_ERROR("[{}] ASR error {}: {}", MODULE_NAME, err_no, err_msg);
      return "";
    }

    if (j.contains("result") && j["result"].is_array() &&
        !j["result"].empty()) {
      std::string transcript = j["result"][0].get<std::string>();
      LOG_INFO("[{}] Transcript: \"{}\"", MODULE_NAME, transcript);
      return transcript;
    }

    LOG_WARN("[{}] ASR returned no result", MODULE_NAME);
    return "";
  } catch (const std::exception& e) {
    LOG_ERROR("[{}] Failed to parse ASR response: {}", MODULE_NAME, e.what());
    return "";
  }
}

}  // namespace shizuru::services
