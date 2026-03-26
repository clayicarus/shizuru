// C API implementation — thin wrapper around runtime::AgentRuntime.

#include "shizuru_api.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "async_logger.h"
#include "io/tool_registry.h"
#include "llm/config.h"
#include "policy/types.h"
#include "runtime/agent_runtime.h"
#include "runtime/route_table.h"

// TTS pipeline device headers
#include "io/tts/baidu/baidu_tts_device.h"
#include "io/tts/elevenlabs/elevenlabs_tts_device.h"
#include "io/audio/audio_playout_device.h"
#include "io/audio/audio_device/port_audio/pa_player.h"
#include "services/tts/config.h"
#include "services/utils/baidu/baidu_config.h"

// ASR pipeline headers
#include "io/audio/audio_device/port_audio/pa_recorder.h"
#include "asr/baidu/baidu_asr_client.h"
#include "utils/baidu/baidu_token_manager.h"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal handle
// ---------------------------------------------------------------------------

// Stored tool handler info (for tools registered with a C handler function).
struct ToolHandlerEntry {
  std::string name;
  ShizuruToolHandler handler;
  void* user_data;
};

struct ShizuruRuntime {
  // Config accumulated before start_session.
  shizuru::runtime::RuntimeConfig config;
  shizuru::services::ToolRegistry tools;
  std::vector<ToolHandlerEntry> tool_handlers;

  // Created at start_session time.
  std::unique_ptr<shizuru::runtime::AgentRuntime> runtime;

  // Callback storage
  std::mutex cb_mutex;
  ShizuruOutputCallback output_cb = nullptr;
  void* output_ud = nullptr;
  ShizuruStateCallback state_cb = nullptr;
  void* state_ud = nullptr;
  ShizuruToolCallCallback tool_call_cb = nullptr;
  void* tool_call_ud = nullptr;

  // Voice config (stored until start_session)
  bool voice_configured = false;
  json voice_config;

  // Pending audio bytes (filled by DartAudioOutputDevice; Dart reads via shizuru_take_audio).
  std::mutex pending_audio_mutex;
  std::vector<uint8_t> pending_audio;

  // ASR recording state (click-to-talk; managed directly, not via IoDevice bus).
  std::unique_ptr<shizuru::io::PaRecorder> asr_recorder;
  std::unique_ptr<shizuru::services::BaiduAsrClient> asr_client;
  std::shared_ptr<shizuru::services::BaiduTokenManager> asr_token_mgr;
  std::mutex asr_audio_mutex;
  std::vector<uint8_t> asr_audio_buffer;
  std::atomic<bool> asr_recording{false};
  std::thread asr_thread;
  ShizuruTranscriptionCallback transcription_cb = nullptr;
  void* transcription_ud = nullptr;
};

// ---------------------------------------------------------------------------
// DartAudioOutputDevice — stores synthesized audio bytes in ShizuruRuntime.
// Dart retrieves them via shizuru_take_audio() (synchronous FFI call).
// ---------------------------------------------------------------------------

class DartAudioOutputDevice : public shizuru::io::IoDevice {
 public:
  explicit DartAudioOutputDevice(ShizuruRuntime* rt) : rt_(rt) {}

  std::string GetDeviceId() const override { return "dart_audio_out"; }

  std::vector<shizuru::io::PortDescriptor> GetPortDescriptors() const override {
    return {{"audio_in", shizuru::io::PortDirection::kInput, "audio/pcm"}};
  }

  void OnInput(const std::string& port,
               shizuru::io::DataFrame frame) override {
    if (port != "audio_in" || frame.payload.empty()) return;
    std::lock_guard<std::mutex> lock(rt_->pending_audio_mutex);
    rt_->pending_audio = std::move(frame.payload);
  }

  void SetOutputCallback(shizuru::io::OutputCallback) override {}  // no outputs
  void Start() override {}
  void Stop()  override {}

 private:
  ShizuruRuntime* rt_;
};

// Portable strdup (MSVC names it _strdup).
static char* api_strdup(const char* s) {
  if (!s) return nullptr;
  const size_t len = std::strlen(s) + 1;
  char* copy = static_cast<char*>(std::malloc(len));
  if (copy) std::memcpy(copy, s, len);
  return copy;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ShizuruRuntime* shizuru_create(const char* config_json) {
  if (!config_json) return nullptr;

  try {
    auto j = json::parse(config_json);

    auto handle = std::make_unique<ShizuruRuntime>();
    auto& cfg = handle->config;

    // LLM config
    if (j.contains("llm_base_url"))  cfg.llm.base_url  = j["llm_base_url"].get<std::string>();
    if (j.contains("llm_api_path"))  cfg.llm.api_path  = j["llm_api_path"].get<std::string>();
    if (j.contains("llm_api_key"))   cfg.llm.api_key   = j["llm_api_key"].get<std::string>();
    if (j.contains("llm_model"))     cfg.llm.model     = j["llm_model"].get<std::string>();

    // Context config
    if (j.contains("system_prompt")) {
      cfg.context.default_system_instruction = j["system_prompt"].get<std::string>();
    }

    // Controller config (optional overrides)
    if (j.contains("max_turns"))     cfg.controller.max_turns    = j["max_turns"].get<int>();
    if (j.contains("token_budget"))  cfg.controller.token_budget = j["token_budget"].get<int>();

    // Logger
    cfg.logger.level = spdlog::level::info;
    if (j.contains("log_level")) {
      const auto& lvl = j["log_level"].get<std::string>();
      if (lvl == "debug") cfg.logger.level = spdlog::level::debug;
      else if (lvl == "warn")  cfg.logger.level = spdlog::level::warn;
      else if (lvl == "error") cfg.logger.level = spdlog::level::err;
    }

    // Runtime is NOT created here — deferred to shizuru_start_session
    // so that tools and voice can be registered first.
    return handle.release();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[shizuru_api] shizuru_create failed: %s\n", e.what());
    return nullptr;
  }
}

void shizuru_destroy(ShizuruRuntime* rt) {
  if (!rt) return;
  // Stop ASR recording if active and join transcription thread.
  if (rt->asr_recording.load() && rt->asr_recorder) {
    rt->asr_recording.store(false);
    rt->asr_recorder->Stop();
  }
  if (rt->asr_thread.joinable()) {
    rt->asr_thread.join();
  }
  if (rt->runtime) {
    rt->runtime->Shutdown();
  }
  delete rt;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

const char* shizuru_start_session(ShizuruRuntime* rt) {
  if (!rt) return nullptr;

  try {
    // Shutdown previous session if any.
    if (rt->runtime) {
      rt->runtime->Shutdown();
      rt->runtime.reset();
    }

    // Create runtime with the accumulated config.
    rt->runtime = std::make_unique<shizuru::runtime::AgentRuntime>(
        rt->config, rt->tools);

    // Wire callbacks before starting the session.
    {
      std::lock_guard<std::mutex> lock(rt->cb_mutex);
      if (rt->output_cb) {
        ShizuruOutputCallback cb = rt->output_cb;
        void* ud = rt->output_ud;
        rt->runtime->OnOutput(
            [cb, ud](const shizuru::runtime::RuntimeOutput& output) {
              char* text_copy = api_strdup(output.text.c_str());
              cb(text_copy, ud);
            });
      }

      if (rt->state_cb) {
        ShizuruStateCallback cb = rt->state_cb;
        void* ud = rt->state_ud;
        rt->runtime->OnStateChange(
            [cb, ud](shizuru::core::State state) {
              cb(static_cast<int32_t>(state), ud);
            });
      }

      if (rt->tool_call_cb) {
        ShizuruToolCallCallback cb = rt->tool_call_cb;
        void* ud = rt->tool_call_ud;
        rt->runtime->OnToolCall(
            [cb, ud](const std::string& name, const std::string& args) {
              char* name_copy = api_strdup(name.c_str());
              char* args_copy = api_strdup(args.c_str());
              cb(name_copy, args_copy, ud);
            });
      }

    }

    // Stop any in-progress ASR from a previous session.
    if (rt->asr_recording.load() && rt->asr_recorder) {
      rt->asr_recording.store(false);
      rt->asr_recorder->Stop();
    }
    if (rt->asr_thread.joinable()) {
      rt->asr_thread.join();
    }
    rt->asr_recorder.reset();
    rt->asr_client.reset();
    rt->asr_token_mgr.reset();

    // --- TTS + audio playout pipeline ----------------------------------------
    if (rt->voice_configured) {
      using namespace shizuru;
      const auto& vc = rt->voice_config;

      const std::string tts_provider = vc.value("tts_provider", "baidu");
      constexpr size_t kCh = 1;
      std::string tts_id;
      int tts_sample_rate = 16000;

      if (tts_provider == "elevenlabs") {
        services::ElevenLabsConfig el_cfg;
        el_cfg.api_key     = vc.value("tts_api_key", "");
        el_cfg.voice_id    = vc.value("tts_voice_id", el_cfg.voice_id);
        el_cfg.output_format = services::TtsOutputFormat::kPcm16000;
        rt->runtime->RegisterDevice(
            std::make_unique<io::ElevenLabsTtsDevice>(std::move(el_cfg)));
        tts_id = "elevenlabs_tts";
      } else {
        services::BaiduConfig baidu_cfg;
        baidu_cfg.api_key    = vc.value("asr_api_key", "");
        baidu_cfg.secret_key = vc.value("asr_secret_key", "");
        baidu_cfg.aue = 6;  // 6=WAV; Dart reads via shizuru_take_audio()
        rt->runtime->RegisterDevice(
            std::make_unique<io::BaiduTtsDevice>(std::move(baidu_cfg)));
        tts_id = "baidu_tts";
      }

      constexpr runtime::RouteOptions kDma{false};
      // Route TTS audio to DartAudioOutputDevice; Dart polls via shizuru_take_audio().
      rt->runtime->RegisterDevice(std::make_unique<DartAudioOutputDevice>(rt));
      rt->runtime->AddRoute({tts_id,           "audio_out"},
                             {"dart_audio_out", "audio_in"}, kDma);

      // --- ASR recording pipeline (click-to-talk) ----------------------------
      const std::string asr_key = vc.value("asr_api_key", "");
      const std::string asr_sec = vc.value("asr_secret_key", "");
      if (!asr_key.empty() && !asr_sec.empty()) {
        services::BaiduConfig asr_cfg;
        asr_cfg.api_key    = asr_key;
        asr_cfg.secret_key = asr_sec;
        asr_cfg.asr_format = "pcm";
        rt->asr_token_mgr = std::make_shared<services::BaiduTokenManager>(asr_cfg);
        rt->asr_client = std::make_unique<services::BaiduAsrClient>(asr_cfg,
                                                                      rt->asr_token_mgr);

        io::RecorderConfig rec_cfg;
        rec_cfg.sample_rate             = 16000;
        rec_cfg.channel_count           = 1;
        rec_cfg.frames_per_buffer       = 320;       // 20ms at 16kHz
        rec_cfg.buffer_capacity_samples = 16000 * 30; // 30s max buffer
        rt->asr_recorder = std::make_unique<io::PaRecorder>(rec_cfg);
        rt->asr_recorder->SetFrameCallback([rt](const io::AudioFrame& frame) {
          if (!rt->asr_recording.load()) return;
          const auto* bytes = reinterpret_cast<const uint8_t*>(frame.data);
          const size_t n = frame.NumSamples() * sizeof(int16_t);
          std::lock_guard<std::mutex> lock(rt->asr_audio_mutex);
          rt->asr_audio_buffer.insert(rt->asr_audio_buffer.end(), bytes, bytes + n);
        });
      }
    }

    std::string session_id = rt->runtime->StartSession();
    return api_strdup(session_id.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[shizuru_api] shizuru_start_session failed: %s\n",
                 e.what());
    return nullptr;
  }
}

void shizuru_shutdown(ShizuruRuntime* rt) {
  if (!rt || !rt->runtime) return;
  rt->runtime->Shutdown();
}

int32_t shizuru_has_active_session(ShizuruRuntime* rt) {
  if (!rt || !rt->runtime) return 0;
  return rt->runtime->HasActiveSession() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

void shizuru_send_message(ShizuruRuntime* rt, const char* content) {
  if (!rt || !rt->runtime || !content) return;
  rt->runtime->SendMessage(std::string(content));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

int32_t shizuru_get_state(ShizuruRuntime* rt) {
  if (!rt || !rt->runtime) return SHIZURU_STATE_TERMINATED;
  return static_cast<int32_t>(rt->runtime->GetState());
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void shizuru_set_output_callback(ShizuruRuntime* rt,
                                  ShizuruOutputCallback cb,
                                  void* user_data) {
  if (!rt) return;
  std::lock_guard<std::mutex> lock(rt->cb_mutex);
  rt->output_cb = cb;
  rt->output_ud = user_data;
}

void shizuru_set_state_callback(ShizuruRuntime* rt,
                                 ShizuruStateCallback cb,
                                 void* user_data) {
  if (!rt) return;
  std::lock_guard<std::mutex> lock(rt->cb_mutex);
  rt->state_cb = cb;
  rt->state_ud = user_data;
}

void shizuru_set_tool_call_callback(ShizuruRuntime* rt,
                                     ShizuruToolCallCallback cb,
                                     void* user_data) {
  if (!rt) return;
  std::lock_guard<std::mutex> lock(rt->cb_mutex);
  rt->tool_call_cb = cb;
  rt->tool_call_ud = user_data;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void shizuru_register_tool(ShizuruRuntime* rt,
                            const char* name,
                            const char* description,
                            const char* params_json,
                            ShizuruToolHandler handler,
                            void* user_data) {
  if (!rt || !name) return;

  const std::string tool_name(name);
  const std::string tool_desc(description ? description : "");

  // 1. Add tool definition to LLM config so the model knows about it.
  shizuru::services::ToolDefinition def;
  def.name = tool_name;
  def.description = tool_desc;
  def.required_capability = tool_name;

  if (params_json) {
    try {
      auto params = json::parse(params_json);
      if (params.is_array()) {
        for (const auto& p : params) {
          shizuru::services::ToolParameter tp;
          tp.name        = p.value("name", "");
          tp.type        = p.value("type", "string");
          tp.description = p.value("description", "");
          tp.required    = p.value("required", false);
          def.parameters.push_back(std::move(tp));
        }
      }
    } catch (...) {
      std::fprintf(stderr, "[shizuru_api] Failed to parse params_json for tool '%s'\n", name);
    }
  }

  rt->config.llm.tools.push_back(std::move(def));

  // 2. Add policy rule + capability to allow this tool.
  shizuru::core::PolicyRule rule;
  rule.priority = 0;
  rule.action_pattern = tool_name;
  rule.required_capability = tool_name;
  rule.outcome = shizuru::core::PolicyOutcome::kAllow;
  rt->config.policy.initial_rules.push_back(std::move(rule));
  rt->config.policy.default_capabilities.insert(tool_name);

  // 3. Register handler in ToolRegistry (if provided).
  if (handler) {
    ShizuruToolHandler h = handler;
    void* ud = user_data;
    rt->tools.Register(tool_name,
        [h, ud](const std::string& arguments) -> shizuru::services::ToolResult {
          const char* result_json = h(arguments.c_str(), ud);
          shizuru::services::ToolResult result;
          if (result_json) {
            try {
              auto j = json::parse(result_json);
              result.success = j.value("success", false);
              result.output = j.value("output", "");
              result.error_message = j.value("error", "");
            } catch (...) {
              result.success = false;
              result.error_message = "Failed to parse tool result JSON";
            }
            std::free(const_cast<char*>(result_json));
          } else {
            result.success = false;
            result.error_message = "Tool handler returned NULL";
          }
          return result;
        });
  }
}

// ---------------------------------------------------------------------------
// Voice pipeline
// ---------------------------------------------------------------------------

int32_t shizuru_setup_voice(ShizuruRuntime* rt,
                             const char* voice_config_json) {
  if (!rt || !voice_config_json) return 0;

  try {
    rt->voice_config = json::parse(voice_config_json);
    rt->voice_configured = true;
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[shizuru_api] shizuru_setup_voice failed: %s\n",
                 e.what());
    return 0;
  }
}

void shizuru_speak(ShizuruRuntime* rt, const char* text) {
  if (!rt || !rt->runtime || !text) return;
  rt->runtime->SpeakText(std::string(text));
}

void shizuru_stop_speaking(ShizuruRuntime* rt) {
  if (!rt || !rt->runtime) return;
  rt->runtime->StopSpeaking();
}

void shizuru_set_transcription_callback(ShizuruRuntime* rt,
                                         ShizuruTranscriptionCallback cb,
                                         void* user_data) {
  if (!rt) return;
  rt->transcription_cb = cb;
  rt->transcription_ud = user_data;
}

int32_t shizuru_start_recording(ShizuruRuntime* rt) {
  if (!rt || !rt->asr_recorder) return 0;
  if (rt->asr_recording.load()) return 1;  // already recording
  {
    std::lock_guard<std::mutex> lock(rt->asr_audio_mutex);
    rt->asr_audio_buffer.clear();
  }
  rt->asr_recording.store(true);
  try {
    rt->asr_recorder->Start();
    return 1;
  } catch (const std::exception& e) {
    rt->asr_recording.store(false);
    LOG_ERROR("shizuru_start_recording: {}", e.what());
    return 0;
  }
}

void shizuru_stop_recording(ShizuruRuntime* rt) {
  if (!rt || !rt->asr_recorder) return;
  if (!rt->asr_recording.load()) return;

  rt->asr_recording.store(false);
  rt->asr_recorder->Stop();

  std::vector<uint8_t> audio;
  {
    std::lock_guard<std::mutex> lock(rt->asr_audio_mutex);
    audio.swap(rt->asr_audio_buffer);
  }

  if (rt->asr_thread.joinable()) {
    rt->asr_thread.join();
  }

  rt->asr_thread = std::thread([rt, audio = std::move(audio)]() {
    ShizuruTranscriptionCallback cb = rt->transcription_cb;
    void* ud = rt->transcription_ud;
    if (!cb) return;

    if (audio.empty() || !rt->asr_client) {
      cb(nullptr, ud);
      return;
    }

    try {
      const std::string audio_str(reinterpret_cast<const char*>(audio.data()),
                                   audio.size());
      const std::string transcript =
          rt->asr_client->Transcribe(audio_str, "audio/pcm");
      if (!transcript.empty()) {
        char* text = api_strdup(transcript.c_str());
        cb(text, ud);
      } else {
        cb(nullptr, ud);
      }
    } catch (const std::exception& e) {
      LOG_ERROR("shizuru_stop_recording: transcription failed: {}", e.what());
      cb(nullptr, ud);
    }
  });
}

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------

void shizuru_free_string(const char* str) {
  std::free(const_cast<char*>(str));
}

int64_t shizuru_peek_audio_size(ShizuruRuntime* rt) {
  if (!rt) return 0;
  std::lock_guard<std::mutex> lock(rt->pending_audio_mutex);
  return static_cast<int64_t>(rt->pending_audio.size());
}

int64_t shizuru_take_audio_into(ShizuruRuntime* rt, uint8_t* buf,
                                  int64_t buf_size) {
  if (!rt || !buf || buf_size <= 0) return -1;
  std::lock_guard<std::mutex> lock(rt->pending_audio_mutex);
  if (rt->pending_audio.empty()) return 0;
  const int64_t n = static_cast<int64_t>(rt->pending_audio.size());
  if (n > buf_size) return -1;
  std::memcpy(buf, rt->pending_audio.data(), static_cast<size_t>(n));
  rt->pending_audio.clear();
  return n;
}
