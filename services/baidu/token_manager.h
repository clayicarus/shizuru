#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include "baidu/baidu_config.h"

namespace shizuru::services {

// Manages Baidu API access_token lifecycle (fetch + auto-refresh).
class BaiduTokenManager {
 public:
  explicit BaiduTokenManager(BaiduConfig config);

  // Returns a valid access_token, refreshing if expired.
  // Throws std::runtime_error on failure.
  std::string GetToken();

 private:
  void Refresh();

  static constexpr char MODULE_NAME[] = "BaiduToken";

  BaiduConfig config_;

  std::mutex mutex_;
  std::string access_token_;
  std::chrono::steady_clock::time_point expires_at_;
};

}  // namespace shizuru::services
