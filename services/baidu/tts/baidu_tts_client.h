#pragma once

#include <memory>
#include <string>

#include "baidu/baidu_config.h"
#include "baidu/token_manager.h"

namespace shizuru::services {

// Baidu TTS client — converts text to audio via Baidu Text-to-Speech API.
class BaiduTtsClient {
 public:
  BaiduTtsClient(BaiduConfig config,
                 std::shared_ptr<BaiduTokenManager> token_mgr);

  // Synthesize text into audio. Returns raw audio bytes.
  // mime_type is set to the output audio MIME type.
  // Throws std::runtime_error on failure.
  std::string Synthesize(const std::string& text, std::string& mime_type);

 private:
  static constexpr char MODULE_NAME[] = "BaiduTTS";

  BaiduConfig config_;
  std::shared_ptr<BaiduTokenManager> token_mgr_;
};

}  // namespace shizuru::services
