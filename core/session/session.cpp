#include "session/session.h"

#include <utility>

namespace shizuru::core {

AgentSession::AgentSession(const std::string& session_id,
                           ControllerConfig ctrl_config,
                           ContextConfig ctx_config,
                           PolicyConfig pol_config,
                           std::unique_ptr<LlmClient> llm,
                           std::unique_ptr<IoBridge> io,
                           std::unique_ptr<MemoryStore> memory,
                           std::unique_ptr<AuditSink> audit)
    : session_id_(session_id),
      memory_(std::move(memory)),
      audit_(std::move(audit)),
      context_(std::move(ctx_config), *memory_),
      policy_(std::move(pol_config), *audit_),
      controller_(session_id_,
                  std::move(ctrl_config),
                  std::move(llm),
                  std::move(io),
                  context_,
                  policy_) {
  context_.InitSession(session_id_);
  policy_.InitSession(session_id_);
}

AgentSession::~AgentSession() {
  if (GetState() != State::kTerminated) {
    Shutdown();
  }
}

void AgentSession::Start() {
  controller_.Start();
}

void AgentSession::Shutdown() {
  controller_.Shutdown();
  context_.ReleaseSession(session_id_);
  policy_.ReleaseSession(session_id_);
}

void AgentSession::EnqueueObservation(Observation obs) {
  controller_.EnqueueObservation(std::move(obs));
}

State AgentSession::GetState() const {
  return controller_.GetState();
}

}  // namespace shizuru::core
