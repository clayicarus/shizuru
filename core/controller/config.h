#pragma once

#include <chrono>

namespace shizuru::core {

struct ControllerConfig {
  int max_retries = 3;                               // LLM retry limit
  std::chrono::milliseconds retry_base_delay{1000};  // Exponential backoff base
  std::chrono::seconds conversation_idle_timeout{60}; // Idle gap that resets budgets
  int token_budget = 100000;                         // Max cumulative tokens per turn
  int action_count_limit = 10;                       // Max tool calls per turn (action loop guard)
  int max_continuations = 5;                         // Max LLM kContinue per turn (output loop guard)
  bool use_streaming = false;                        // Use SSE streaming for LLM responses
  std::chrono::seconds tool_call_timeout{30};        // Max wait time for tool results
  std::chrono::milliseconds debounce_duration{500};  // Post-interrupt debounce window
  std::chrono::milliseconds aggregation_timeout{5000}; // Aggregator timeout before force-flush
};

}  // namespace shizuru::core
