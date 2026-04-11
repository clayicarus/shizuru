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
#include <unistd.h>

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
#include "strategies/llm_observation_aggregator.h"
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
  auto* capture_ptr = capture.get();
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
  // ObservationAggregator: LLM-based endpointing — buffers ASR fragments
  // until the user finishes speaking.
  rt_cfg.observation_aggregator_factory = [=]() {
    services::OpenAiConfig agg_llm_cfg;
    agg_llm_cfg.base_url        = base_url;
    agg_llm_cfg.api_path        = "/compatible-mode/v1/chat/completions";
    agg_llm_cfg.api_key         = openai_key;
    agg_llm_cfg.model           = model;
    agg_llm_cfg.max_tokens      = 8;
    agg_llm_cfg.temperature     = 0.0;
    agg_llm_cfg.connect_timeout = std::chrono::seconds(5);
    agg_llm_cfg.read_timeout    = std::chrono::seconds(10);

    core::LlmAggregatorConfig agg_cfg;
    agg_cfg.aggregation_timeout = std::chrono::milliseconds(5000);
    agg_cfg.llm_timeout         = std::chrono::milliseconds(2000);

    return std::make_unique<core::LlmObservationAggregator>(
        std::make_unique<services::OpenAiClient>(agg_llm_cfg),
        std::move(agg_cfg));
  };

  // ObservationFilter: LLM-based relevance check — decides if the complete
  // observation is worth sending to the main LLM.
  rt_cfg.observation_filter_factory = [=]() {
    services::OpenAiConfig filter_llm_cfg;
    filter_llm_cfg.base_url        = base_url;
    filter_llm_cfg.api_path        = "/compatible-mode/v1/chat/completions";
    filter_llm_cfg.api_key         = openai_key;
    filter_llm_cfg.model           = model;
    filter_llm_cfg.max_tokens      = 8;
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

  services::ToolRegistry tools;

  // ── Builtin tools ─────────────────────────────────────────────────────────
  tools.Register("get_current_time",
                 [](const std::string& /*args*/) -> services::ToolResult {
                   auto now = std::chrono::system_clock::now();
                   auto t = std::chrono::system_clock::to_time_t(now);
                   char buf[64];
                   std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
                                 std::localtime(&t));
                   return {true, buf, ""};
                 });

  tools.Register("get_system_info",
                 [](const std::string& /*args*/) -> services::ToolResult {
                   char hostname[256] = {};
                   gethostname(hostname, sizeof(hostname));
#if defined(__APPLE__)
                   const char* os = "macOS";
#elif defined(__linux__)
                   const char* os = "Linux";
#elif defined(_WIN32)
                   const char* os = "Windows";
#else
                   const char* os = "Unknown";
#endif
                   std::string info = std::string(R"({"os":")") + os +
                                      R"(","hostname":")" + hostname + R"("})";
                   return {true, info, ""};
                 });

  tools.Register("calculate",
                 [](const std::string& args) -> services::ToolResult {
                   // Simple eval: parse "expression" field, support +,-,*,/
                   // For safety, only handle a op b format.
                   auto expr_pos = args.find(R"("expression":")");
                   if (expr_pos == std::string::npos) {
                     return {false, "", "Missing 'expression' parameter"};
                   }
                   auto val_start = expr_pos + 15;
                   auto val_end = args.find('"', val_start);
                   if (val_end == std::string::npos) {
                     return {false, "", "Malformed expression"};
                   }
                   std::string expr = args.substr(val_start, val_end - val_start);

                   // Parse: number op number
                   double a = 0, b = 0;
                   char op = 0;
                   if (std::sscanf(expr.c_str(), "%lf %c %lf", &a, &op, &b) != 3) {
                     return {false, "", "Cannot parse expression: " + expr};
                   }
                   double result = 0;
                   switch (op) {
                     case '+': result = a + b; break;
                     case '-': result = a - b; break;
                     case '*': result = a * b; break;
                     case '/':
                       if (b == 0) return {false, "", "Division by zero"};
                       result = a / b;
                       break;
                     default:
                       return {false, "", std::string("Unknown operator: ") + op};
                   }
                   char buf[64];
                   std::snprintf(buf, sizeof(buf), "%.6g", result);
                   return {true, buf, ""};
                 });

  tools.Register("set_reminder",
                 [](const std::string& args) -> services::ToolResult {
                   // Parse message and minutes fields.
                   auto msg_pos = args.find(R"("message":")");
                   auto min_pos = args.find(R"("minutes":)");
                   std::string message = "reminder";
                   int minutes = 0;
                   if (msg_pos != std::string::npos) {
                     auto s = msg_pos + 11;
                     auto e = args.find('"', s);
                     if (e != std::string::npos) message = args.substr(s, e - s);
                   }
                   if (min_pos != std::string::npos) {
                     std::sscanf(args.c_str() + min_pos + 10, "%d", &minutes);
                   }
                   std::string result = R"({"status":"set","message":")" +
                                        message + R"(","minutes":)" +
                                        std::to_string(minutes) + "}";
                   return {true, result, ""};
                 });

  // ── Tool definitions for LLM (function calling schema) ────────────────────
  {
    services::ToolDefinition time_tool;
    time_tool.name = "get_current_time";
    time_tool.description = "Get the current date and time";
    time_tool.required_capability = "builtin";
    rt_cfg.llm.tools.push_back(std::move(time_tool));

    services::ToolDefinition sys_tool;
    sys_tool.name = "get_system_info";
    sys_tool.description = "Get system information (OS, hostname)";
    sys_tool.required_capability = "builtin";
    rt_cfg.llm.tools.push_back(std::move(sys_tool));

    services::ToolDefinition calc_tool;
    calc_tool.name = "calculate";
    calc_tool.description = "Evaluate a simple math expression (a op b)";
    calc_tool.parameters = {
        {"expression", "string", "Math expression like '2 + 3' or '10 / 4'", true}};
    calc_tool.required_capability = "builtin";
    rt_cfg.llm.tools.push_back(std::move(calc_tool));

    services::ToolDefinition reminder_tool;
    reminder_tool.name = "set_reminder";
    reminder_tool.description = "Set a reminder with a message and delay in minutes";
    reminder_tool.parameters = {
        {"message", "string", "Reminder message", true},
        {"minutes", "integer", "Minutes from now", true}};
    reminder_tool.required_capability = "builtin";
    rt_cfg.llm.tools.push_back(std::move(reminder_tool));
  }

  // ── Policy: allow all builtin tools ───────────────────────────────────────
  rt_cfg.policy.default_capabilities = {"builtin"};
  {
    core::PolicyRule allow_builtin;
    allow_builtin.priority = 0;
    allow_builtin.action_pattern = "*";
    allow_builtin.required_capability = "builtin";
    allow_builtin.outcome = core::PolicyOutcome::kAllow;
    rt_cfg.policy.initial_rules = {allow_builtin};
  }

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

  // ── Output callback: display only ──────────────────────────────────────────
  // TTS is now driven by the route core:tts_out → elevenlabs_tts:text_in.
  // This callback only handles console display of streaming tokens and final text.
  runtime.OnOutput([](const runtime::RuntimeOutput& output) {
    if (output.is_partial) {
      std::printf("%s", output.text.c_str());
      std::fflush(stdout);
      return;
    }
    std::printf("\n[agent] %s\n", output.text.c_str());
    std::fflush(stdout);
  });

  // ── Start ─────────────────────────────────────────────────────────────────
  runtime.StartSession();

  // Start audio capture explicitly (not auto-started by StartSession).
  capture_ptr->Start();

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
