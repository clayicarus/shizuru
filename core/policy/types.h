#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "controller/types.h"

namespace shizuru::core {

enum class PolicyOutcome {
  kAllow,
  kDeny,
  kRequireApproval,
};

// A declarative policy rule.
struct PolicyRule {
  int priority = 0;                      // Lower number = higher priority
  std::string action_pattern;            // Glob or exact match on action_name
  std::string resource_pattern;          // Glob or exact match on target resource
  std::string required_capability;       // Capability that must be present
  PolicyOutcome outcome = PolicyOutcome::kDeny;
};

// Result of a policy evaluation.
struct PolicyResult {
  PolicyOutcome outcome;
  std::string reason;                    // Human-readable explanation
  uint64_t request_id = 0;              // Non-zero if RequireApproval (for async resolution)
};

inline const char* PolicyOutcomeName(PolicyOutcome o) {
  switch (o) {
    case PolicyOutcome::kAllow:           return "Allow";
    case PolicyOutcome::kDeny:            return "Deny";
    case PolicyOutcome::kRequireApproval: return "RequireApproval";
    default:                              return "Unknown";
  }
}

// A structured audit log entry.
struct AuditRecord {
  uint64_t sequence_number = 0;
  std::string session_id;
  std::chrono::steady_clock::time_point timestamp;

  // State transition fields (optional)
  std::optional<State> previous_state;
  std::optional<State> new_state;
  std::optional<Event> triggering_event;

  // Action execution fields (optional)
  std::optional<std::string> action_type;
  std::optional<std::string> target_resource;
  std::optional<PolicyOutcome> policy_outcome;
  std::optional<std::string> denial_reason;
  std::optional<std::string> matching_rule;

  // Capabilities snapshot at time of evaluation
  std::vector<std::string> granted_capabilities;
};

}  // namespace shizuru::core
