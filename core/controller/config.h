#pragma once

#include <chrono>

namespace shizuru::core {

struct ControllerConfig {
  int max_turns = 20;                                // Stop condition: max turns per session
  int max_retries = 3;                               // LLM retry limit
  std::chrono::milliseconds retry_base_delay{1000};  // Exponential backoff base
  std::chrono::seconds turn_timeout{30};             // Max duration of one user turn
  std::chrono::seconds conversation_idle_timeout{60}; // Idle gap that resets budgets
  int token_budget = 100000;                         // Max cumulative tokens
  int action_count_limit = 50;                       // Max IO actions per session
  bool use_streaming = false;                        // Use SSE streaming for LLM responses
  std::chrono::seconds tool_call_timeout{30};        // Max wait time for tool results
};

}  // namespace shizuru::core
