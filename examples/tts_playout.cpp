#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "async_logger.h"
#include "audio_device/port_audio/pa_player.h"
#include "tts/config.h"
#include "tts/elevenlabs_client.h"

static void PrintUsage(const char* prog) {
  std::fprintf(stderr,
    "Usage: %s [options] [text]\n"
    "\n"
    "Options:\n"
    "  --voice_id <id>    ElevenLabs voice ID (overrides default)\n"
    "  --model_id <id>    ElevenLabs model ID (overrides default)\n"
    "\n"
    "Environment:\n"
    "  ELEVENLABS_API_KEY  Required API key\n"
    "\n"
    "Example:\n"
    "  %s --voice_id 21m00Tcm4TlvDq8ikWAM \"Hello world\"\n",
    prog, prog);
}

int main(int argc, char* argv[]) {
  shizuru::core::InitLogger();

  std::string text;
  std::string voice_id;
  std::string model_id;

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--voice_id") == 0 && i + 1 < argc) {
      voice_id = argv[++i];
    } else if (std::strcmp(argv[i], "--model_id") == 0 && i + 1 < argc) {
      model_id = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0) {
      PrintUsage(argv[0]);
      return 0;
    } else if (argv[i][0] != '-') {
      text = argv[i];
    } else {
      std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
      PrintUsage(argv[0]);
      return 1;
    }
  }

  if (text.empty()) {
    text = "Hello! This is a streaming text-to-speech demo using ElevenLabs.";
  }

  const char* api_key = std::getenv("ELEVENLABS_API_KEY");
  if (api_key == nullptr || api_key[0] == '\0') {
    std::fprintf(stderr, "Error: ELEVENLABS_API_KEY not set.\n");
    return 1;
  }

  try {
    // ── TTS client ────────────────────────────────────────────────────────
    shizuru::services::ElevenLabsConfig tts_cfg;
    tts_cfg.api_key       = api_key;
    tts_cfg.output_format = shizuru::services::TtsOutputFormat::kPcm16000;
    tts_cfg.optimize_streaming_latency = 3;
    if (!voice_id.empty()) { tts_cfg.voice_id = voice_id; }
    if (!model_id.empty()) { tts_cfg.model_id = model_id; }

    shizuru::services::ElevenLabsClient tts(tts_cfg);

    // ── Audio player — must match TTS output format ───────────────────────
    constexpr double kSampleRate      = 16000.0;
    constexpr size_t kChannels        = 1;
    constexpr size_t kFramesPerBuffer = 320;  // 20ms at 16kHz

    shizuru::io::PlayerConfig play_cfg;
    play_cfg.sample_rate            = kSampleRate;
    play_cfg.channels               = kChannels;
    play_cfg.frames_per_buffer      = kFramesPerBuffer;
    play_cfg.format                 = shizuru::io::SampleFormat::kInt16;
    play_cfg.buffer_capacity_frames = static_cast<size_t>(kSampleRate) * 10;

    auto player = std::make_unique<shizuru::io::PaPlayer>(play_cfg);
    player->Start();

    std::printf("Voice:  %s\n", tts_cfg.voice_id.c_str());
    std::printf("Model:  %s\n", tts_cfg.model_id.c_str());
    std::printf("Text:   \"%s\"\n", text.c_str());
    std::printf("Streaming audio to output device...\n");

    // ── Stream TTS → player ───────────────────────────────────────────────
    // The TTS callback delivers raw bytes from the HTTP stream. For s16le PCM,
    // chunks may have an odd byte count, splitting a sample across two chunks.
    // We buffer the leftover byte and stitch it with the next chunk here,
    // at the consumer layer — the TTS client delivers raw bytes as-is.
    shizuru::services::TtsRequest req;
    req.text = text;

    uint8_t carry = 0;
    bool    has_carry = false;
    size_t  total_bytes = 0;

    tts.Synthesize(req, [&](const void* data, size_t bytes) {
      total_bytes += bytes;
      const auto* src = static_cast<const uint8_t*>(data);
      size_t offset = 0;

      if (has_carry) {
        // Complete the split sample: [carry | src[0]]
        uint8_t pair[2] = {carry, src[0]};
        player->Write(pair, 1);  // 1 frame = 2 bytes for s16le mono
        has_carry = false;
        offset = 1;
      }

      const size_t remaining = bytes - offset;
      const size_t frames    = remaining / sizeof(int16_t);
      if (frames > 0) {
        player->Write(src + offset, frames);
      }

      if ((remaining % sizeof(int16_t)) != 0) {
        carry = src[offset + frames * sizeof(int16_t)];
        has_carry = true;
      }
    });

    std::printf("Received %zu bytes (%.1f s). Draining...\n",
                total_bytes,
                static_cast<double>(total_bytes) / (kSampleRate * sizeof(int16_t)));

    // ── Drain: wait until the ring buffer is empty ────────────────────────
    while (player->Buffered() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    player->Stop();
    std::printf("Done.\n");

  } catch (const std::exception& e) {
    std::fprintf(stderr, "Error: %s\n", e.what());
    return 1;
  }
  return 0;
}
