#pragma once

#include <memory>
#include <string>

#include "baidu/baidu_config.h"
#include "baidu/token_manager.h"

namespace shizuru::services {

// Baidu ASR client — converts audio to text via Baidu Speech Recognition API.
class BaiduAsrClient {
 public:
  BaiduAsrClient(BaiduConfig config,
                 std::shared_ptr<BaiduTokenManager> token_mgr);

  // Transcribe audio to text. audio_data is raw audio bytes.
  // Returns transcript string. Empty on failure.
  std::string Transcribe(const std::string& audio_data,
                         const std::string& mime_type);

 private:
  static constexpr char MODULE_NAME[] = "BaiduASR";

  // Detect audio format from MIME type.
  std::string DetectFormat(const std::string& mime_type) const;

  BaiduConfig config_;
  std::shared_ptr<BaiduTokenManager> token_mgr_;
};

}  // namespace shizuru::services
