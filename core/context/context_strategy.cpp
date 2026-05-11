#include "context/context_strategy.h"

namespace shizuru::core {

ContextStrategy::ContextStrategy(ContextConfig config, HistoryStore& store)
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

void ContextStrategy::ReleaseSession(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(instruction_mutex_);
  system_instructions_.erase(session_id);
}

void ContextStrategy::SetSystemInstruction(const std::string& session_id,
                                           const std::string& instruction) {
  std::lock_guard<std::mutex> lock(instruction_mutex_);
  system_instructions_[session_id] = instruction;
}

std::string ContextStrategy::GetSystemInstruction(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(instruction_mutex_);
  auto it = system_instructions_.find(session_id);
  if (it != system_instructions_.end()) {
    return it->second;
  }
  return config_.default_system_instruction;
}

}  // namespace shizuru::core
