#include "baidu_token_manager.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

#include "async_logger.h"
#include "services/utils/curl_helper.h"

namespace shizuru::services {

BaiduTokenManager::BaiduTokenManager(BaiduConfig config)
    : config_(std::move(config)),
      expires_at_(std::chrono::steady_clock::time_point::min()) {}

std::string BaiduTokenManager::GetToken() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::chrono::steady_clock::now() < expires_at_ &&
      !access_token_.empty()) {
    return access_token_;
  }
  Refresh();
  return access_token_;
}

void BaiduTokenManager::Refresh() {
  LOG_INFO("[{}] Refreshing access token", MODULE_NAME);

  std::string url = config_.token_url + config_.token_path;

  std::string body = "grant_type=client_credentials"
                     "&client_id=" + config_.api_key +
                     "&client_secret=" + config_.secret_key;

  auto res = CurlPost(
      url,
      {"Content-Type: application/x-www-form-urlencoded"},
      body,
      config_.connect_timeout,
      config_.read_timeout);

  if (res.status_code != 200) {
    LOG_ERROR("[{}] Token request status {}: {}", MODULE_NAME,
              res.status_code, res.body);
    throw std::runtime_error("Baidu token API returned status " +
                             std::to_string(res.status_code) + ": " + res.body);
  }

  nlohmann::json j = nlohmann::json::parse(res.body);

  if (!j.contains("access_token") || !j["access_token"].is_string()) {
    LOG_ERROR("[{}] Invalid token response: {}", MODULE_NAME, res.body);
    throw std::runtime_error("Baidu token response missing access_token");
  }

  access_token_ = j["access_token"].get<std::string>();

  // expires_in is in seconds; subtract a 5-minute margin.
  int expires_in = j.value("expires_in", 2592000);
  expires_at_ = std::chrono::steady_clock::now() +
                std::chrono::seconds(expires_in) -
                std::chrono::minutes(5);

  LOG_INFO("[{}] Token refreshed, expires_in={}s", MODULE_NAME, expires_in);
}

}  // namespace shizuru::services
