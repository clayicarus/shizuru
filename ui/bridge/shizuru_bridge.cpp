// shizuru_bridge.cpp — C shared library wrapping AppRuntime for Dart FFI.
//
// This is a thin C API layer.  All product logic lives in app/assembly/AppRuntime.
// The bridge only handles:
//   - JSON config parsing → AppConfig
//   - Platform-specific audio device creation (Oboe vs PortAudio)
//   - C callback wiring (heap-allocating strings for Dart NativeCallable)
//   - State polling thread
//   - Voice pathway enable/disable

#include "shizuru_bridge.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#include <vector>

#include <nlohmann/json.hpp>

// App layer
#include "app/assembly/app_runtime.h"
#include "app/assembly/app_config.h"

// IO devices (platform-specific audio + probes)
#include "io/audio/audio_capture_device.h"
#include "io/audio/audio_playout_device.h"
#include "io/asr/baidu/baidu_asr_device.h"
#include "io/tts/elevenlabs/elevenlabs_tts_device.h"
#include "io/vad/energy_vad_device.h"
#include "io/probe/pcm_dump_device.h"
#include "io/io_device.h"

// Audio backends
#ifdef __ANDROID__
#include "io/audio/audio_device/oboe/oboe_recorder.h"
#include "io/audio/audio_device/oboe/oboe_player.h"
#else
#include "io/audio/audio_device/port_audio/pa_recorder.h"
#include "io/audio/audio_device/port_audio/pa_player.h"
#endif

// Service configs (for Baidu/ElevenLabs device construction)
#include "services/tts/config.h"
#include "services/utils/baidu/baidu_config.h"
#include "services/utils/baidu/baidu_token_manager.h"

// Core types
#include "core/content_part.h"
#include "core/conversation_item.h"
#include "core/controller/types.h"
#include "core/policy/types.h"
#include "runtime/route_table.h"

using namespace shizuru;

// ---------------------------------------------------------------------------
// AudioLevelProbe — computes RMS and fires a C callback
// ---------------------------------------------------------------------------

namespace {

std::string ItemKindToString(core::ConversationItemKind kind) {
  switch (kind) {
    case core::ConversationItemKind::kUserMessage:
      return "human_message";
    case core::ConversationItemKind::kAssistantMessage:
      return "assistant_message";
    case core::ConversationItemKind::kSystemEvent:
      return "system_event";
    case core::ConversationItemKind::kToolCall:
      return "tool_call";
    case core::ConversationItemKind::kToolResult:
      return "tool_result";
  }
  return "unknown";
}

std::string ActorKindToString(core::ActorKind kind) {
  switch (kind) {
    case core::ActorKind::kHuman:
      return "human";
    case core::ActorKind::kAssistant:
      return "assistant";
    case core::ActorKind::kSystem:
      return "system";
    case core::ActorKind::kTool:
      return "tool";
  }
  return "unknown";
}

std::string SerializeConversationItemForUi(const core::ConversationItem& item) {
  using json = nlohmann::json;

  json j;
  j["item_id"] = item.item_id;
  j["conversation_id"] = item.conversation_id;
  j["kind"] = ItemKindToString(item.kind);
  j["actor"] = {
      {"actor_id", item.actor.actor_id},
      {"display_name", item.actor.display_name},
      {"kind", ActorKindToString(item.actor.kind)},
  };
  j["timestamp_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
      item.wall_time.time_since_epoch()).count();
  if (item.reply_to_item_id.has_value()) {
    j["reply_to_item_id"] = *item.reply_to_item_id;
  }

  json parts = json::array();
  for (const auto& part : item.parts) {
    json pj;
    if (const auto* tp = std::get_if<core::TextPart>(&part)) {
      pj["type"] = "text";
      pj["text"] = tp->text;
    } else if (const auto* ip = std::get_if<core::ImagePart>(&part)) {
      pj["type"] = "image";
      pj["url"] = ip->url;
    } else if (const auto* ap = std::get_if<core::AudioPart>(&part)) {
      pj["type"] = "audio";
      pj["format"] = ap->format;
    } else if (const auto* tcp = std::get_if<core::ToolCallPart>(&part)) {
      pj["type"] = "tool_call";
      pj["tool_call_id"] = tcp->tool_call_id;
      pj["name"] = tcp->name;
      auto args = json::parse(tcp->arguments_json, nullptr, false);
      pj["arguments"] = args.is_discarded() ? json(tcp->arguments_json) : args;
    } else if (const auto* trp = std::get_if<core::ToolResultPart>(&part)) {
      pj["type"] = "tool_result";
      pj["tool_call_id"] = trp->tool_call_id;
      pj["tool_name"] = trp->tool_name;
      pj["success"] = trp->success;
      auto result = json::parse(trp->result_json, nullptr, false);
      pj["result"] = result.is_discarded() ? json(trp->result_json) : result;
    }
    parts.push_back(std::move(pj));
  }
  j["parts"] = parts;

  // Backward-compatible payload for current Flutter provider.
  json payload = json::object();
  payload["timestamp_ms"] = j["timestamp_ms"];
  switch (item.kind) {
    case core::ConversationItemKind::kUserMessage:
    case core::ConversationItemKind::kAssistantMessage:
    case core::ConversationItemKind::kSystemEvent: {
      std::string text;
      for (const auto& part : item.parts) {
        if (const auto* tp = std::get_if<core::TextPart>(&part)) {
          text += tp->text;
        }
      }
      payload["text"] = text;
      break;
    }
    case core::ConversationItemKind::kToolCall: {
      json tool_calls = json::array();
      for (const auto& part : item.parts) {
        if (const auto* tcp = std::get_if<core::ToolCallPart>(&part)) {
          auto args = json::parse(tcp->arguments_json, nullptr, false);
          tool_calls.push_back({
              {"id", tcp->tool_call_id},
              {"type", "function"},
              {"function", {
                  {"name", tcp->name},
                  {"arguments", args.is_discarded() ? json(tcp->arguments_json) : args},
              }},
          });
        }
      }
      payload["tool_calls"] = std::move(tool_calls);
      break;
    }
    case core::ConversationItemKind::kToolResult: {
      if (!item.parts.empty()) {
        if (const auto* trp = std::get_if<core::ToolResultPart>(&item.parts.front())) {
          payload["tool_name"] = trp->tool_name;
          payload["tool_call_id"] = trp->tool_call_id;
          auto result = json::parse(trp->result_json, nullptr, false);
          if (result.is_discarded()) {
            payload["content"] = {
                {"success", trp->success},
                {"output", trp->result_json},
            };
          } else {
            payload["content"] = result;
          }
        }
      }
      break;
    }
  }
  j["payload"] = std::move(payload);

  return j.dump();
}

std::optional<core::ActorKind> ParseActorKind(const std::string& kind) {
  if (kind == "human") { return core::ActorKind::kHuman; }
  if (kind == "assistant") { return core::ActorKind::kAssistant; }
  if (kind == "system") { return core::ActorKind::kSystem; }
  if (kind == "tool") { return core::ActorKind::kTool; }
  return std::nullopt;
}

std::optional<core::ConversationItemKind> ParseItemKind(const std::string& kind) {
  if (kind == "human_message") { return core::ConversationItemKind::kUserMessage; }
  if (kind == "assistant_message") {
    return core::ConversationItemKind::kAssistantMessage;
  }
  if (kind == "system_event") { return core::ConversationItemKind::kSystemEvent; }
  if (kind == "tool_call") { return core::ConversationItemKind::kToolCall; }
  if (kind == "tool_result") { return core::ConversationItemKind::kToolResult; }
  return std::nullopt;
}

std::optional<core::ConversationItem> ParseConversationItemFromUiJson(
    const std::string& item_json) {
  using json = nlohmann::json;

  auto parsed = json::parse(item_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::nullopt;
  }

  core::ConversationItem item;
  item.item_id = parsed.value(
      "item_id",
      "ui:" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  item.conversation_id = parsed.value("conversation_id", "");

  const auto item_kind = ParseItemKind(parsed.value("kind", "human_message"));
  if (!item_kind.has_value()) { return std::nullopt; }
  item.kind = *item_kind;

  const auto actor_json =
      parsed.value("actor", json::object());
  const auto actor_kind =
      ParseActorKind(actor_json.value("kind", "human"));
  if (!actor_kind.has_value()) { return std::nullopt; }
  item.actor = core::ActorRef{
      actor_json.value("actor_id", "local-user"),
      actor_json.value("display_name", "User"),
      *actor_kind,
  };

  if (parsed.contains("reply_to_item_id") &&
      parsed["reply_to_item_id"].is_string()) {
    item.reply_to_item_id = parsed["reply_to_item_id"].get<std::string>();
  }

  const auto parts = parsed.value("parts", json::array());
  if (!parts.is_array()) { return std::nullopt; }
  for (const auto& part : parts) {
    const auto type = part.value("type", "");
    if (type == "text") {
      item.parts.emplace_back(core::TextPart{part.value("text", "")});
      continue;
    }
    if (type == "image") {
      item.parts.emplace_back(core::ImagePart{part.value("url", "")});
      continue;
    }
    if (type == "audio") {
      core::AudioPart audio_part;
      audio_part.format = part.value("format", "");
      item.parts.emplace_back(std::move(audio_part));
      continue;
    }
    if (type == "tool_call") {
      item.parts.emplace_back(core::ToolCallPart{
          part.value("tool_call_id", ""),
          part.value("name", ""),
          part.contains("arguments") ? part["arguments"].dump() : std::string("{}"),
      });
      continue;
    }
    if (type == "tool_result") {
      item.parts.emplace_back(core::ToolResultPart{
          part.value("tool_call_id", ""),
          part.value("tool_name", ""),
          part.value("success", true),
          part.contains("result") ? part["result"].dump() : std::string("{}"),
      });
      continue;
    }
    return std::nullopt;
  }

  if (item.parts.empty()) { return std::nullopt; }

  if (parsed.contains("timestamp_ms") && parsed["timestamp_ms"].is_number_integer()) {
    item.wall_time = std::chrono::system_clock::time_point{
        std::chrono::milliseconds(parsed["timestamp_ms"].get<int64_t>())};
  } else {
    item.wall_time = std::chrono::system_clock::now();
  }

  return item;
}

class AudioLevelProbe : public io::IoDevice {
 public:
  explicit AudioLevelProbe(std::string device_id = "audio_level_probe")
      : device_id_(std::move(device_id)) {}

  void SetLevelCallback(ShizuruAudioLevelCallback cb, void* ud) {
    std::lock_guard<std::mutex> lock(mu_);
    cb_ = cb; ud_ = ud;
  }

  std::string GetDeviceId() const override { return device_id_; }
  std::vector<io::PortDescriptor> GetPortDescriptors() const override {
    return {{"audio_in", io::PortDirection::kInput, "audio/pcm",
             runtime::PortPayloadKind::kAudioFrame}};
  }
  void OnAudioFrame(const std::string&, io::AudioFrame frame) override {
    const auto* s = frame.data;
    const size_t n = frame.NumSamples();
    if (n == 0) { return; }
    double sum = 0;
    for (size_t i = 0; i < n; ++i) { double v = s[i]; sum += v * v; }
    float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
    ShizuruAudioLevelCallback cb; void* ud;
    { std::lock_guard<std::mutex> lock(mu_); cb = cb_; ud = ud_; }
    if (cb) { cb(rms, ud); }
  }
  void Start() override {}
  void Stop() override {}

 private:
  std::string device_id_;
  std::mutex mu_;
  ShizuruAudioLevelCallback cb_ = nullptr;
  void* ud_ = nullptr;
};

class TranscriptProbe : public io::IoDevice {
 public:
  explicit TranscriptProbe(std::string device_id = "transcript_probe")
      : device_id_(std::move(device_id)) {}

  void SetTranscriptCallback(ShizuruTranscriptCallback cb, void* ud) {
    std::lock_guard<std::mutex> lock(mu_);
    cb_ = cb; ud_ = ud;
  }

  std::string GetDeviceId() const override { return device_id_; }
  std::vector<io::PortDescriptor> GetPortDescriptors() const override {
    return {{"item_in", io::PortDirection::kInput, "",
             runtime::PortPayloadKind::kConversationItem}};
  }
  void OnConversationItem(const std::string&, core::ConversationItem item) override {
    ShizuruTranscriptCallback cb; void* ud;
    { std::lock_guard<std::mutex> lock(mu_); cb = cb_; ud = ud_; }
    if (cb) {
      std::string text;
      for (const auto& part : item.parts) {
        if (const auto* tp = std::get_if<core::TextPart>(&part)) {
          text += tp->text;
        }
      }
      if (text.empty()) { return; }
      auto* heap = static_cast<char*>(std::malloc(text.size() + 1));
      std::memcpy(heap, text.c_str(), text.size() + 1);
      cb(heap, ud);
    }
  }
  void Start() override {}
  void Stop() override {}

 private:
  std::string device_id_;
  std::mutex mu_;
  ShizuruTranscriptCallback cb_ = nullptr;
  void* ud_ = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// ShizuruContext
// ---------------------------------------------------------------------------

struct ShizuruContext {
  std::unique_ptr<app::AppRuntime> app;

  // Non-owning pointers into devices owned by app->Bus().
  io::AudioCaptureDevice* capture = nullptr;
  io::AudioPlayoutDevice* playout = nullptr;
  AudioLevelProbe* level_probe = nullptr;
  TranscriptProbe* transcript_probe = nullptr;

  std::atomic<bool> capture_running{false};
  std::atomic<bool> playout_running{false};

  // C callbacks (guarded by cb_mutex).
  ShizuruOutputCallback output_cb = nullptr;
  void* output_ud = nullptr;
  ShizuruStateCallback state_cb = nullptr;
  void* state_ud = nullptr;
  ShizuruDiagnosticCallback diagnostic_cb = nullptr;
  void* diagnostic_ud = nullptr;
  ShizuruActivityCallback activity_cb = nullptr;
  void* activity_ud = nullptr;
  std::mutex cb_mutex;

  // State polling thread.
  std::thread state_poll_thread;
  std::atomic<bool> state_poll_stop{false};
};

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

  nlohmann::json cfg;
  try {
    cfg = nlohmann::json::parse(config_json);
  } catch (const std::exception& e) {
    WriteError(error_buf, error_buf_len,
               (std::string("JSON parse error: ") + e.what()).c_str());
    return nullptr;
  }

  auto get_str = [&](const char* key, const char* def = "") -> std::string {
    if (cfg.contains(key) && cfg[key].is_string()) { return cfg[key].get<std::string>(); }
    return def;
  };
  auto get_int = [&](const char* key, int def = 0) -> int {
    if (cfg.contains(key) && cfg[key].is_number_integer()) { return cfg[key].get<int>(); }
    return def;
  };

  const std::string llm_base_url  = get_str("llm_base_url", "https://dashscope.aliyuncs.com");
  const std::string llm_api_path  = get_str("llm_api_path", "/compatible-mode/v1/chat/completions");
  const std::string llm_api_key   = get_str("llm_api_key");
  const std::string llm_model     = get_str("llm_model", "qwen3-coder-next");
  const std::string el_api_key    = get_str("elevenlabs_api_key");
  const std::string el_voice_id   = get_str("elevenlabs_voice_id");
  const std::string baidu_api_key = get_str("baidu_api_key");
  const std::string baidu_sec_key = get_str("baidu_secret_key");
  const std::string user_id       = get_str("user_id", "local-user");
  const std::string db_path       = get_str("db_path");
  const std::string user_instr    = get_str("system_instruction");

  if (llm_api_key.empty()) {
    WriteError(error_buf, error_buf_len, "llm_api_key is required");
    return nullptr;
  }

  // ── Build AppConfig ──────────────────────────────────────────────────────
  app::AppConfig app_cfg;
  app_cfg.llm.base_url        = llm_base_url;
  app_cfg.llm.api_path        = llm_api_path;
  app_cfg.llm.api_key         = llm_api_key;
  app_cfg.llm.model           = llm_model;
  app_cfg.llm.connect_timeout = std::chrono::seconds(10);
  app_cfg.llm.read_timeout    = std::chrono::seconds(60);
  app_cfg.llm.enable_thinking = true;
  app_cfg.controller.use_streaming = true;
  app_cfg.policy.default_capabilities = {"builtin"};
  app_cfg.user_id = user_id;
  app_cfg.db_path = db_path;
  {
    core::PolicyRule allow_builtin;
    allow_builtin.priority = 0;
    allow_builtin.action_pattern = "*";
    allow_builtin.required_capability = "builtin";
    allow_builtin.outcome = core::PolicyOutcome::kAllow;
    app_cfg.policy.initial_rules = {allow_builtin};
  }
#ifdef __ANDROID__
  app_cfg.logger.log_file = "";
#endif
  app_cfg.user_instruction = user_instr;

  // ── Build voice device configs ───────────────────────────────────────────
  services::BaiduConfig baidu_cfg;
  baidu_cfg.api_key    = baidu_api_key;
  baidu_cfg.secret_key = baidu_sec_key;
  baidu_cfg.aue        = 5;
  baidu_cfg.per        = 0;
  baidu_cfg.asr_format = "pcm";

  services::ElevenLabsConfig el_cfg;
  el_cfg.api_key       = el_api_key;
  el_cfg.output_format = services::TtsOutputFormat::kPcm16000;
  if (!el_voice_id.empty()) { el_cfg.voice_id = el_voice_id; }

  constexpr int kRate = 16000;
  constexpr size_t kCh = 1;
  constexpr size_t kFpb = 320;

  io::RecorderConfig rec_cfg;
  rec_cfg.sample_rate = kRate;
  rec_cfg.channel_count = kCh;
  rec_cfg.frames_per_buffer = kFpb;
  rec_cfg.buffer_capacity_samples = static_cast<size_t>(kRate) * 5;

  io::PlayerConfig play_cfg;
  play_cfg.sample_rate = kRate;
  play_cfg.channel_count = kCh;
  play_cfg.frames_per_buffer = kFpb;
  play_cfg.buffer_capacity_samples = static_cast<size_t>(kRate) * 10;

  // ── Create AppRuntime + register platform devices ────────────────────────
  auto ctx = std::make_unique<ShizuruContext>();

  try {
    ctx->app = std::make_unique<app::AppRuntime>(std::move(app_cfg));
  } catch (const std::exception& e) {
    WriteError(error_buf, error_buf_len,
               (std::string("AppRuntime init error: ") + e.what()).c_str());
    return nullptr;
  }

  try {
    auto& bus = ctx->app->Bus();
    auto token_mgr = std::make_shared<services::BaiduTokenManager>(baidu_cfg);

    // Platform-specific audio devices.
#ifdef __ANDROID__
    auto capture_dev = std::make_unique<io::AudioCaptureDevice>(
        std::make_unique<io::OboeRecorder>(rec_cfg));
    auto playout_dev = std::make_unique<io::AudioPlayoutDevice>(
        std::make_unique<io::OboePlayer>(play_cfg));
#else
    auto capture_dev = std::make_unique<io::AudioCaptureDevice>(
        std::make_unique<io::PaRecorder>(rec_cfg));
    auto playout_dev = std::make_unique<io::AudioPlayoutDevice>(
        std::make_unique<io::PaPlayer>(play_cfg));
#endif
    auto capture_dump = std::make_unique<io::PcmDumpDevice>("capture");
    auto vad_dev = [&] {
      io::EnergyVadConfig v;
      v.energy_threshold = 400.0F;
      v.speech_onset_frames = 3;
      v.silence_hangover_frames = 20;
      v.pre_roll_frames = 3;
      return std::make_unique<io::EnergyVadDevice>(v);
    }();
    auto vad_dump = std::make_unique<io::PcmDumpDevice>("vad_dump");
    auto asr_dev = std::make_unique<io::BaiduAsrDevice>(baidu_cfg, token_mgr);
    auto tts_dev = std::make_unique<io::ElevenLabsTtsDevice>(el_cfg);
    auto playout_dump = std::make_unique<io::PcmDumpDevice>("playout_dump");
    auto level_probe = std::make_unique<AudioLevelProbe>();
    auto transcript_probe = std::make_unique<TranscriptProbe>();

    ctx->capture = capture_dev.get();
    ctx->playout = playout_dev.get();
    ctx->level_probe = level_probe.get();
    ctx->transcript_probe = transcript_probe.get();

    constexpr runtime::DeviceOptions kManual{.auto_start = false};
    bus.RegisterDevice(std::move(capture_dev), kManual);
    bus.RegisterDevice(std::move(capture_dump));
    bus.RegisterDevice(std::move(vad_dev));
    bus.RegisterDevice(std::move(vad_dump));
    bus.RegisterDevice(std::move(asr_dev));
    bus.RegisterDevice(std::move(tts_dev));
    bus.RegisterDevice(std::move(playout_dump));
    bus.RegisterDevice(std::move(playout_dev), kManual);
    bus.RegisterDevice(std::move(level_probe));
    bus.RegisterDevice(std::move(transcript_probe));

    // Voice pipeline DMA routes.
    constexpr runtime::RouteOptions kDma{.requires_control_plane = false};
    constexpr runtime::RouteOptions kCtrl{.requires_control_plane = true};
    bus.AddRoute({"audio_capture", "audio_out"}, {"capture", "pass_in"}, kDma);
    bus.AddRoute({"capture", io::PcmDumpDevice::kPassOut}, {"vad", io::EnergyVadDevice::kAudioIn}, kDma);
    bus.AddRoute({"vad", io::EnergyVadDevice::kAudioOut}, {"vad_dump", io::PcmDumpDevice::kPassIn}, kDma);
    bus.AddRoute({"vad_dump", io::PcmDumpDevice::kPassOut}, {"baidu_asr", "audio_in"}, kDma);
    bus.AddRoute({"vad", io::EnergyVadDevice::kControlSignalOut}, {"baidu_asr", "signal_in"}, kCtrl);
    bus.AddRoute({"vad", io::EnergyVadDevice::kInterruptSignalOut}, {"core", "control_in"}, kCtrl);
    bus.AddRoute({"baidu_asr", "item_out"}, {"core", "item_in"}, kDma);
    bus.AddRoute({"elevenlabs_tts", "audio_out"}, {"playout_dump", io::PcmDumpDevice::kPassIn}, kDma);
    bus.AddRoute({"playout_dump", io::PcmDumpDevice::kPassOut}, {"audio_playout", "audio_in"}, kDma);
    bus.AddRoute({"audio_capture", "audio_out"}, {"audio_level_probe", "audio_in"}, kDma);
    bus.AddRoute({"baidu_asr", "item_out"}, {"transcript_probe", "item_in"}, kDma);

    // Disable voice input pathway by default (capture → VAD chain).
    // The route was just added above, so this takes effect immediately.
    bus.SetRouteEnabled({"audio_capture", "audio_out"}, {"capture", "pass_in"}, false);

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
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  ShizuruContext* raw = ctx;

  // Wire ConversationItem callback → C output callback.
  ctx->app->OnConversationItem(
      [raw](const core::ConversationItem& item, bool is_delta) {
    ShizuruOutputCallback cb; void* ud;
    {
      std::lock_guard<std::mutex> lock(raw->cb_mutex);
      cb = raw->output_cb; ud = raw->output_ud;
    }
    if (!cb) { return; }
    const std::string item_json = SerializeConversationItemForUi(item);
    auto* heap = static_cast<char*>(std::malloc(item_json.size() + 1));
    std::memcpy(heap, item_json.c_str(), item_json.size() + 1);
    cb(heap, is_delta ? 1 : 0, ud);
  });

  ctx->app->OnDiagnostic([raw](const std::string& msg) {
    ShizuruDiagnosticCallback cb; void* ud;
    { std::lock_guard<std::mutex> lock(raw->cb_mutex); cb = raw->diagnostic_cb; ud = raw->diagnostic_ud; }
    if (cb) {
      auto* heap = static_cast<char*>(std::malloc(msg.size() + 1));
      std::memcpy(heap, msg.c_str(), msg.size() + 1);
      cb(heap, ud);
    }
  });

  ctx->app->OnActivity([raw](const core::ActivityEvent& event) {
    ShizuruActivityCallback cb; void* ud;
    { std::lock_guard<std::mutex> lock(raw->cb_mutex); cb = raw->activity_cb; ud = raw->activity_ud; }
    if (cb) {
      auto* heap = static_cast<char*>(std::malloc(event.detail.size() + 1));
      std::memcpy(heap, event.detail.c_str(), event.detail.size() + 1);
      cb(static_cast<int32_t>(event.kind), heap, ud);
    }
  });

  // Start on background thread so Dart main isolate is not blocked.
  ctx->state_poll_stop.store(false);
  ctx->state_poll_thread = std::thread([raw] {
    try {
      raw->app->Start();
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(raw->cb_mutex);
      if (raw->diagnostic_cb) {
        std::string msg = std::string("Start error: ") + e.what();
        auto* heap = static_cast<char*>(std::malloc(msg.size() + 1));
        std::memcpy(heap, msg.c_str(), msg.size() + 1);
        raw->diagnostic_cb(heap, raw->diagnostic_ud);
      }
      if (raw->state_cb) {
        raw->state_cb(static_cast<int32_t>(core::State::kError), raw->state_ud);
      }
      return;
    }

    // Disable voice output pathway by default.  This must happen AFTER
    // app->Start() because WireRoutes() creates the core→tts route.
    // (The voice input pathway is disabled in shizuru_create where the
    // capture→vad route is added by the bridge itself.)
    raw->app->Bus().SetRouteEnabled(
        {"core", "item_out"}, {"elevenlabs_tts", "item_in"}, false);

    // State polling loop.
    core::State last = core::State::kTerminated;
    while (!raw->state_poll_stop.load()) {
      core::State cur = raw->app->GetState();
      if (cur != last) {
        last = cur;
        ShizuruStateCallback cb; void* ud;
        { std::lock_guard<std::mutex> lock(raw->cb_mutex); cb = raw->state_cb; ud = raw->state_ud; }
        if (cb) { cb(static_cast<int32_t>(cur), ud); }
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
  if (!handle) { return; }
  auto* ctx = static_cast<ShizuruContext*>(handle);

  ctx->state_poll_stop.store(true);
  if (ctx->state_poll_thread.joinable()) { ctx->state_poll_thread.join(); }

  // Stop audio devices FIRST — their callbacks run on PortAudio/Oboe threads
  // and may call into the bus.  If we destroy the bus while callbacks are
  // still firing, we get a use-after-free or deadlock.
  if (ctx->capture_running.load()) {
    try { ctx->capture->Stop(); } catch (...) {}
    ctx->capture_running.store(false);
  }
  if (ctx->playout_running.load()) {
    try { ctx->playout->Stop(); } catch (...) {}
    ctx->playout_running.store(false);
  }

  // Clear probe callbacks BEFORE Shutdown so no stale function pointers
  // can fire during bus teardown.
  if (ctx->level_probe) { ctx->level_probe->SetLevelCallback(nullptr, nullptr); }
  if (ctx->transcript_probe) { ctx->transcript_probe->SetTranscriptCallback(nullptr, nullptr); }
  {
    std::lock_guard<std::mutex> lock(ctx->cb_mutex);
    ctx->output_cb = nullptr;
    ctx->state_cb = nullptr;
    ctx->diagnostic_cb = nullptr;
    ctx->activity_cb = nullptr;
  }

  ctx->app->Shutdown();
  delete ctx;
}

// ---------------------------------------------------------------------------
// Messaging and state
// ---------------------------------------------------------------------------

int32_t shizuru_send_conversation_item_json(ShizuruHandle handle,
                                            const char* item_json) {
  if (!handle || !item_json) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  const auto item = ParseConversationItemFromUiJson(item_json);
  if (!item.has_value()) { return -2; }
  try {
    ctx->app->SendConversationItem(*item);
  } catch (...) {
    return -3;
  }
  return 0;
}

int32_t shizuru_get_state(ShizuruHandle handle) {
  if (!handle) { return static_cast<int32_t>(core::State::kTerminated); }
  return static_cast<int32_t>(static_cast<ShizuruContext*>(handle)->app->GetState());
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void shizuru_set_output_callback(ShizuruHandle handle, ShizuruOutputCallback cb, void* ud) {
  if (!handle) { return; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->output_cb = cb; ctx->output_ud = ud;
}

void shizuru_set_state_callback(ShizuruHandle handle, ShizuruStateCallback cb, void* ud) {
  if (!handle) { return; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->state_cb = cb; ctx->state_ud = ud;
}

// ---------------------------------------------------------------------------
// Voice control
// ---------------------------------------------------------------------------

int32_t shizuru_start_capture(ShizuruHandle handle) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  bool exp = false;
  if (!ctx->capture_running.compare_exchange_strong(exp, true)) { return 0; }
  try { ctx->capture->Start(); } catch (const std::exception& e) {
    ctx->capture_running.store(false);
    std::lock_guard<std::mutex> lock(ctx->cb_mutex);
    if (ctx->diagnostic_cb) {
      std::string msg = std::string("Capture error: ") + e.what();
      auto* heap = static_cast<char*>(std::malloc(msg.size() + 1));
      std::memcpy(heap, msg.c_str(), msg.size() + 1);
      ctx->diagnostic_cb(heap, ctx->diagnostic_ud);
    }
    return -2;
  }
  return 0;
}

int32_t shizuru_stop_capture(ShizuruHandle handle) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  bool exp = true;
  if (!ctx->capture_running.compare_exchange_strong(exp, false)) { return 0; }
  try { ctx->capture->Stop(); } catch (...) { ctx->capture_running.store(true); return -2; }
  return 0;
}

void shizuru_set_audio_level_callback(ShizuruHandle handle, ShizuruAudioLevelCallback cb, void* ud) {
  if (!handle) { return; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  if (ctx->level_probe) { ctx->level_probe->SetLevelCallback(cb, ud); }
}

void shizuru_set_transcript_callback(ShizuruHandle handle, ShizuruTranscriptCallback cb, void* ud) {
  if (!handle) { return; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  if (ctx->transcript_probe) { ctx->transcript_probe->SetTranscriptCallback(cb, ud); }
}

void shizuru_set_diagnostic_callback(ShizuruHandle handle, ShizuruDiagnosticCallback cb, void* ud) {
  if (!handle) { return; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->diagnostic_cb = cb; ctx->diagnostic_ud = ud;
}

void shizuru_set_activity_callback(ShizuruHandle handle, ShizuruActivityCallback cb, void* ud) {
  if (!handle) { return; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  std::lock_guard<std::mutex> lock(ctx->cb_mutex);
  ctx->activity_cb = cb; ctx->activity_ud = ud;
}

// ---------------------------------------------------------------------------
// Pathway control
// ---------------------------------------------------------------------------

int32_t shizuru_set_voice_input(ShizuruHandle handle, int32_t enable) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  ctx->app->Bus().SetRouteEnabled(
      {"audio_capture", "audio_out"}, {"capture", "pass_in"}, enable != 0);
  return 0;
}

int32_t shizuru_set_voice_output(ShizuruHandle handle, int32_t enable) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  ctx->app->Bus().SetRouteEnabled(
      {"core", "item_out"}, {"elevenlabs_tts", "item_in"}, enable != 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Playout control
// ---------------------------------------------------------------------------

int32_t shizuru_start_playout(ShizuruHandle handle) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  bool exp = false;
  if (!ctx->playout_running.compare_exchange_strong(exp, true)) { return 0; }
  try { ctx->playout->Start(); } catch (const std::exception& e) {
    ctx->playout_running.store(false);
    std::lock_guard<std::mutex> lock(ctx->cb_mutex);
    if (ctx->diagnostic_cb) {
      std::string msg = std::string("Playout error: ") + e.what();
      auto* heap = static_cast<char*>(std::malloc(msg.size() + 1));
      std::memcpy(heap, msg.c_str(), msg.size() + 1);
      ctx->diagnostic_cb(heap, ctx->diagnostic_ud);
    }
    return -2;
  }
  return 0;
}

int32_t shizuru_stop_playout(ShizuruHandle handle) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  bool exp = true;
  if (!ctx->playout_running.compare_exchange_strong(exp, false)) { return 0; }
  try { ctx->playout->Stop(); } catch (...) { ctx->playout_running.store(true); return -2; }
  return 0;
}

// ---------------------------------------------------------------------------
// Memory / context management
// ---------------------------------------------------------------------------

int32_t shizuru_clear_database(ShizuruHandle handle) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  try { ctx->app->ClearDatabase(); } catch (...) { return -2; }
  return 0;
}

int32_t shizuru_clear_context(ShizuruHandle handle) {
  if (!handle) { return -1; }
  auto* ctx = static_cast<ShizuruContext*>(handle);
  try { ctx->app->ClearContext(); } catch (...) { return -2; }
  return 0;
}

// ---------------------------------------------------------------------------
// String memory management
// ---------------------------------------------------------------------------

void shizuru_free_string(char* str) { std::free(str); }
