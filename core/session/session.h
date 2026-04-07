#pragma once

#include <functional>
#include <memory>
#include <string>

#include "context/config.h"
#include "context/context_strategy.h"
#include "controller/config.h"
#include "controller/controller.h"
#include "controller/types.h"
#include "interfaces/audit_sink.h"
#include "interfaces/llm_client.h"
#include "interfaces/memory_store.h"
#include "policy/config.h"
#include "policy/policy_layer.h"
#include "strategies/observation_filter.h"
#include "strategies/response_filter.h"
#include "strategies/tts_segment_strategy.h"

namespace shizuru::core {

// Owns the lifecycle of a single agent session.
// Wires Controller, ContextStrategy, and PolicyLayer together.
class AgentSession {
 public:
  AgentSession(const std::string& session_id,
               ControllerConfig ctrl_config,
               ContextConfig ctx_config,
               PolicyConfig pol_config,
               std::unique_ptr<LlmClient> llm,
               Controller::EmitFrameCallback emit_frame,
               Controller::CancelCallback cancel,
               std::unique_ptr<MemoryStore> memory,
               std::unique_ptr<AuditSink> audit,
               std::unique_ptr<ObservationFilter> observation_filter = nullptr,
               std::unique_ptr<TtsSegmentStrategy> tts_segment = nullptr,
               std::unique_ptr<ResponseFilter> response_filter = nullptr);

  ~AgentSession();

  void Start();
  void Shutdown();
  void EnqueueObservation(Observation obs);
  State GetState() const;

  const std::string& SessionId() const { return session_id_; }
  Controller& GetController() { return controller_; }
  ContextStrategy& GetContext() { return context_; }
  PolicyLayer& GetPolicy() { return policy_; }

 private:
  std::string session_id_;
  std::unique_ptr<MemoryStore> memory_;
  std::unique_ptr<AuditSink> audit_;
  ContextStrategy context_;
  PolicyLayer policy_;
  Controller controller_;
};

}  // namespace shizuru::core
