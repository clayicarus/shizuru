// shizuru_bridge.cpp — C shared library wrapping AgentRuntime for Dart FFI.
//
// Device topology mirrors examples/voice_agent.cpp exactly.

#include "shizuru_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

// Runtime
#include "runtime/agent_runtime.h"
#include "runtime/route_table.h"
#include "io/tool_registry.h"

// IO devices
#include "io/audio/audio_capture_device.h"
#include "io/audio/audio_playout_device.h"
#include "io/asr/baidu/baidu_asr_device.h"
#include "io/tts/elevenlabs/elevenlabs_tts_device.h"
#include "io/vad/energy_vad_device.h"
#include "io/vad/vad_event_device.h"
#include "io/probe/pcm_dump_device.h"
#include "io/io_device.h"
#include "io/data_frame.h"

// Audio backends
#ifdef __ANDROID__
#include "io/audio/audio_device/oboe/oboe_recorder.h"
#include "io/audio/audio_device/oboe/oboe_player.h"
#else
#include "io/audio/audio_device/port_audio/pa_recorder.h"
#include "io/audio/audio_device/port_audio/pa_player.h"
#endif

// Service configs
#include "services/llm/config.h"
#include "services/tts/config.h"
#include "services/utils/baidu/baidu_config.h"
#include "services/utils/baidu/baidu_token_manager.h"

// Core types
#include "core/controller/types.h"
#include "core/policy/types.h"
#include "core/strategies/llm_observation_filter.h"
#include "core/strategies/llm_observation_aggregator.h"
#include "core/strategies/tts_segment_strategy.h"
#include "core/strategies/response_filter.h"
#include "services/llm/openai/openai_client.h"
#include "async_logger.h"

using namespace shizuru;

// ---------------------------------------------------------------------------
// AudioLevelProbe — inline IoDevice that computes RMS and fires a callback
// ---------------------------------------------------------------------------

namespace {

class AudioLevelProbe : public io::IoDevice {
 public:
  explicit AudioLevelProbe(std::string device_id = "audio_level_probe")
      : device_id_(std::move(device_id)) {}

  void SetLevelCallback(ShizuruAudioLevelCallback cb, void* user_data) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    cb_ = cb;
    user_data_ = user_data;
  }

  std::string GetDeviceId() const override { return device_id_; }

  std::vector<io::PortDescriptor> GetPortDescriptors() const override {
    return {
        {kAudioIn, io::PortDirection::kInput, "audio/pcm"},
    };
  }

  void OnInput(const std::string& /*port_name*/, io::DataFrame frame) override {
    if (frame.payload.empty()) return;

    // Compute RMS from s16le payload.
    const auto* samples =
        reinterpret_cast<const int16_t*>(frame.payload.data());
    const size_t n = frame.payload.size() / sizeof(int16_t);
    if (n == 0) return;

    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
      double s = static_cast<double>(samples[i]);
      sum += s * s;
    }
    float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(n)));

    ShizuruAudioLevelCallback cb = nullptr;
    void* ud = nullptr;
    {
      std::lock_guard<std::mutex> lock(cb_mutex_);
      cb = cb_;
      ud = user_data_;
    }
    if (cb) { cb(rms, ud); }
  }

  void SetOutputCallback(io::OutputCallback /*cb*/) override {}
  void Start() override {}
  void Stop() override {}

  static constexpr char kAudioIn[] = "audio_in";

 private:
  std::string device_id_;
  std::mutex cb_mutex_;
  ShizuruAudioLevelCallback cb_ = nullptr;
  void* user_data_ = nullptr;
};

// TranscriptProbe — taps ASR text_out to fire a callback with the transcript.
class TranscriptProbe : public io::IoDevice {
 public:
  explicit TranscriptProbe(std::string device_id = "transcript_probe")
      : device_id_(std::move(device_id)) {}

  void SetTranscriptCallback(ShizuruTranscriptCallback cb, void* user_data) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    cb_ = cb;
    user_data_ = user_data;
  }

  std::string GetDeviceId() const override { return device_id_; }

  std::vector<io::PortDescriptor> GetPortDescriptors() const override {
    return {{kTextIn, io::PortDirection::kInput, "text/plain"}};
  }

  void OnInput(const std::string& /*port_name*/, io::DataFrame frame) override {
    if (frame.payload.empty()) return;

    ShizuruTranscriptCallback cb = nullptr;
    void* ud = nullptr;
    {
      std::lock_guard<std::mutex> lock(cb_mutex_);
      cb = cb_;
      ud = user_data_;
    }
    if (cb) {
      std::string text(frame.payload.begin(), frame.payload.end());
      auto* heap = static_cast<char*>(std::malloc(text.size() + 1));
      std::memcpy(heap, text.c_str(), text.size() + 1);
      cb(heap, ud);
    }
  }

  void SetOutputCallback(io::OutputCallback /*cb*/) override {}
  void Start() override {}
  void Stop() override {}

  static constexpr char kTextIn[] = "text_in";

 private:
  std::string device_id_;
  std::mutex cb_mutex_;
  ShizuruTranscriptCallback cb_ = nullptr;
  void* user_data_ = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// ShizuruContext
// ---------------------------------------------------------------------------

struct ShizuruContext {
  std::unique_ptr<services::ToolRegistry> tools;
  std::unique_ptr<runtime::AgentRuntime> runtime;

  // Non-owning pointers into devices owned by runtime.
  io::AudioCaptureDevice* capture = nullptr;
  io::AudioPlayoutDevice* playout = nullptr;
  AudioLevelProbe* level_probe = nullptr;
  TranscriptProbe* transcript_probe = nullptr;

  std::atomic<bool> capture_running{false};
  std::atomic<bool> playout_running{false};

  // Callbacks
  ShizuruOutputCallback output_cb = nullptr;
  void* output_user_data = nullptr;
  ShizuruStateCallback state_cb = nullptr;
  void* state_user_data = nullptr;
  ShizuruAudioLevelCallback audio_level_cb = nullptr;
  void* audio_level_user_data = nullptr;
  ShizuruTranscriptCallback transcript_cb = nullptr;
  void* transcript_user_data = nullptr;
  ShizuruDiagnosticCallback diagnostic_cb = nullptr;
  void* diagnostic_user_data = nullptr;
  ShizuruActivityCallback activity_cb = nullptr;
  void* activity_user_data = nullptr;

  std::mutex cb_mutex;

  // Accumulated partial text for the current streaming response.
  // Guarded by cb_mutex.
  std::string accumulated_text;

  // State polling thread
  std::thread state_poll_thread;
  std::atomic<bool> state_poll_stop{false};
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void WriteError(char* buf, int len, const char* msg) {
  if (buf && len > 0) {
    std::strncpy(buf, msg, static_cast<size_t>(len - 1));
    buf[len - 1] = '\0';
  }
}

// ---------------------------------------------------------------------------
// shizuru_create
// ---------------------------------------------------------------------------

ShizuruHandle shizuru_create(const char* config_json, char* error_buf,
                             int error_buf_len) {
  if (!config_json) {
    WriteError(error_buf, error_buf_len, "config_json is null");
    return nullptr;
  }

  // ── Parse config JSON ────────────────────────────────────────────────────
  nlohmann::json cfg;
  try {
    cfg = nlohmann::json::parse(config_json);
  } catch (const std::exception& e) {
    WriteError(error_buf, error_buf_len,
               (std::string("JSON parse error: ") + e.what()).c_str());
    return nullptr;
  }

  auto get_str = [&](const char* key, const char* def = "") -> std::string {
    if (cfg.contains(key) && cfg[key].is_string()) return cfg[key].get<std::string>();
    return def;
  };
  auto get_int = [&](const char* key, int def = 0) -> int {
    if (cfg.contains(key) && cfg[key].is_number_integer()) return cfg[key].get<int>();
    return def;
  };

  const std::string llm_base_url   = get_str("llm_base_url", "https://dashscope.aliyuncs.com");
  const std::string llm_api_path   = get_str("llm_api_path", "/compatible-mode/v1/chat/completions");
  const std::string llm_api_key    = get_str("llm_api_key");
  const std::string llm_model      = get_str("llm_model", "qwen3-coder-next");
  const std::string el_api_key     = get_str("elevenlabs_api_key");
  const std::string el_voice_id    = get_str("elevenlabs_voice_id");  // empty → ElevenLabsConfig default (Rachel)
  const std::string baidu_api_key  = get_str("baidu_api_key");
  const std::string baidu_sec_key  = get_str("baidu_secret_key");
  const std::string system_instr   = get_str("system_instruction",
      "You are a helpful voice assistant. Keep responses concise and natural "
      "for speech. Avoid markdown formatting.");
  const int max_turns = get_int("max_turns", 100);

  if (llm_api_key.empty()) {
    WriteError(error_buf, error_buf_len, "llm_api_key is required");
    return nullptr;
  }

  // ── Build configs ────────────────────────────────────────────────────────
  services::BaiduConfig baidu_cfg;
  baidu_cfg.api_key    = baidu_api_key;
  baidu_cfg.secret_key = baidu_sec_key;
  baidu_cfg.aue        = 5;      // PCM 16kHz
  baidu_cfg.per        = 0;      // female voice
  baidu_cfg.asr_format = "pcm";

  services::ElevenLabsConfig el_cfg;
  el_cfg.api_key       = el_api_key;
  el_cfg.output_format = services::TtsOutputFormat::kPcm16000;
  if (!el_voice_id.empty()) { el_cfg.voice_id = el_voice_id; }

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

  // ── Build RuntimeConfig ──────────────────────────────────────────────────
  runtime::RuntimeConfig rt_cfg;
#ifdef __ANDROID__
  rt_cfg.logger.log_file = "";  // Disable file logging on Android (read-only filesystem)
#endif
  rt_cfg.llm.base_url  = llm_base_url;
  rt_cfg.llm.api_path  = llm_api_path;
  rt_cfg.llm.api_key   = llm_api_key;
  rt_cfg.llm.model     = llm_model;
  rt_cfg.llm.connect_timeout = std::chrono::seconds(10);
  rt_cfg.llm.read_timeout    = std::chrono::seconds(60);
  rt_cfg.llm.enable_thinking = true;
  rt_cfg.context.default_system_instruction = system_instr;
  rt_cfg.controller.max_turns     = max_turns;
  rt_cfg.controller.use_streaming = true;

  // ── Strategy factories ───────────────────────────────────────────────────
  // Note: capture by value — these lambdas outlive shizuru_create().
  rt_cfg.observation_aggregator_factory = [=]() {
    services::OpenAiConfig agg_llm_cfg;
    agg_llm_cfg.base_url        = llm_base_url;
    agg_llm_cfg.api_path        = llm_api_path;
    agg_llm_cfg.api_key         = llm_api_key;
    agg_llm_cfg.model           = llm_model;
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

  rt_cfg.observation_filter_factory = [=]() {
    services::OpenAiConfig filter_llm_cfg;
    filter_llm_cfg.base_url        = llm_base_url;
    filter_llm_cfg.api_path        = llm_api_path;
    filter_llm_cfg.api_key         = llm_api_key;
    filter_llm_cfg.model           = llm_model;
    filter_llm_cfg.max_tokens      = 8;
    filter_llm_cfg.temperature     = 0.0;
    filter_llm_cfg.connect_timeout = std::chrono::seconds(5);
    filter_llm_cfg.read_timeout    = std::chrono::seconds(10);
    return std::make_unique<core::LlmObservationFilter>(
        std::make_unique<services::OpenAiClient>(filter_llm_cfg));
  };

  rt_cfg.tts_segment_factory = []() {
    core::PunctuationSegmentStrategy::Config seg_cfg;
    seg_cfg.min_chars = 15;
    seg_cfg.max_chars = 200;
    return std::make_unique<core::PunctuationSegmentStrategy>(seg_cfg);
  };

  rt_cfg.response_filter_factory = []() {
    return std::make_unique<core::StripThinkingFilter>();
  };

  // ── Builtin tools ────────────────────────────────────────────────────────
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

  // ── Policy: allow all builtin tools ──────────────────────────────────────
  rt_cfg.policy.default_capabilities = {"builtin"};
  {
    core::PolicyRule allow_builtin;
    allow_builtin.priority = 0;
    allow_builtin.action_pattern = "*";
    allow_builtin.required_capability = "builtin";
    allow_builtin.outcome = core::PolicyOutcome::kAllow;
    rt_cfg.policy.initial_rules = {allow_builtin};
  }

  // ── Allocate context ─────────────────────────────────────────────────────
  auto ctx = std::make_unique<ShizuruContext>();
  ctx->tools = std::make_unique<services::ToolRegistry>();

  // Register builtin tool functions.
  ctx->tools->Register("get_current_time",
                       [](const std::string& /*args*/) -> services::ToolResult {
                         auto now = std::chrono::system_clock::now();
                         auto t = std::chrono::system_clock::to_time_t(now);
                         char buf[64];
                         std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
                                       std::localtime(&t));
                         return {true, buf, ""};
                       });

  ctx->tools->Register("get_system_info",
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

  ctx->tools->Register("calculate",
                       [](const std::string& args) -> services::ToolResult {
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

  ctx->tools->Register("set_reminder",
                       [](const std::string& args) -> services::ToolResult {
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

  try {
    ctx->runtime = std::make_unique<runtime::AgentRuntime>(rt_cfg, *ctx->tools);
  } catch (const std::exception& e) {
    WriteError(error_buf, error_buf_len,
               (std::string("AgentRuntime init error: ") + e.what()).c_str());
    return nullptr;
  }

  try {
    // ── Build voice devices ────────────────────────────────────────────────
    // Note: skip GetToken() pre-warm here — it blocks the calling thread
    // (Dart main isolate). The token will be fetched lazily on first ASR
    // request.
    auto token_mgr = std::make_shared<services::BaiduTokenManager>(baidu_cfg);

#ifdef __ANDROID__
    auto capture_dev = std::make_unique<io::AudioCaptureDevice>(
        std::make_unique<io::OboeRecorder>(rec_cfg));
#else
    auto capture_dev = std::make_unique<io::AudioCaptureDevice>(
        std::make_unique<io::PaRecorder>(rec_cfg));
#endif
    auto capture_dump_dev = std::make_unique<io::PcmDumpDevice>("capture");
    auto vad_dev = [&] {
      io::EnergyVadConfig vad_cfg;
      vad_cfg.energy_threshold        = 400.0F;
      vad_cfg.speech_onset_frames     = 3;
      vad_cfg.silence_hangover_frames = 20;
      vad_cfg.pre_roll_frames         = 3;
      return std::make_unique<io::EnergyVadDevice>(vad_cfg);
    }();
    auto vad_dump_dev     = std::make_unique<io::PcmDumpDevice>("vad_dump");
    auto asr_flush_dev    = std::make_unique<io::VadEventDevice>();
    auto asr_dev          =
        std::make_unique<io::BaiduAsrDevice>(baidu_cfg, token_mgr);
    auto tts_dev          = std::make_unique<io::ElevenLabsTtsDevice>(el_cfg);
    auto playout_dump_dev = std::make_unique<io::PcmDumpDevice>("playout_dump");
#ifdef __ANDROID__
    auto playout_dev      = std::make_unique<io::AudioPlayoutDevice>(
        std::make_unique<io::OboePlayer>(play_cfg));
#else
    auto playout_dev      = std::make_unique<io::AudioPlayoutDevice>(
        std::make_unique<io::PaPlayer>(play_cfg));
#endif
    auto level_probe_dev      = std::make_unique<AudioLevelProbe>();
    auto transcript_probe_dev = std::make_unique<TranscriptProbe>();

    // Keep non-owning raw pointers before moving into runtime.
    ctx->capture          = capture_dev.get();
    ctx->playout          = playout_dev.get();
    ctx->level_probe      = level_probe_dev.get();
    ctx->transcript_probe = transcript_probe_dev.get();

    // ── Register devices ───────────────────────────────────────────────────
    ctx->runtime->RegisterDevice(std::move(capture_dev));
    ctx->runtime->RegisterDevice(std::move(capture_dump_dev));
    ctx->runtime->RegisterDevice(std::move(vad_dev));
    ctx->runtime->RegisterDevice(std::move(vad_dump_dev));
    ctx->runtime->RegisterDevice(std::move(asr_flush_dev));
    ctx->runtime->RegisterDevice(std::move(asr_dev));
    ctx->runtime->RegisterDevice(std::move(tts_dev));
    ctx->runtime->RegisterDevice(std::move(playout_dump_dev));
    ctx->runtime->RegisterDevice(std::move(playout_dev));
    ctx->runtime->RegisterDevice(std::move(level_probe_dev));
    ctx->runtime->RegisterDevice(std::move(transcript_probe_dev));

    // ── DMA routes (mirrors voice_agent.cpp) ───────────────────────────────
    constexpr runtime::RouteOptions kDma{.requires_control_plane = false};

    // capture → capture_dump → vad
    ctx->runtime->AddRoute({"audio_capture", "audio_out"},
                           {"capture",       io::PcmDumpDevice::kPassIn}, kDma);
    ctx->runtime->AddRoute({"capture",       io::PcmDumpDevice::kPassOut},
                           {"vad",           io::EnergyVadDevice::kAudioIn}, kDma);

    // vad audio_out → vad_dump → asr
    ctx->runtime->AddRoute({"vad",      io::EnergyVadDevice::kAudioOut},
                           {"vad_dump", io::PcmDumpDevice::kPassIn}, kDma);
    ctx->runtime->AddRoute({"vad_dump", io::PcmDumpDevice::kPassOut},
                           {"baidu_asr","audio_in"}, kDma);

    // vad vad_out → asr_flush
    ctx->runtime->AddRoute({"vad",       io::EnergyVadDevice::kVadOut},
                           {"vad_event", io::VadEventDevice::kVadIn}, kDma);

    // asr text_out → core text_in
    ctx->runtime->AddRoute({"baidu_asr", "text_out"},
                           {"core",      "text_in"}, kDma);

    // tts audio_out → playout_dump → playout
    ctx->runtime->AddRoute({"elevenlabs_tts", "audio_out"},
                           {"playout_dump",   io::PcmDumpDevice::kPassIn}, kDma);
    ctx->runtime->AddRoute({"playout_dump",   io::PcmDumpDevice::kPassOut},
                           {"audio_playout",  "audio_in"}, kDma);

    // capture audio_out → level probe (parallel tap for RMS)
    ctx->runtime->AddRoute({"audio_capture",    "audio_out"},
                           {"audio_level_probe", AudioLevelProbe::kAudioIn}, kDma);

    // asr text_out → transcript probe (parallel tap for Dart UI)
    ctx->runtime->AddRoute({"baidu_asr",        "text_out"},
                           {"transcript_probe",  TranscriptProbe::kTextIn}, kDma);

    // ── OnOutput callback: display only (TTS is route-driven) ────────────
    ShizuruContext* raw_ctx = ctx.get();

    ctx->runtime->OnOutput([raw_ctx](const runtime::RuntimeOutput& output) {
    // Accumulate partial tokens so Dart always receives valid UTF-8
    // (SSE chunk boundaries may split multi-byte characters).
    ShizuruOutputCallback cb = nullptr;
    void* ud = nullptr;
    char* heap_str = nullptr;
    int32_t is_partial_flag = 0;

    {
      std::lock_guard<std::mutex> lock(raw_ctx->cb_mutex);
      cb = raw_ctx->output_cb;
      ud = raw_ctx->output_user_data;

      if (cb) {
        if (output.is_partial) {
          raw_ctx->accumulated_text += output.text;
          const std::string& snap = raw_ctx->accumulated_text;
          heap_str = static_cast<char*>(std::malloc(snap.size() + 1));
          std::memcpy(heap_str, snap.c_str(), snap.size() + 1);
          is_partial_flag = 1;
        } else {
          raw_ctx->accumulated_text.clear();
          heap_str = static_cast<char*>(
              std::malloc(output.text.size() + 1));
          std::memcpy(heap_str, output.text.c_str(),
                      output.text.size() + 1);
          is_partial_flag = 0;
        }
      }
    }

    // Call cb outside the lock — NativeCallable.listener posts to Dart
    // event queue asynchronously; heap_str stays valid until Dart calls
    // shizuru_free_string after toDartString().
    if (cb && heap_str) {
      cb(heap_str, is_partial_flag, ud);
    }
    });

    // Wire diagnostic callback: forward Controller events to Dart.
    ctx->runtime->OnDiagnostic([raw_ctx](const std::string& message) {
      ShizuruDiagnosticCallback cb = nullptr;
      void* ud = nullptr;
      {
        std::lock_guard<std::mutex> lock(raw_ctx->cb_mutex);
        cb = raw_ctx->diagnostic_cb;
        ud = raw_ctx->diagnostic_user_data;
      }
      if (cb) {
        auto* heap = static_cast<char*>(std::malloc(message.size() + 1));
        std::memcpy(heap, message.c_str(), message.size() + 1);
        cb(heap, ud);
      }
    });

    // Wire structured activity callback: forward ActivityEvents to Dart.
    ctx->runtime->OnActivity(
        [raw_ctx](const core::ActivityEvent& event) {
          ShizuruActivityCallback cb = nullptr;
          void* ud = nullptr;
          {
            std::lock_guard<std::mutex> lock(raw_ctx->cb_mutex);
            cb = raw_ctx->activity_cb;
            ud = raw_ctx->activity_user_data;
          }
          if (cb) {
            auto* heap_detail = static_cast<char*>(
                std::malloc(event.detail.size() + 1));
            std::memcpy(heap_detail, event.detail.c_str(),
                        event.detail.size() + 1);
            cb(static_cast<int32_t>(event.kind), heap_detail, ud);
          }
        });
  } catch (const std::exception& e) {
    WriteError(error_buf, error_buf_len,
               (std::string("Bridge init error: ") + e.what()).c_str());
    return nullptr;
  }

  return ctx.release();
}

// ---------------------------------------------------------------------------
// shizuru_start
// ---------------------------------------------------------------------------

int32_t shizuru_start(ShizuruHandle handle) {
  if (!handle) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  // Run StartSession() + state polling on a background thread so the Dart
  // main isolate (UI thread) is never blocked.
  ctx->state_poll_stop.store(false);
  ctx->state_poll_thread = std::thread([ctx] {
    try {
      ctx->runtime->StartSession();
      // Disable voice input/output pathways by default.
      // Users enable them via shizuru_set_voice_input / shizuru_set_voice_output.
      ctx->runtime->SetRouteEnabled(
          {"audio_capture", "audio_out"}, {"capture", "pass_in"}, false);
      ctx->runtime->SetRouteEnabled(
          {"core", "tts_out"}, {"elevenlabs_tts", "text_in"}, false);
    } catch (const std::exception& e) {
      // Fire error state + diagnostic message so Dart knows what went wrong.
      std::lock_guard<std::mutex> lock(ctx->cb_mutex);
      if (ctx->diagnostic_cb) {
        std::string msg = std::string("StartSession error: ") + e.what();
        auto* heap = static_cast<char*>(std::malloc(msg.size() + 1));
        std::memcpy(heap, msg.c_str(), msg.size() + 1);
        ctx->diagnostic_cb(heap, ctx->diagnostic_user_data);
      }
      if (ctx->state_cb) {
        ctx->state_cb(static_cast<int32_t>(core::State::kError),
                      ctx->state_user_data);
      }
      return;
    }

    // State polling loop.
    core::State last_state = core::State::kTerminated;
    while (!ctx->state_poll_stop.load()) {
      core::State current = ctx->runtime->GetState();
      if (current != last_state) {
        last_state = current;
        ShizuruStateCallback cb = nullptr;
        void* ud = nullptr;
        {
          std::lock_guard<std::mutex> lock(ctx->cb_mutex);
          cb = ctx->state_cb;
          ud = ctx->state_user_data;
        }
        if (cb) { cb(static_cast<int32_t>(current), ud); }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  return 0;
}

// ---------------------------------------------------------------------------
// shizuru_destroy
// ---------------------------------------------------------------------------

void shizuru_destroy(ShizuruHandle handle) {
  if (!handle) return;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  // Stop polling thread first.
  ctx->state_poll_stop.store(true);
  if (ctx->state_poll_thread.joinable()) {
    ctx->state_poll_thread.join();
  }

  ctx->runtime->Shutdown();
  delete ctx;
}

// ---------------------------------------------------------------------------
// Messaging and state
// ---------------------------------------------------------------------------

int32_t shizuru_send_message(ShizuruHandle handle, const char* text) {
  if (!handle || !text) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);
  try {
    ctx->runtime->SendMessage(text);
  } catch (const std::exception&) {
    return -2;
  }
  return 0;
}

int32_t shizuru_get_state(ShizuruHandle handle) {
  if (!handle) return static_cast<int32_t>(core::State::kTerminated);
  auto* ctx = static_cast<ShizuruContext*>(handle);
  return static_cast<int32_t>(ctx->runtime->GetState());
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void shizuru_set_output_callback(ShizuruHandle handle,
                                 ShizuruOutputCallback cb, void* user_data) {
  if (!handle) return;
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->output_cb        = cb;
  ctx->output_user_data = user_data;
}

void shizuru_set_state_callback(ShizuruHandle handle, ShizuruStateCallback cb,
                                void* user_data) {
  if (!handle) return;
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->state_cb        = cb;
  ctx->state_user_data = user_data;
}

// ---------------------------------------------------------------------------
// Voice control
// ---------------------------------------------------------------------------

int32_t shizuru_start_capture(ShizuruHandle handle) {
  if (!handle) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  bool expected = false;
  if (!ctx->capture_running.compare_exchange_strong(expected, true)) {
    return 0;  // already running
  }

  try {
    ctx->capture->Start();
  } catch (const std::exception& e) {
    ctx->capture_running.store(false);
    std::lock_guard<std::mutex> lock(ctx->cb_mutex);
    if (ctx->diagnostic_cb) {
      std::string msg = std::string("Capture start error: ") + e.what();
      auto* heap = static_cast<char*>(std::malloc(msg.size() + 1));
      std::memcpy(heap, msg.c_str(), msg.size() + 1);
      ctx->diagnostic_cb(heap, ctx->diagnostic_user_data);
    }
    return -2;
  }
  return 0;
}

int32_t shizuru_stop_capture(ShizuruHandle handle) {
  if (!handle) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  bool expected = true;
  if (!ctx->capture_running.compare_exchange_strong(expected, false)) {
    return 0;  // already stopped
  }

  try {
    ctx->capture->Stop();
  } catch (const std::exception&) {
    ctx->capture_running.store(true);
    return -2;
  }
  return 0;
}

void shizuru_set_audio_level_callback(ShizuruHandle handle,
                                      ShizuruAudioLevelCallback cb,
                                      void* user_data) {
  if (!handle) return;
  auto* ctx = static_cast<ShizuruContext*>(handle);
  {
    std::lock_guard<std::mutex> lock(ctx->cb_mutex);
    ctx->audio_level_cb        = cb;
    ctx->audio_level_user_data = user_data;
  }
  if (ctx->level_probe) {
    ctx->level_probe->SetLevelCallback(cb, user_data);
  }
}

void shizuru_set_transcript_callback(ShizuruHandle handle,
                                     ShizuruTranscriptCallback cb,
                                     void* user_data) {
  if (!handle) return;
  auto* ctx = static_cast<ShizuruContext*>(handle);
  {
    std::lock_guard<std::mutex> lock(ctx->cb_mutex);
    ctx->transcript_cb        = cb;
    ctx->transcript_user_data = user_data;
  }
  if (ctx->transcript_probe) {
    ctx->transcript_probe->SetTranscriptCallback(cb, user_data);
  }
}

void shizuru_set_diagnostic_callback(ShizuruHandle handle,
                                     ShizuruDiagnosticCallback cb,
                                     void* user_data) {
  if (!handle) return;
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->diagnostic_cb        = cb;
  ctx->diagnostic_user_data = user_data;
}

void shizuru_set_activity_callback(ShizuruHandle handle,
                                   ShizuruActivityCallback cb,
                                   void* user_data) {
  if (!handle) return;
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->activity_cb        = cb;
  ctx->activity_user_data = user_data;
}

// ---------------------------------------------------------------------------
// Pathway control
// ---------------------------------------------------------------------------

int32_t shizuru_set_voice_input(ShizuruHandle handle, int32_t enable) {
  if (!handle) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  // Enable/disable the entry-point route of the voice input chain.
  ctx->runtime->SetRouteEnabled(
      {"audio_capture", "audio_out"}, {"capture", "pass_in"}, enable != 0);
  return 0;
}

int32_t shizuru_set_voice_output(ShizuruHandle handle, int32_t enable) {
  if (!handle) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  // Enable/disable the entry-point route of the voice output chain.
  ctx->runtime->SetRouteEnabled(
      {"core", "tts_out"}, {"elevenlabs_tts", "text_in"}, enable != 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Playout control
// ---------------------------------------------------------------------------

int32_t shizuru_start_playout(ShizuruHandle handle) {
  if (!handle) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  bool expected = false;
  if (!ctx->playout_running.compare_exchange_strong(expected, true)) {
    return 0;  // already running
  }

  try {
    ctx->playout->Start();
  } catch (const std::exception& e) {
    ctx->playout_running.store(false);
    std::lock_guard<std::mutex> lock(ctx->cb_mutex);
    if (ctx->diagnostic_cb) {
      std::string msg = std::string("Playout start error: ") + e.what();
      auto* heap = static_cast<char*>(std::malloc(msg.size() + 1));
      std::memcpy(heap, msg.c_str(), msg.size() + 1);
      ctx->diagnostic_cb(heap, ctx->diagnostic_user_data);
    }
    return -2;
  }
  return 0;
}

int32_t shizuru_stop_playout(ShizuruHandle handle) {
  if (!handle) return -1;
  auto* ctx = static_cast<ShizuruContext*>(handle);

  bool expected = true;
  if (!ctx->playout_running.compare_exchange_strong(expected, false)) {
    return 0;  // already stopped
  }

  try {
    ctx->playout->Stop();
  } catch (const std::exception& e) {
    ctx->playout_running.store(true);
    return -2;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// String memory management
// ---------------------------------------------------------------------------

void shizuru_free_string(char* str) {
  std::free(str);
}
