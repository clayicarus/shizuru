// app/assembly/app_runtime.cpp — Product-level runtime assembly.

#include "app/assembly/app_runtime.h"

#include <chrono>
#include <string>
#include <utility>

#include "app/persona/persona.h"
#include "app/tools/builtin_tools.h"
#include "async_logger.h"
#include "runtime/tool_dispatch_device.h"
#include "runtime/core_device.h"
#include "runtime/route_table.h"
#include "services/audit/log_audit_sink.h"
#include "services/llm/openai/openai_client.h"
#include "services/memory/in_memory_store.h"

// Strategies
#include "core/strategies/llm_observation_aggregator.h"
#include "core/strategies/llm_observation_filter.h"
#include "core/strategies/response_filter.h"
#include "core/strategies/tts_segment_strategy.h"

namespace shizuru::app {

AppRuntime::AppRuntime(AppConfig config) : config_(std::move(config)) {
  // Initialize logger.
  core::InitLogger(config_.logger);

  // Append builtin tool definitions to LLM config.
  // Tool functions are registered later in Start() when scheduler is available.
  auto defs = BuiltinToolDefinitions();
  for (auto& d : defs) {
    config_.llm.tools.push_back(std::move(d));
  }
}

AppRuntime::~AppRuntime() { Shutdown(); }

runtime::AgentRuntime& AppRuntime::Bus() { return bus_; }

runtime::ToolRegistry& AppRuntime::Tools() { return tools_; }

SchedulerDevice* AppRuntime::Scheduler() { return scheduler_; }

void AppRuntime::OnDiagnostic(DiagnosticCallback cb) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  diagnostic_cb_ = std::move(cb);
}

void AppRuntime::OnActivity(ActivityCallback cb) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  activity_cb_ = std::move(cb);
}

void AppRuntime::OnConversationItem(ConversationItemCallback cb) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  conversation_item_cb_ = std::move(cb);
}

void AppRuntime::Start() {
  // ── Build system prompt with persona ─────────────────────────────────────
  // TODO: Load user preferences and active followups from persistent memory.
  std::string system_prompt = BuildSystemPrompt({}, {});

  // Append user's custom instruction if provided.
  if (!config_.user_instruction.empty()) {
    system_prompt += "\n\n## Additional instructions from user\n";
    system_prompt += config_.user_instruction;
  }

  config_.context.default_system_instruction = system_prompt;

  // ── Build strategy instances ─────────────────────────────────────────────
  // ObservationAggregator: LLM-based endpointing.
  std::unique_ptr<core::ObservationAggregator> obs_agg;
  {
    services::OpenAiConfig agg_cfg = config_.llm;
    agg_cfg.max_tokens  = 8;
    agg_cfg.temperature = 0.0;
    agg_cfg.connect_timeout = std::chrono::seconds(5);
    agg_cfg.read_timeout    = std::chrono::seconds(10);
    agg_cfg.tools.clear();  // Aggregator doesn't need tools.

    core::LlmAggregatorConfig agg_params;
    agg_params.aggregation_timeout = std::chrono::milliseconds(5000);
    agg_params.llm_timeout         = std::chrono::milliseconds(2000);

    obs_agg = std::make_unique<core::LlmObservationAggregator>(
        std::make_unique<services::OpenAiClient>(agg_cfg),
        std::move(agg_params));
  }

  // ObservationFilter: LLM-based relevance check.
  std::unique_ptr<core::ObservationFilter> obs_filter;
  {
    services::OpenAiConfig filter_cfg = config_.llm;
    filter_cfg.max_tokens  = 8;
    filter_cfg.temperature = 0.0;
    filter_cfg.connect_timeout = std::chrono::seconds(5);
    filter_cfg.read_timeout    = std::chrono::seconds(10);
    filter_cfg.tools.clear();

    obs_filter = std::make_unique<core::LlmObservationFilter>(
        std::make_unique<services::OpenAiClient>(filter_cfg));
  }

  // TtsSegmentStrategy: punctuation-based sentence segmentation.
  auto tts_seg = []() {
    core::PunctuationSegmentStrategy::Config seg_cfg;
    seg_cfg.min_chars = 15;
    seg_cfg.max_chars = 200;
    return std::make_unique<core::PunctuationSegmentStrategy>(seg_cfg);
  }();

  // ResponseFilter: strip <think> tags.
  auto resp_filter = std::make_unique<core::StripThinkingFilter>();

  // ── Create CoreDevice ────────────────────────────────────────────────────
  const std::string session_id =
      "session-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());

  auto core = std::make_unique<runtime::CoreDevice>(
      "core", session_id,
      config_.controller, config_.context, config_.policy,
      std::make_unique<services::OpenAiClient>(config_.llm),
      std::make_unique<services::InMemoryStore>(),
      std::make_unique<services::LogAuditSink>(),
      std::move(obs_agg),
      std::move(obs_filter),
      std::move(tts_seg),
      std::move(resp_filter));

  core_device_ = core.get();

  // ── Wire Controller callbacks ────────────────────────────────────────────
  core->Session().GetController().OnDiagnostic(
      [this](const std::string& msg) {
        DiagnosticCallback cb;
        {
          std::lock_guard<std::mutex> lock(cb_mutex_);
          cb = diagnostic_cb_;
        }
        if (cb) { cb(msg); }
      });

  core->Session().GetController().OnTransition(
      [this](core::State from, core::State to, core::Event event) {
        DiagnosticCallback cb;
        {
          std::lock_guard<std::mutex> lock(cb_mutex_);
          cb = diagnostic_cb_;
        }
        if (cb) {
          cb(std::string(core::StateName(from)) + " -> " +
             core::StateName(to) + " [" + core::EventName(event) + "]");
        }
      });

  core->Session().GetController().OnActivity(
      [this](const core::ActivityEvent& event) {
        ActivityCallback cb;
        {
          std::lock_guard<std::mutex> lock(cb_mutex_);
          cb = activity_cb_;
        }
        if (cb) { cb(event); }
      });

  core->Session().GetController().OnConversationItem(
      [this](const core::conversation::ConversationItem& item, bool is_delta) {
        ConversationItemCallback cb;
        {
          std::lock_guard<std::mutex> lock(cb_mutex_);
          cb = conversation_item_cb_;
        }
        if (cb) { cb(item, is_delta); }
      });

  // ── Create ToolDispatchDevice ────────────────────────────────────────────
  auto tool_dispatch = std::make_unique<runtime::ToolDispatchDevice>(tools_);

  // ── Create SchedulerDevice ───────────────────────────────────────────────
  auto scheduler = std::make_unique<SchedulerDevice>();
  scheduler_ = scheduler.get();

  // ── Register builtin tool functions (now that scheduler is available) ────
  RegisterBuiltinTools(tools_, scheduler_);

  // ── Register core devices on bus ─────────────────────────────────────────
  bus_.RegisterDevice(std::move(core));
  bus_.RegisterDevice(std::move(tool_dispatch));
  bus_.RegisterDevice(std::move(scheduler));

  // ── Wire core routes ─────────────────────────────────────────────────────
  WireRoutes();

  // ── Start all auto_start devices ─────────────────────────────────────────
  bus_.StartAll();
}

void AppRuntime::SendMessage(const std::string& text) {
  if (core_device_ == nullptr) { return; }
  io::DataFrame frame;
  frame.type = "text/plain";
  frame.payload.assign(text.begin(), text.end());
  frame.source_device = "user";
  frame.source_port = "text";
  frame.timestamp = std::chrono::steady_clock::now();
  core_device_->OnInput("text_in", std::move(frame));
}

core::State AppRuntime::GetState() const {
  if (core_device_ == nullptr) { return core::State::kIdle; }
  return core_device_->GetState();
}

void AppRuntime::Shutdown() {
  core_device_ = nullptr;
  scheduler_ = nullptr;
  bus_.Shutdown();
}

void AppRuntime::WireRoutes() {
  using runtime::PortAddress;
  using runtime::RouteOptions;
  constexpr RouteOptions kDma{.requires_control_plane = false};
  constexpr RouteOptions kCtrl{.requires_control_plane = true};

  // TTS segment route: core streaming chunks → TTS device.
  bus_.AddRoute({"core", "tts_out"}, {"elevenlabs_tts", "text_in"}, kDma);

  // Tool call round-trip.
  bus_.AddRoute({"core", "action_out"},
                {"tool_dispatch", runtime::ToolDispatchDevice::kActionIn}, kCtrl);
  bus_.AddRoute({"tool_dispatch", runtime::ToolDispatchDevice::kResultOut},
                {"core", "tool_result_in"}, kCtrl);

  // VAD event → core (interrupt detection).
  bus_.AddRoute({"vad_event", "vad_out"}, {"core", "vad_in"}, kDma);

  // Control plane: core → IO devices.
  bus_.AddRoute({"core", "control_out"}, {"baidu_asr", "control_in"}, kCtrl);
  bus_.AddRoute({"core", "control_out"}, {"elevenlabs_tts", "control_in"}, kCtrl);
  bus_.AddRoute({"core", "control_out"}, {"audio_playout", "control_in"}, kCtrl);

  // Scheduler event → core scheduler_in (proactive conversation, bypasses filter).
  bus_.AddRoute({"scheduler", SchedulerDevice::kEventOut},
                {"core", "scheduler_in"}, kDma);
}

}  // namespace shizuru::app
