// Quick integration test for Baidu ASR + TTS clients.
// Usage: ./baidu_voice_test
//
// This test:
//   1. Calls TTS to synthesize a short Chinese sentence into audio.
//   2. Calls ASR to transcribe the audio back to text.
//   3. Prints the round-trip result.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include "baidu/baidu_config.h"
#include "baidu/token_manager.h"
#include "baidu/tts/baidu_tts_client.h"
#include "baidu/asr/baidu_asr_client.h"

int main() {
  std::printf("=== Baidu Voice Integration Test ===\n\n");

  shizuru::services::BaiduConfig config;

  // Read keys from environment variables, or use hardcoded test keys.
  const char* env_ak = std::getenv("BAIDU_API_KEY");
  const char* env_sk = std::getenv("BAIDU_SECRET_KEY");

  if (env_ak && env_sk) {
    config.api_key    = env_ak;
    config.secret_key = env_sk;
  } else {
    // TODO: Set your own Baidu API Key and Secret Key here, or use env vars.
    // config.api_key    = "YOUR_API_KEY";
    // config.secret_key = "YOUR_SECRET_KEY";
    std::fprintf(stderr, "Error: Set BAIDU_API_KEY and BAIDU_SECRET_KEY env vars.\n");
    return 1;
  }

  config.aue = 6;  // wav format for round-trip test
  config.per = 0;  // female voice

  auto token_mgr = std::make_shared<shizuru::services::BaiduTokenManager>(config);
  shizuru::services::BaiduTtsClient tts(config, token_mgr);
  shizuru::services::BaiduAsrClient asr(config, token_mgr);

  // --- Step 1: TTS ---
  std::printf("[1] TTS: Synthesizing text...\n");
  std::string tts_text = "你好，这是一个语音合成测试。";
  std::string mime_type;
  std::string audio;

  try {
    audio = tts.Synthesize(tts_text, mime_type);
    std::printf("    OK: %zu bytes, mime=%s\n", audio.size(), mime_type.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "    FAIL: %s\n", e.what());
    return 1;
  }

  // Save audio to file for manual inspection.
  {
    std::ofstream f("baidu_tts_test.wav", std::ios::binary);
    f.write(audio.data(), static_cast<std::streamsize>(audio.size()));
    std::printf("    Saved to baidu_tts_test.wav\n");
  }

  // --- Step 2: ASR ---
  std::printf("[2] ASR: Transcribing audio back to text...\n");
  std::string transcript;

  try {
    transcript = asr.Transcribe(audio, mime_type);
    if (transcript.empty()) {
      std::fprintf(stderr, "    FAIL: empty transcript\n");
      return 1;
    }
    std::printf("    OK: \"%s\"\n", transcript.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "    FAIL: %s\n", e.what());
    return 1;
  }

  std::printf("\n[Result] TTS input:  \"%s\"\n", tts_text.c_str());
  std::printf("[Result] ASR output: \"%s\"\n", transcript.c_str());
  std::printf("\n=== Test complete ===\n");
  return 0;
}
