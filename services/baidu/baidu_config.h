#pragma once

#include <chrono>
#include <string>

namespace shizuru::services {

// Configuration for Baidu Voice API (ASR + TTS).
struct BaiduConfig {
  std::string api_key;
  std::string secret_key;

  // Token endpoint
  std::string token_url = "https://aip.baidubce.com";
  std::string token_path = "/oauth/2.0/token";

  // TTS endpoint
  std::string tts_host = "https://tsn.baidu.com";
  std::string tts_path = "/text2audio";

  // ASR endpoint
  std::string asr_host = "https://vop.baidu.com";
  std::string asr_path = "/server_api";

  // Client unique identifier (any string, e.g. MAC address or machine name).
  std::string cuid = "shizuru_agent";

  // TTS parameters
  int spd = 5;   // Speed 0-15, default 5
  int pit = 5;   // Pitch 0-15, default 5
  int vol = 5;   // Volume 0-15, default 5
  int per = 0;   // Voice: 0=female, 1=male, 3=emotional_male, 4=emotional_female
  int aue = 6;   // Audio format: 3=mp3, 4=pcm-8k, 5=pcm-16k, 6=wav

  // ASR parameters
  std::string asr_format = "wav";  // pcm, wav, amr, m4a
  int asr_rate = 16000;            // Sample rate: 16000
  int asr_dev_pid = 1537;          // Model: 1537=mandarin+punctuation

  // Timeouts
  std::chrono::seconds connect_timeout{10};
  std::chrono::seconds read_timeout{30};
};

}  // namespace shizuru::services
