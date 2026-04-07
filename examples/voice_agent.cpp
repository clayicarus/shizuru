// Voice agent example: Microphone → VAD → ASR → AgentRuntime (LLM) → TTS → Speaker
//
// Pipeline topology (DMA routes registered manually below):
//
//   [AudioCaptureDevice]    audio_out ──► [PcmDumpDevice]      pass_in   (capture.pcm)
//   [PcmDumpDevice]         pass_out  ──► [EnergyVadDevice]    audio_in
//   [EnergyVadDevice]       audio_out ──► [PcmDumpDevice]      pass_in   (vad_dump.pcm)
//   [PcmDumpDevice]         pass_out  ──► [BaiduAsrDevice]     audio_in
//   [EnergyVadDevice]       vad_out   ──► [VadEventDevice]     vad_in
//   [BaiduAsrDevice]        text_out  ──► [core]               text_in
//   [core]                  text_out  ──► app_output (AgentRuntime built-in sink)
//   [ElevenLabsTtsDevice]   audio_out ──► [PcmDumpDevice]      pass_in   (playout_dump.pcm)
//   [PcmDumpDevice]         pass_out  ──► [AudioPlayoutDevice] audio_in
//
// Control routes registered automatically by AgentRuntime::StartSession:
//
//   [VadEventDevice]        vad_out   ──► [core]               vad_in
//   [core]                  control_out ► [BaiduAsrDevice]     control_in
//   [core]                  control_out ► [ElevenLabsTtsDevice] control_in
//   [core]                  control_out ► [AudioPlayoutDevice] control_in
//   [core]                  action_out  ► [ToolDispatchDevice] action_in
//   [ToolDispatchDevice]    result_out  ► [core]               tool_result_in
//
// The TTS device is driven by the AgentRuntime output callback: when the LLM
// produces a final text response, it is fed directly into ElevenLabsTtsDevice.
//
// All audio routes use requires_control_plane = false (DMA path).
// The LLM/controller path is handled internally by AgentRuntime.
//
// Usage:
//   export BAIDU_API_KEY=...
//   export BAIDU_SECRET_KEY=...
//   export OPENAI_API_KEY=...
//   export ELEVENLABS_API_KEY=...
//   ./voice_agent [--base-url <url>] [--model <model>] [--voice-id <id>] [--debug]
//
// Speak naturally — VAD handles segmentation. Ctrl+C to quit.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>
#include "async_logger.h"
#include "io/data_frame.h"
#include "io/asr/baidu/baidu_asr_device.h"
#include "io/tts/elevenlabs/elevenlabs_tts_device.h"
#include "io/vad/energy_vad_device.h"
#include "io/vad/vad_event_device.h"
#include "audio/audio_capture_device.h"
#include "io/probe/pcm_dump_device.h"
#include "audio/audio_playout_device.h"
#include "audio_device/port_audio/pa_player.h"
#include "audio_device/port_audio/pa_recorder.h"
#include "utils/baidu/baidu_config.h"
#include "tts/config.h"
#include "runtime/agent_runtime.h"
#include "runtime/route_table.h"
#include "io/tool_registry.h"
#include "llm/config.h"
#include "strategies/llm_observation_filter.h"
#include "strategies/tts_segment_strategy.h"
#include "strategies/response_filter.h"
#include "llm/openai/openai_client.h"

using namespace shizuru;

int main(int argc, char* argv[]) {
  // ── CLI args ──────────────────────────────────────────────────────────────
  bool debug_mode = false;
  std::string base_url = "https://dashscope.aliyuncs.com";
  std::string model    = "qwen3-coder-next";
  std::string voice_id;  // empty → ElevenLabsConfig default (Rachel)

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--debug") {
      debug_mode = true;
    } else if (arg == "--base-url" && i + 1 < argc) {
      base_url = argv[++i];
    } else if (arg == "--model" && i + 1 < argc) {
      model = argv[++i];
    } else if (arg == "--voice-id" && i + 1 < argc) {
      voice_id = argv[++i];
    }
  }

  // ── Environment ───────────────────────────────────────────────────────────
  const char* baidu_ak      = std::getenv("BAIDU_API_KEY");
  const char* baidu_sk      = std::getenv("BAIDU_SECRET_KEY");
  const char* openai_key    = std::getenv("OPENAI_API_KEY");
  const char* elevenlabs_key = std::getenv("ELEVENLABS_API_KEY");

  if (baidu_ak == nullptr || baidu_sk == nullptr) {
    std::fprintf(stderr,
                 "Error: set BAIDU_API_KEY and BAIDU_SECRET_KEY env vars.\n");
    return 1;
  }
  if (openai_key == nullptr) {
    std::fprintf(stderr, "Error: set OPENAI_API_KEY env var.\n");
    return 1;
  }
  if (elevenlabs_key == nullptr) {
    std::fprintf(stderr, "Error: set ELEVENLABS_API_KEY env var.\n");
    return 1;
  }

  // ── Logger ────────────────────────────────────────────────────────────────
  core::LoggerConfig log_cfg;
  log_cfg.level = debug_mode ? spdlog::level::debug : spdlog::level::info;
  core::InitLogger(log_cfg);

  // ── Baidu config ──────────────────────────────────────────────────────────
  services::BaiduConfig baidu_cfg;
  baidu_cfg.api_key    = baidu_ak;
  baidu_cfg.secret_key = baidu_sk;
  baidu_cfg.aue        = 5;      // PCM 16kHz output from TTS
  baidu_cfg.per        = 0;      // female voice
  baidu_cfg.asr_format = "pcm";

  // ── Audio config ──────────────────────────────────────────────────────────
  constexpr int    kRate = 16000;
  constexpr size_t kCh   = 1;
  constexpr size_t kFpb  = 320;  // 20ms at 16kHz

  io::RecorderConfig rec_cfg;
  rec_cfg.sample_rate             = kRate;
  rec_cfg.channel_count           = kCh;
  rec_cfg.frames_per_buffer       = kFpb;
  rec_cfg.buffer_capacity_samples = static_cast<size_t>(kRate) * 5;

  io::PlayerConfig play_cfg;
  play_cfg.sample_rate             = kRate;
  play_cfg.channel_count           = kCh;
  play_cfg.frames_per_buffer       = kFpb;
  play_cfg.buffer_capacity_samples = static_cast<size_t>(kRate) * 10;

  // ── Shared token manager ──────────────────────────────────────────────────
  auto token_mgr = std::make_shared<services::BaiduTokenManager>(baidu_cfg);
  token_mgr->GetToken();  // pre-warm

  // ── Voice devices (owned by AgentRuntime via RegisterDevice) ─────────────
  auto capture = std::make_unique<io::AudioCaptureDevice>(
      std::make_unique<io::PaRecorder>(rec_cfg));
  auto capture_dump  = std::make_unique<io::PcmDumpDevice>("capture");
  auto vad_dump      = std::make_unique<io::PcmDumpDevice>("vad_dump");
  auto playout_dump  = std::make_unique<io::PcmDumpDevice>("playout_dump");
  auto playout = std::make_unique<io::AudioPlayoutDevice>(
      std::make_unique<io::PaPlayer>(play_cfg));

  io::EnergyVadConfig vad_cfg;
  vad_cfg.energy_threshold        = 400.0F;
  vad_cfg.speech_onset_frames     = 3;   // ~60ms
  vad_cfg.silence_hangover_frames = 20;  // ~400ms
  vad_cfg.pre_roll_frames         = 3;
  auto vad = std::make_unique<io::EnergyVadDevice>(vad_cfg);

  auto asr = std::make_unique<io::BaiduAsrDevice>(baidu_cfg, token_mgr);

  // ElevenLabs TTS: pcm_16000 matches the pipeline sample rate.
  services::ElevenLabsConfig el_cfg;
  el_cfg.api_key       = elevenlabs_key;
  el_cfg.output_format = services::TtsOutputFormat::kPcm16000;
  if (!voice_id.empty()) { el_cfg.voice_id = voice_id; }
  auto tts = std::make_unique<io::ElevenLabsTtsDevice>(el_cfg);

  // Keep raw pointers before moving ownership into the runtime.
  io::ElevenLabsTtsDevice*  tts_ptr = tts.get();

  // VadEventDevice: emits vad/event frames on vad_out (routed to core:vad_in).
  auto asr_flush = std::make_unique<io::VadEventDevice>();

  // ── AgentRuntime config ───────────────────────────────────────────────────
  runtime::RuntimeConfig rt_cfg;
  rt_cfg.logger                        = log_cfg;
  rt_cfg.llm.base_url                  = base_url;
  rt_cfg.llm.api_path                  = "/compatible-mode/v1/chat/completions";
  rt_cfg.llm.api_key                   = openai_key;
  rt_cfg.llm.model                     = model;
  rt_cfg.llm.connect_timeout           = std::chrono::seconds(10);
  rt_cfg.llm.read_timeout              = std::chrono::seconds(60);
  rt_cfg.context.default_system_instruction =
      "You are a helpful voice assistant. Keep responses concise and natural "
      "for speech. Avoid markdown formatting.";
  rt_cfg.controller.max_turns          = 100;
  rt_cfg.controller.use_streaming      = true;  // Enable SSE streaming

  // ── Strategy factories ────────────────────────────────────────────────────
  // ObservationFilter: use a lightweight LLM to classify ASR transcripts.
  // Reuses the same API endpoint but could use a cheaper/faster model.
  rt_cfg.observation_filter_factory = [&]() {
    services::OpenAiConfig filter_llm_cfg;
    filter_llm_cfg.base_url        = base_url;
    filter_llm_cfg.api_path        = "/compatible-mode/v1/chat/completions";
    filter_llm_cfg.api_key         = openai_key;
    filter_llm_cfg.model           = model;  // could use a lighter model
    filter_llm_cfg.max_tokens      = 8;      // only need "yes" or "no"
    filter_llm_cfg.temperature     = 0.0;
    filter_llm_cfg.connect_timeout = std::chrono::seconds(5);
    filter_llm_cfg.read_timeout    = std::chrono::seconds(10);
    return std::make_unique<core::LlmObservationFilter>(
        std::make_unique<services::OpenAiClient>(filter_llm_cfg));
  };

  // TtsSegmentStrategy: punctuation-based sentence segmentation for TTS.
  rt_cfg.tts_segment_factory = []() {
    core::PunctuationSegmentStrategy::Config seg_cfg;
    seg_cfg.min_chars = 15;
    seg_cfg.max_chars = 200;
    return std::make_unique<core::PunctuationSegmentStrategy>(seg_cfg);
  };

  // ResponseFilter: strip <think> tags from LLM output.
  rt_cfg.response_filter_factory = []() {
    return std::make_unique<core::StripThinkingFilter>();
  };

  services::ToolRegistry tools;  // no tools for this example

  runtime::AgentRuntime runtime(rt_cfg, tools);

  // ── Register voice devices ────────────────────────────────────────────────
  runtime.RegisterDevice(std::move(capture));
  runtime.RegisterDevice(std::move(capture_dump));
  runtime.RegisterDevice(std::move(vad));
  runtime.RegisterDevice(std::move(vad_dump));
  runtime.RegisterDevice(std::move(asr_flush));
  runtime.RegisterDevice(std::move(asr));
  runtime.RegisterDevice(std::move(tts));
  runtime.RegisterDevice(std::move(playout_dump));
  runtime.RegisterDevice(std::move(playout));

  // ── Routes (all DMA — requires_control_plane = false) ────────────────────
  constexpr runtime::RouteOptions kDma{.requires_control_plane = false};

  // capture → dump → vad
  runtime.AddRoute({"audio_capture", "audio_out"},
                   {"capture",       io::PcmDumpDevice::kPassIn}, kDma);
  runtime.AddRoute({"capture",       io::PcmDumpDevice::kPassOut},
                   {"vad",           io::EnergyVadDevice::kAudioIn}, kDma);

  // vad audio_out (speech frames) → vad_dump → asr
  runtime.AddRoute({"vad",      io::EnergyVadDevice::kAudioOut},
                   {"vad_dump", io::PcmDumpDevice::kPassIn}, kDma);
  runtime.AddRoute({"vad_dump", io::PcmDumpDevice::kPassOut},
                   {"baidu_asr","audio_in"}, kDma);

  // vad vad_out (events) → asr_flush (triggers Flush on speech_end)
  runtime.AddRoute({"vad",       io::EnergyVadDevice::kVadOut},
                   {"vad_event", io::VadEventDevice::kVadIn}, kDma);

  // asr text_out → core text_in (LLM reasoning)
  runtime.AddRoute({"baidu_asr", "text_out"},
                   {"core",      "text_in"}, kDma);

  // tts audio_out → playout_dump → playout
  runtime.AddRoute({"elevenlabs_tts", "audio_out"},
                   {"playout_dump",   io::PcmDumpDevice::kPassIn}, kDma);
  runtime.AddRoute({"playout_dump",   io::PcmDumpDevice::kPassOut},
                   {"audio_playout",  "audio_in"}, kDma);

  // ── Output callback: LLM response → TTS ──────────────────────────────────
  // Streaming tokens are printed in real-time; only the final complete response
  // is fed into TTS to avoid fragmented audio synthesis.
  runtime.OnOutput([tts_ptr](const runtime::RuntimeOutput& output) {
    if (output.is_partial) {
      // Print streaming token without newline for real-time display.
      std::printf("%s", output.text.c_str());
      std::fflush(stdout);
      return;
    }

    // Final response: newline + feed into TTS.
    std::printf("\n[agent] %s\n", output.text.c_str());
    std::fflush(stdout);

    io::DataFrame frame;
    frame.type    = "text/plain";
    frame.payload = std::vector<uint8_t>(output.text.begin(), output.text.end());
    frame.source_device = "app_output";
    frame.source_port   = "text_out";
    frame.timestamp     = std::chrono::steady_clock::now();
    tts_ptr->OnInput("text_in", std::move(frame));
  });

  // ── Start ─────────────────────────────────────────────────────────────────
  runtime.StartSession();

  std::printf("=== Voice Agent (VAD + Baidu ASR + %s LLM + ElevenLabs TTS) ===\n",
              model.c_str());
  std::printf("Log level : %s\n",
              debug_mode ? "debug (all frames)" : "info (events + text)");
  std::printf("Speak naturally — VAD detects speech automatically.\n");
  std::printf("Ctrl+C to quit.\n\n");

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  runtime.Shutdown();
  return 0;
}
