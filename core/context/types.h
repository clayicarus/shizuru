#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace shizuru::core {

enum class MemoryEntryType {
  kUserMessage,
  kAssistantMessage,
  kToolCall,
  kToolResult,
  kSummary,
  kExternalContext,
};

// A single message in the context window sent to the LLM.
struct ContextMessage {
  std::string role;           // "system", "user", "assistant", "tool"
  std::string content;
  std::string tool_call_id;   // For tool result messages
  std::string name;           // Tool name (for tool messages)
  std::string tool_calls_json; // JSON array string for assistant tool call messages
};

// The assembled prompt for a single LLM inference call.
struct ContextWindow {
  std::vector<ContextMessage> messages;
  int estimated_tokens = 0;
};

// A stored piece of conversation memory.
struct MemoryEntry {
  MemoryEntryType type;
  std::string role;           // "user", "assistant", "tool", "system"
  std::string content;
  std::string source_tag;     // For external context: origin identifier
  std::string tool_call_id;   // For tool call/result pairing
  std::string tool_calls_json; // JSON array string for kToolCall entries
  std::chrono::steady_clock::time_point timestamp;
  int estimated_tokens = 0;
};

}  // namespace shizuru::core
