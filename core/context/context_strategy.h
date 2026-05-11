#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "context/config.h"
#include "core/history.h"

namespace shizuru::core {

class ContextStrategy {
 public:
  ContextStrategy(ContextConfig config, HistoryStore& store);

  // Initialize session with system instruction.
  void InitSession(const std::string& session_id,
                   const std::string& system_instruction = "");

  // Release ephemeral per-session state while preserving committed history.
  void ReleaseSession(const std::string& session_id);

  // Update system instruction mid-session.
  void SetSystemInstruction(const std::string& session_id,
                            const std::string& instruction);

  // Get the system instruction for a session.
  std::string GetSystemInstruction(const std::string& session_id);

  // Access the underlying history store.
  HistoryStore& GetStore() { return store_; }

  // Get context config.
  const ContextConfig& GetConfig() const { return config_; }

 private:
  ContextConfig config_;
  HistoryStore& store_;

  // Per-session system instructions
  std::mutex instruction_mutex_;
  std::unordered_map<std::string, std::string> system_instructions_;
};

}  // namespace shizuru::core
