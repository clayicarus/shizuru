#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "context/config.h"
#include "context/types.h"
#include "controller/types.h"
#include "interfaces/memory_store.h"

namespace shizuru::core {

class ContextStrategy {
 public:
  ContextStrategy(ContextConfig config, MemoryStore& store);

  // Initialize session with system instruction.
  void InitSession(const std::string& session_id,
                   const std::string& system_instruction = "");

  // Build a context window for the current turn.
  ContextWindow BuildContext(const std::string& session_id,
                             const Observation& current_observation);

  // Record a completed turn into memory.
  void RecordTurn(const std::string& session_id, const MemoryEntry& entry);

  // Inject external context (retrieved docs, user profile, etc.)
  void InjectContext(const std::string& session_id, const MemoryEntry& entry);

  // Update system instruction mid-session.
  void SetSystemInstruction(const std::string& session_id,
                            const std::string& instruction);

  // Release ephemeral per-session state while preserving committed history.
  void ReleaseSession(const std::string& session_id);

 private:
  void MaybeSummarize(const std::string& session_id);
  int EstimateTokens(const std::vector<MemoryEntry>& entries) const;

  ContextConfig config_;
  MemoryStore& store_;

  // Per-session system instructions
  std::mutex instruction_mutex_;
  std::unordered_map<std::string, std::string> system_instructions_;
};

}  // namespace shizuru::core
