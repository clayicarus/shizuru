// app/assembly/app_runtime.cpp — Product-level runtime assembly.

#include "app/assembly/app_runtime.h"

#include <chrono>
#include <string>
#include <utility>

#include "core/conversation_item.h"
#include "core/control_signal.h"
#include "app/persona/persona.h"
#include "app/memory/sqlite_memory_store.h"
#include "app/tools/builtin_tools.h"
#include "async_logger.h"
#include "runtime/tool_dispatch_device.h"
#include "runtime/core_device.h"
#include "runtime/route_table.h"
#include "io/vad/energy_vad_device.h"
#include "services/audit/log_audit_sink.h"
#include "services/llm/openai/openai_client.h"

// Strategies
#include "core/strategies/response_filter.h"
#include "core/strategies/tts_segment_strategy.h"

namespace shizuru::app {
namespace {

constexpr size_t kMaxStartupHistoryReplayEntries = 256;

}  // namespace

AppRuntime::AppRuntime(AppConfig config) : config_(std::move(config)) {
  // Initialize logger.
  core::InitLogger(config_.logger);

  // Append builtin tool definitions to LLM config.
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
  std::string system_prompt = BuildSystemPrompt({}, {});

  if (!config_.user_instruction.empty()) {
    system_prompt += "\n\n## Additional instructions from user\n";
    system_prompt += config_.user_instruction;
  }

  config_.context.default_system_instruction = system_prompt;

  // ── Build strategy instances ─────────────────────────────────────────────
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
  const std::string generated_session_id =
      "session-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string session_id =
      !config_.user_id.empty() ? config_.user_id : generated_session_id;

  std::unique_ptr<core::HistoryStore> history_store;
  if (!config_.db_path.empty()) {
    history_store = std::make_unique<app::SqliteMemoryStore>(config_.db_path);
  } else {
    history_store = std::make_unique<app::SqliteMemoryStore>(":memory:");
  }

  std::vector<core::ConversationItem> persisted_items;
  {
    persisted_items =
        history_store->GetRecent(session_id, kMaxStartupHistoryReplayEntries);
  }

  auto core = std::make_unique<runtime::CoreDevice>(
      "core", session_id,
      config_.controller, config_.context, config_.policy,
      std::make_unique<services::OpenAiClient>(config_.llm),
      std::move(history_store),
      std::make_unique<services::LogAuditSink>(),
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
      [this](const core::ConversationItem& item, bool is_delta) {
        ConversationItemCallback cb;
        {
          std::lock_guard<std::mutex> lock(cb_mutex_);
          cb = conversation_item_cb_;
        }
        if (cb) { cb(item, is_delta); }
      });

  // ── Create ToolDispatchDevice ────────────────────────────────────────────
  auto tool_dispatch = std::make_unique<runtime::ToolDispatchDevice>(tools_);

  // Wire tool dispatch to deliver ToolResultSignal to CoreDevice.
  tool_dispatch->SetOnResultCallback([this](core::ToolResultSignal signal) {
    if (core_device_) {
      core_device_->OnControl(std::move(signal));
    }
  });

  // ── Create SchedulerDevice ───────────────────────────────────────────────
  auto scheduler = std::make_unique<SchedulerDevice>();
  scheduler_ = scheduler.get();

  // Wire scheduler to deliver items to core.
  scheduler_->SetOnItemCallback([this](core::ConversationItem item) {
    if (core_device_) {
      core_device_->OnConversationItem(std::move(item));
    }
  });

  // ── Register builtin tool functions (now that scheduler is available) ────
  RegisterBuiltinTools(tools_, scheduler_);

  // ── Register core devices on bus ─────────────────────────────────────────
  bus_.RegisterDevice(std::move(core));
  bus_.RegisterDevice(std::move(tool_dispatch));
  // Note: SchedulerDevice is not an IoDevice — it delivers via callback.

  // ── Wire core routes ─────────────────────────────────────────────────────
  WireRoutes();

  // ── Replay persisted conversation history for the UI ────────────────────
  ReplayPersistedConversationHistory(persisted_items);

  // ── Start all auto_start devices ─────────────────────────────────────────
  bus_.StartAll();

  // Start scheduler timer thread.
  scheduler_->Start();
}

void AppRuntime::SendMessage(const std::string& text) {
  if (core_device_ == nullptr) { return; }

  core::ConversationItem item;
  item.item_id = "ui:" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  item.conversation_id = config_.user_id;
  item.kind = core::ConversationItemKind::kUserMessage;
  item.actor = core::ActorRef{config_.user_id, "User", core::ActorKind::kHuman};
  item.parts.emplace_back(core::TextPart{text});
  item.wall_time = std::chrono::system_clock::now();

  core_device_->OnConversationItem(std::move(item));
}

core::State AppRuntime::GetState() const {
  if (core_device_ == nullptr) { return core::State::kIdle; }
  return core_device_->GetState();
}

void AppRuntime::Shutdown() {
  if (scheduler_) {
    scheduler_->Stop();
  }
  core_device_ = nullptr;
  scheduler_ = nullptr;
  bus_.Shutdown();
}

void AppRuntime::ClearDatabase() {
  if (core_device_ == nullptr) { return; }
  auto& session = core_device_->Session();
  const auto& sid = session.SessionId();

  session.GetHistoryStore().Clear(sid);

  session.GetContext().ReleaseSession(sid);
  session.GetContext().InitSession(
      sid, config_.context.default_system_instruction);
}

void AppRuntime::ClearContext() {
  if (core_device_ == nullptr) { return; }
  auto& session = core_device_->Session();
  const auto& sid = session.SessionId();

  session.GetContext().ReleaseSession(sid);
  session.GetContext().InitSession(
      sid, config_.context.default_system_instruction);
}

void AppRuntime::ReplayPersistedConversationHistory(
    const std::vector<core::ConversationItem>& items) {
  ConversationItemCallback cb;
  {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    cb = conversation_item_cb_;
  }
  if (!cb) {
    return;
  }
  for (const auto& item : items) {
    cb(item, false);
  }
}

void AppRuntime::WireRoutes() {
  using runtime::PortAddress;
  using runtime::RouteOptions;
  constexpr RouteOptions kDma{.requires_control_plane = false};
  constexpr RouteOptions kCtrl{.requires_control_plane = true};

  // TTS route: assistant semantic output → TTS device.
  bus_.AddRoute({"core", "item_out"}, {"elevenlabs_tts", "item_in"}, kDma);

  // Tool call round-trip.
  // Semantic tool dispatch now flows over the typed control plane:
  //   core:signal_out -> tool_dispatch:control_in
  // Tool results already return via ToolResultSignal into CoreDevice::OnControl().
  bus_.AddRoute({"core", "signal_out"},
                {"tool_dispatch", "control_in"}, kCtrl);

  // VAD route: speech_end drives ASR flush via typed control signal.
  bus_.AddRoute({"vad", io::EnergyVadDevice::kControlSignalOut},
                {"baidu_asr", "signal_in"}, kCtrl);

  // Control plane: core → IO devices via typed signals.
  bus_.AddRoute({"core", "signal_out"}, {"baidu_asr", "signal_in"}, kCtrl);
  bus_.AddRoute({"core", "signal_out"}, {"elevenlabs_tts", "signal_in"}, kCtrl);
  bus_.AddRoute({"core", "signal_out"}, {"audio_playout", "signal_in"}, kCtrl);
}

}  // namespace shizuru::app
