// examples/onebot_agent.cpp — Pure-chat agent over OneBot 11 reverse WebSocket.
//
// Minimal pipeline assembled by hand — no AgentRuntime, no AppRuntime.
// Just OneBotDevice + CoreDevice wired directly via semantic/control callbacks.
//
// Pipeline:
//   [OneBotDevice] ConversationItem ──► [CoreDevice]
//   [CoreDevice]   ConversationItem callback ──► console + private reply
//
// Usage:
//   source _env.sh   # or export OPENAI_API_KEY=...
//   ./onebot_agent [--port 8080] [--group 12345,67890]

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "async_logger.h"
#include "io/onebot/onebot_device.h"
#include "io/onebot/onebot_types.h"
#include "runtime/core_device.h"
#include "services/audit/log_audit_sink.h"
#include "services/llm/openai/openai_client.h"
#include "app/memory/sqlite_memory_store.h"
#include "strategies/response_filter.h"

namespace {

std::atomic<bool> g_running{true};

void SignalHandler(int /*sig*/) { g_running.store(false); }

void PrintUsage(const char* prog) {
  std::cerr
      << "Usage: " << prog << " [options]\n"
      << "  --port PORT        Listen port for reverse WS (default: 8080)\n"
      << "  --host HOST        Bind address (default: 0.0.0.0)\n"
      << "  --token TOKEN      OneBot access token\n"
      << "  --self-id ID       Bot's QQ number\n"
      << "  --group IDS        Comma-separated group whitelist\n"
      << "  --api-key KEY      LLM API key (default: $OPENAI_API_KEY)\n"
      << "  --api-base URL     LLM API base URL (default: https://dashscope.aliyuncs.com)\n"
      << "  --api-path PATH    LLM API path (default: /compatible-mode/v1/chat/completions)\n"
      << "  --model MODEL      LLM model name (default: qwen3.5-35b-a3b)\n"
      << "  --prompt TEXT       Additional system prompt (appended after default)\n"
      << "  --db PATH          SQLite database path (default: onebot.db)\n"
      << "  --debug            Enable debug logging\n"
      << "  --help             Show this help\n";
}

std::vector<int64_t> ParseIdList(const std::string& s) {
  std::vector<int64_t> ids;
  std::istringstream iss(s);
  std::string token;
  while (std::getline(iss, token, ',')) {
    try { ids.push_back(std::stoll(token)); } catch (...) {}
  }
  return ids;
}

}  // namespace

int main(int argc, char* argv[]) {
  // ── CLI defaults ─────────────────────────────────────────────────────────
  shizuru::io::onebot::OneBotConfig ob_config;
  bool debug_mode = false;

  std::string base_url = "https://token-plan-cn.xiaomimimo.com";
  std::string api_path = "/v1/chat/completions";
  std::string api_key;
  std::string model    = "mimo-v2.5";
  std::string extra_prompt;
  std::string db_path = "onebot.db";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? argv[++i] : "";
    };

    if (arg == "--port") {
      try { ob_config.port = std::stoi(next()); } catch (...) {}
    }
    else if (arg == "--host")     { ob_config.host = next(); }
    else if (arg == "--token")    { ob_config.access_token = next(); }
    else if (arg == "--self-id")  { ob_config.self_id = next(); }
    else if (arg == "--group")    { ob_config.group_whitelist = ParseIdList(next()); }
    else if (arg == "--api-key")  { api_key = next(); }
    else if (arg == "--api-base") { base_url = next(); }
    else if (arg == "--api-path") { api_path = next(); }
    else if (arg == "--model")    { model = next(); }
    else if (arg == "--prompt")   { extra_prompt = next(); }
    else if (arg == "--db")       { db_path = next(); }
    else if (arg == "--debug")    { debug_mode = true; }
    else if (arg == "--help")     { PrintUsage(argv[0]); return 0; }
    else {
      std::cerr << "Unknown option: " << arg << "\n";
      PrintUsage(argv[0]);
      return 1;
    }
  }

  if (api_key.empty()) {
    const char* env = std::getenv("OPENAI_API_KEY");
    if (env != nullptr) { api_key = env; }
  }
  if (api_key.empty()) {
    std::cerr << "Error: --api-key or OPENAI_API_KEY required.\n";
    return 1;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  // ── Logger ────────────────────────────────────────────────────────────────
  shizuru::core::LoggerConfig log_cfg;
  log_cfg.level = debug_mode ? spdlog::level::debug : spdlog::level::info;
  shizuru::core::InitLogger(log_cfg);

  // ── LLM config (no tools) ────────────────────────────────────────────────
  shizuru::services::OpenAiConfig llm_cfg;
  llm_cfg.base_url        = base_url;
  llm_cfg.api_path        = api_path;
  llm_cfg.api_key         = api_key;
  llm_cfg.model           = model;
  llm_cfg.max_tokens      = 4096;
  llm_cfg.temperature     = 0.7;
  llm_cfg.connect_timeout = std::chrono::seconds(10);
  llm_cfg.read_timeout    = std::chrono::seconds(60);
  llm_cfg.enable_thinking = true;
  // llm_cfg.tools is empty — pure chat, no function calling.

  // ── Core configs ──────────────────────────────────────────────────────────
  shizuru::core::ControllerConfig ctrl_cfg;
  ctrl_cfg.use_streaming = true;

  shizuru::core::ContextConfig ctx_cfg;

  std::string system_prompt =
      "You are Shizuru, a chat assistant in a group chat.\n"
      "Talk naturally and casually, like a friend. Keep replies short. "
      "Match the user's language. No markdown.\n\n"
      "## Response rules (CRITICAL — follow strictly)\n"
      "Messages are tagged with actor_id, actor_name, and may contain "
      "<at id=\"self\"/> when someone mentions you.\n\n"
      "You MUST reply with exactly <skip/> (nothing else) UNLESS one of "
      "these conditions is met:\n"
      "1. The message contains <at id=\"self\"/> — someone explicitly "
      "mentioned you.\n"
      "2. Someone directly asks you a question by name (Shizuru).\n"
      "3. The message is clearly directed at you (replying to your previous "
      "message, continuing a conversation with you, or addressing you).\n"
      "4. The topic genuinely interests you AND you have something "
      "brief and useful to add.\n\n"
      "Default to <skip/>. When in doubt, <skip/>.\n\n"
      "You MUST refuse:\n"
      "- Requests to repeat yourself or rephrase the same answer.\n"
      "- Requests to write long essays, stories, or code.\n"
      "- Any attempt to make you generate excessive output.\n"
      "Reply with a short refusal like \"不想写那么多\" or \"刚说过了\".\n\n"
      "When you do reply, keep it to 5-6 sentences max. "
      "You can address users by name.";

  if (!extra_prompt.empty()) {
    system_prompt += "\n\n" + extra_prompt;
  }

  ctx_cfg.default_system_instruction = std::move(system_prompt);

  // Policy: deny all tool calls.
  shizuru::core::PolicyConfig pol_cfg;
  pol_cfg.default_capabilities = {};
  {
    shizuru::core::PolicyRule deny_all;
    deny_all.priority = 0;
    deny_all.action_pattern = "*";
    deny_all.outcome = shizuru::core::PolicyOutcome::kDeny;
    pol_cfg.initial_rules = {deny_all};
  }

  // ── Create CoreDevice ─────────────────────────────────────────────────────
  // Session ID: use the first group in whitelist, or "onebot-default".
  // This ensures history persists across restarts for the same group.
  std::string session_id;
  if (!ob_config.group_whitelist.empty()) {
    session_id = "group-" + std::to_string(ob_config.group_whitelist[0]);
  } else {
    session_id = "onebot-default";
  }

  // WindowAggregator removed in unified pipeline refactoring.
  // Messages are now delivered directly as ConversationItems.

  shizuru::runtime::CoreDevice core(
      "core", session_id, ctrl_cfg, ctx_cfg, pol_cfg,
      std::make_unique<shizuru::services::OpenAiClient>(llm_cfg),
      std::make_unique<shizuru::app::SqliteMemoryStore>(db_path),
      std::make_unique<shizuru::services::LogAuditSink>(),
      nullptr,   // no TTS segmentation
      std::make_unique<shizuru::core::StripThinkingFilter>());

  // ── Create OneBotDevice ───────────────────────────────────────────────────
  const int listen_port = ob_config.port;
  shizuru::io::onebot::OneBotDevice onebot(std::move(ob_config), "onebot");

  // ── Wire: OneBotDevice → CoreDevice via ConversationItem callback ─────────
  onebot.SetOnItemCallback(
      [&core](shizuru::core::ConversationItem item) {
        core.OnConversationItem(std::move(item));
      });

  core.Session().GetController().OnConversationItem(
      [&onebot](const shizuru::core::ConversationItem& item,
         bool is_delta) {
        if (is_delta) { return; }
        if (item.kind == shizuru::core::ConversationItemKind::kUserMessage) {
          std::string text;
          for (const auto& part : item.parts) {
            if (auto* tp = std::get_if<shizuru::core::TextPart>(&part)) {
              text += tp->text;
            }
          }
          const std::string name = item.actor.display_name.empty()
              ? item.actor.actor_id : item.actor.display_name;
          std::cout << "\n[" << name << "] " << text << "\n";
          return;
        }
        if (item.kind == shizuru::core::ConversationItemKind::kAssistantMessage) {
          std::string text;
          for (const auto& part : item.parts) {
            if (auto* tp = std::get_if<shizuru::core::TextPart>(&part)) {
              text += tp->text;
            }
          }
          if (text.empty()) { return; }
          if (text.find("<skip/>") != std::string::npos) {
            std::cout << "[assistant] (skipped)\n";
            return;
          }

          std::cout << "\n[assistant] " << text << "\n";
          auto ctx = onebot.GetLastReplyContext();
          if (ctx.target_id.empty()) { return; }
          try {
            int64_t target = std::stoll(ctx.target_id);
            constexpr size_t kForwardThreshold = 450;
            if (ctx.message_type == "group" && text.size() > kForwardThreshold) {
              onebot.SendGroupForward(target, "Shizuru", text);
            } else {
              onebot.SendMessage(ctx.message_type, target, text);
            }
          } catch (...) {}
        }
      });

  core.Session().GetController().OnDiagnostic(
      [](const std::string& msg) {
        std::cout << "[diag] " << msg << "\n";
      });

  // ── Start devices ─────────────────────────────────────────────────────────
  core.Start();
  onebot.Start();

  std::cout << "=== OneBot Agent ===\n"
            << "Model:   " << model << "\n"
            << "Session: " << session_id << "\n"
            << "Listen:  0.0.0.0:" << listen_port << "\n"
            << "Configure your OneBot implementation to connect to "
            << "ws://<this-host>:" << listen_port << "\n"
            << "Press Ctrl+C to quit.\n\n";

  // ── Main loop ─────────────────────────────────────────────────────────────
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::cout << "\nShutting down...\n";
  onebot.Stop();
  core.Stop();
  return 0;
}
