#include "session/session.h"

#include <utility>

namespace shizuru::core {

AgentSession::AgentSession(const std::string& session_id,
                           ControllerConfig ctrl_config,
                           ContextConfig ctx_config,
                           PolicyConfig pol_config,
                           std::unique_ptr<LlmClient> llm,
                           Controller::EmitFrameCallback emit_frame,
                           Controller::CancelCallback cancel,
                           std::unique_ptr<MemoryStore> memory,
                           std::unique_ptr<AuditSink> audit,
                           std::unique_ptr<ObservationAggregator> observation_aggregator,
                           std::unique_ptr<ObservationFilter> observation_filter,
                           std::unique_ptr<TtsSegmentStrategy> tts_segment,
                           std::unique_ptr<ResponseFilter> response_filter)
    : session_id_(session_id),
      memory_(std::move(memory)),
      audit_(std::move(audit)),
      context_(std::move(ctx_config), *memory_),
      policy_(std::move(pol_config), *audit_),
      controller_(session_id_,
                  std::move(ctrl_config),
                  std::move(llm),
                  std::move(emit_frame),
                  std::move(cancel),
                  context_,
                  policy_,
                  std::move(observation_aggregator),
                  std::move(observation_filter),
                  std::move(tts_segment),
                  std::move(response_filter)) {
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
