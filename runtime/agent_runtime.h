#pragma once

#include <memory>
#include <string>

#include "context/config.h"
#include "controller/config.h"
#include "controller/types.h"
#include "io/tool_registry.h"
#include "llm/config.h"
#include "policy/config.h"
#include "session/session.h"

namespace shizuru::runtime {

// Configuration bundle for creating an AgentRuntime.
struct RuntimeConfig {
  core::ControllerConfig controller;
  core::ContextConfig context;
  core::PolicyConfig policy;
  services::OpenAiConfig llm;
};

// Top-level entry point that assembles all components and manages
// the lifecycle of an AgentSession.
class AgentRuntime {
 public:
  AgentRuntime(RuntimeConfig config, services::ToolRegistry& tools);
  ~AgentRuntime();

  AgentRuntime(const AgentRuntime&) = delete;
  AgentRuntime& operator=(const AgentRuntime&) = delete;

  // Create and start a new session. Returns the session ID.
  std::string StartSession();

  // Send a user message to the active session.
  void SendMessage(const std::string& content);

  // Shut down the active session.
  void Shutdown();

  // Query the current state of the active session.
  core::State GetState() const;

  // Check if a session is active.
  bool HasActiveSession() const;

 private:
  RuntimeConfig config_;
  services::ToolRegistry& tools_;
  std::unique_ptr<core::AgentSession> session_;
};

}  // namespace shizuru::runtime
