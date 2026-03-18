#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace shizuru::services {

// ---------------------------------------------------------------------------
// Output format
// ---------------------------------------------------------------------------

// Output format of the generated audio.
// PCM formats return raw signed-16-bit little-endian samples (mono).
// MP3 formats return compressed audio.
// Note: pcm_44100 and mp3_*_192 require Creator/Pro tier subscription.
enum class TtsOutputFormat {
  kPcm16000,       // pcm_16000
  kPcm22050,       // pcm_22050
  kPcm24000,       // pcm_24000  (recommended — low latency, no decoder needed)
  kPcm44100,       // pcm_44100  (Pro tier)
  kMp3_22050_32,   // mp3_22050_32
  kMp3_44100_32,   // mp3_44100_32
  kMp3_44100_64,   // mp3_44100_64
  kMp3_44100_96,   // mp3_44100_96
  kMp3_44100_128,  // mp3_44100_128  (default)
  kMp3_44100_192,  // mp3_44100_192  (Creator tier)
  kUlaw8000,       // ulaw_8000  (Twilio)
};

inline const char* TtsOutputFormatString(TtsOutputFormat fmt) {
  switch (fmt) {
    case TtsOutputFormat::kPcm16000:      return "pcm_16000";
    case TtsOutputFormat::kPcm22050:      return "pcm_22050";
    case TtsOutputFormat::kPcm24000:      return "pcm_24000";
    case TtsOutputFormat::kPcm44100:      return "pcm_44100";
    case TtsOutputFormat::kMp3_22050_32:  return "mp3_22050_32";
    case TtsOutputFormat::kMp3_44100_32:  return "mp3_44100_32";
    case TtsOutputFormat::kMp3_44100_64:  return "mp3_44100_64";
    case TtsOutputFormat::kMp3_44100_96:  return "mp3_44100_96";
    case TtsOutputFormat::kMp3_44100_128: return "mp3_44100_128";
    case TtsOutputFormat::kMp3_44100_192: return "mp3_44100_192";
    case TtsOutputFormat::kUlaw8000:      return "ulaw_8000";
  }
  return "pcm_24000";
}

// Returns the sample rate (Hz) for PCM formats; 0 for compressed formats.
inline int TtsOutputFormatSampleRate(TtsOutputFormat fmt) {
  switch (fmt) {
    case TtsOutputFormat::kPcm16000:  return 16000;
    case TtsOutputFormat::kPcm22050:  return 22050;
    case TtsOutputFormat::kPcm24000:  return 24000;
    case TtsOutputFormat::kPcm44100:  return 44100;
    default:                          return 0;
  }
}

// ---------------------------------------------------------------------------
// text_normalization enum
// ---------------------------------------------------------------------------

enum class TextNormalization { kAuto, kOn, kOff };

inline const char* TextNormalizationString(TextNormalization v) {
  switch (v) {
    case TextNormalization::kAuto: return "auto";
    case TextNormalization::kOn:   return "on";
    case TextNormalization::kOff:  return "off";
  }
  return "auto";
}

// ---------------------------------------------------------------------------
// Voice settings (per-request override)
// ---------------------------------------------------------------------------

struct VoiceSettings {
  double stability        = 0.5;
  double similarity_boost = 0.75;
  double style            = 0.0;
  bool   use_speaker_boost = true;
};

// ---------------------------------------------------------------------------
// Per-request parameters (mirrors the API request body)
// ---------------------------------------------------------------------------

struct TtsRequest {
  // Required
  std::string text;

  // Voice — if empty, falls back to ElevenLabsConfig::voice_id
  std::string voice_id;

  // Optional body fields
  std::string model_id;          // defaults to config value if empty
  std::string language_code;     // ISO 639-1, e.g. "en", "ja"

  std::optional<VoiceSettings> voice_settings;  // overrides stored settings

  std::optional<int>  seed;          // 0–4294967295 for deterministic output
  std::string         previous_text;
  std::string         next_text;
  std::vector<std::string> previous_request_ids;  // max 3
  std::vector<std::string> next_request_ids;       // max 3

  TextNormalization apply_text_normalization = TextNormalization::kAuto;
  bool apply_language_text_normalization = false;
};

// ---------------------------------------------------------------------------
// Client configuration
// ---------------------------------------------------------------------------

struct ElevenLabsConfig {
  std::string api_key;
  std::string base_url = "https://api.elevenlabs.io";

  // Default voice used when TtsRequest::voice_id is empty.
  std::string voice_id = "21m00Tcm4TlvDq8ikWAM";  // Rachel

  // Default model.
  std::string model_id = "eleven_multilingual_v2";

  // Audio output format.
  TtsOutputFormat output_format = TtsOutputFormat::kPcm24000;

  // Latency optimization level: 0 (none) – 4 (max).
  // Deprecated by ElevenLabs but still functional.
  int optimize_streaming_latency = 3;

  // Whether to enable request logging on ElevenLabs side.
  bool enable_logging = true;

  std::chrono::seconds connect_timeout{10};
  std::chrono::seconds read_timeout{60};
};

}  // namespace shizuru::services
