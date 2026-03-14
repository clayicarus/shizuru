#include "agent_runtime.h"

#include <chrono>
#include <utility>

#include "io/tool_dispatcher.h"
#include "llm/openai_client.h"
#include "memory/in_memory_store.h"
#include "audit/log_audit_sink.h"

namespace shizuru::runtime {

namespace {

std::string GenerateSessionId() {
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count();
  return "session_" + std::to_string(ms);
}

}  // namespace

AgentRuntime::AgentRuntime(RuntimeConfig config,
                           services::ToolRegistry& tools)
    : config_(std::move(config)), tools_(tools) {}

AgentRuntime::~AgentRuntime() {
  if (session_) {
    Shutdown();
  }
}

std::string AgentRuntime::StartSession() {
  if (session_) {
    Shutdown();
  }

  std::string session_id = GenerateSessionId();

  auto llm = std::make_unique<services::OpenAiClient>(config_.llm);
  auto io = std::make_unique<services::ToolDispatcher>(tools_);
  auto memory = std::make_unique<services::InMemoryStore>();
  auto audit = std::make_unique<services::LogAuditSink>();

  session_ = std::make_unique<core::AgentSession>(
      session_id, config_.controller, config_.context, config_.policy,
      std::move(llm), std::move(io), std::move(memory), std::move(audit));

  session_->Start();
  return session_id;
}

void AgentRuntime::SendMessage(const std::string& content) {
  if (!session_) {
    return;
  }

  core::Observation obs;
  obs.type = core::ObservationType::kUserMessage;
  obs.content = content;
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  session_->EnqueueObservation(std::move(obs));
}

void AgentRuntime::Shutdown() {
  if (session_) {
    session_->Shutdown();
    session_.reset();
  }
}

core::State AgentRuntime::GetState() const {
  if (!session_) {
    return core::State::kTerminated;
  }
  return session_->GetState();
}

bool AgentRuntime::HasActiveSession() const {
  return session_ != nullptr;
}

}  // namespace shizuru::runtime
