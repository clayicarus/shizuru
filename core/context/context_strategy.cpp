#include "context/context_strategy.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace shizuru::core {

int EntryTokens(const MemoryEntry& entry) {
  return static_cast<int>((entry.content.size() + entry.tool_calls_json.size()) / 4);
}

ContextStrategy::ContextStrategy(ContextConfig config, MemoryStore& store)
    : config_(std::move(config)), store_(store) {}

void ContextStrategy::InitSession(const std::string& session_id,
                                  const std::string& system_instruction) {
  std::lock_guard<std::mutex> lock(instruction_mutex_);
  if (system_instruction.empty()) {
    system_instructions_[session_id] = config_.default_system_instruction;
  } else {
    system_instructions_[session_id] = system_instruction;
  }
}

ContextWindow ContextStrategy::BuildContext(
    const std::string& session_id,
    const Observation& current_observation) {
  // 1. Get system instruction.
  std::string system_instruction;
  {
    std::lock_guard<std::mutex> lock(instruction_mutex_);
    auto it = system_instructions_.find(session_id);
    if (it != system_instructions_.end()) {
      system_instruction = it->second;
    } else {
      system_instruction = config_.default_system_instruction;
    }
  }

  // 2. Convert current observation to a ContextMessage.
  // kContinuation is a no-op signal: no message is appended to the context.
  ContextMessage obs_message;
  bool append_obs = (current_observation.type != ObservationType::kContinuation);
  if (append_obs) {
    switch (current_observation.type) {
      case ObservationType::kUserMessage:
        obs_message.role = "user";
        break;
      case ObservationType::kToolResult:
        obs_message.role = "tool";
        break;
      case ObservationType::kSystemEvent:
      case ObservationType::kInterruption:
        obs_message.role = "system";
        break;
      case ObservationType::kContinuation:
        break;  // unreachable
    }
    obs_message.content = current_observation.content;
  }

  // 3. Calculate fixed token costs.
  int system_tokens = static_cast<int>(system_instruction.size()) / 4;
  int observation_tokens =
      append_obs ? static_cast<int>(obs_message.content.size()) / 4 : 0;
  int remaining_budget =
      config_.max_context_tokens - system_tokens - observation_tokens;
  if (remaining_budget < 0) {
    remaining_budget = 0;
  }

  // 4. Get all memory entries and select which fit in the budget.
  auto all_entries = store_.GetAll(session_id);

  // Iterate from newest to oldest, accumulating tokens.
  // Track which entries are included.
  std::vector<bool> included(all_entries.size(), false);
  int accumulated_tokens = 0;

  for (int i = static_cast<int>(all_entries.size()) - 1; i >= 0; --i) {
    int entry_tokens = EntryTokens(all_entries[i]);
    if (accumulated_tokens + entry_tokens <= remaining_budget) {
      included[i] = true;
      accumulated_tokens += entry_tokens;
    }
  }

  // 5. Enforce tool_call/tool_result pairing.
  // If a tool_result is included, its paired tool_call must also be included
  // (and vice versa). If the pair can't fit, remove both.
  for (size_t i = 0; i < all_entries.size(); ++i) {
    if (all_entries[i].type == MemoryEntryType::kToolResult && included[i]) {
      // Find the paired tool_call (preceding entry with same tool_call_id).
      bool found_pair = false;
      for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
        if (all_entries[j].type == MemoryEntryType::kToolCall &&
            all_entries[j].tool_call_id == all_entries[i].tool_call_id) {
          if (!included[j]) {
            // Try to include the paired tool_call.
            int pair_tokens = EntryTokens(all_entries[j]);
            if (accumulated_tokens + pair_tokens <= remaining_budget) {
              included[j] = true;
              accumulated_tokens += pair_tokens;
            } else {
              // Can't fit the pair — remove both.
              included[i] = false;
              accumulated_tokens -= EntryTokens(all_entries[i]);
            }
          }
          found_pair = true;
          break;
        }
      }
      if (!found_pair) {
        // Orphaned tool_result with no matching tool_call — keep as-is.
      }
    }

    if (all_entries[i].type == MemoryEntryType::kToolCall && included[i]) {
      // Find the paired tool_result (following entry with same tool_call_id).
      bool found_pair = false;
      for (size_t j = i + 1; j < all_entries.size(); ++j) {
        if (all_entries[j].type == MemoryEntryType::kToolResult &&
            all_entries[j].tool_call_id == all_entries[i].tool_call_id) {
          if (!included[j]) {
            // Try to include the paired tool_result.
            int pair_tokens = EntryTokens(all_entries[j]);
            if (accumulated_tokens + pair_tokens <= remaining_budget) {
              included[j] = true;
              accumulated_tokens += pair_tokens;
            } else {
              // Can't fit the pair — remove both.
              included[i] = false;
              accumulated_tokens -= EntryTokens(all_entries[i]);
            }
          }
          found_pair = true;
          break;
        }
      }
      if (!found_pair) {
        // Orphaned tool_call with no matching tool_result — keep as-is.
      }
    }
  }

  // 6. Assemble the ContextWindow.
  ContextWindow window;

  // System message first.
  ContextMessage system_msg;
  system_msg.role = "system";
  system_msg.content = system_instruction;
  window.messages.push_back(std::move(system_msg));

  // Memory entries in chronological order (only included ones).
  for (size_t i = 0; i < all_entries.size(); ++i) {
    if (!included[i]) continue;

    ContextMessage msg;
    msg.role = all_entries[i].role;
    msg.content = all_entries[i].content;
    msg.tool_call_id = all_entries[i].tool_call_id;
    // source_tag maps to name for tool messages.
    msg.name = all_entries[i].source_tag;
    // Propagate tool_calls_json for assistant tool call entries.
    msg.tool_calls_json = all_entries[i].tool_calls_json;
    window.messages.push_back(std::move(msg));
  }

  // Current observation last (skipped for kContinuation).
  if (append_obs) {
    window.messages.push_back(std::move(obs_message));
  }

  // Calculate total estimated tokens.
  window.estimated_tokens = system_tokens + accumulated_tokens + observation_tokens;

  return window;
}

void ContextStrategy::RecordTurn(const std::string& session_id,
                                 const MemoryEntry& entry) {
  store_.Append(session_id, entry);
  MaybeSummarize(session_id);
}

void ContextStrategy::InjectContext(const std::string& session_id,
                                    const MemoryEntry& entry) {
  store_.Append(session_id, entry);
}

void ContextStrategy::SetSystemInstruction(const std::string& session_id,
                                           const std::string& instruction) {
  std::lock_guard<std::mutex> lock(instruction_mutex_);
  system_instructions_[session_id] = instruction;
}

void ContextStrategy::ReleaseSession(const std::string& session_id) {
  store_.Clear(session_id);
  {
    std::lock_guard<std::mutex> lock(instruction_mutex_);
    system_instructions_.erase(session_id);
  }
}

void ContextStrategy::MaybeSummarize(const std::string& session_id) {
  auto all = store_.GetAll(session_id);
  if (static_cast<int>(all.size()) <= config_.summarization_threshold) {
    return;
  }

  // Summarize the oldest half of entries.
  size_t half = all.size() / 2;

  // Build a summary entry.
  MemoryEntry summary;
  summary.type = MemoryEntryType::kSummary;
  summary.role = "system";
  summary.content =
      "Summary of " + std::to_string(half) + " entries";
  summary.timestamp = std::chrono::steady_clock::now();

  // Rough token estimate: sum of summarized entries' tokens / 4
  // (compression ratio), with a minimum of the content size / 4.
  int summarized_tokens = 0;
  for (size_t i = 0; i < half; ++i) {
    summarized_tokens += EntryTokens(all[i]);
  }
  summary.estimated_tokens =
      static_cast<int>(summary.content.size()) / 4;

  store_.Summarize(session_id, 0, half, summary);
}

int ContextStrategy::EstimateTokens(
    const std::vector<MemoryEntry>& entries) const {
  int total = 0;
  for (const auto& entry : entries) {
    total += EntryTokens(entry);
  }
  return total;
}

}  // namespace shizuru::core
