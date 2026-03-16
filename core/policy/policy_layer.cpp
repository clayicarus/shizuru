#include "policy/policy_layer.h"

#include <algorithm>
#include <chrono>

namespace shizuru::core {

PolicyLayer::PolicyLayer(PolicyConfig config, AuditSink& sink)
    : config_(std::move(config)), sink_(sink) {}

void PolicyLayer::InitSession(const std::string& session_id) {
  {
    std::lock_guard<std::mutex> lock(cap_mutex_);
    capabilities_[session_id] = config_.default_capabilities;
  }
  {
    std::lock_guard<std::mutex> lock(seq_mutex_);
    sequence_numbers_[session_id] = 0;
  }
}

void PolicyLayer::ReleaseSession(const std::string& session_id) {
  {
    std::lock_guard<std::mutex> lock(cap_mutex_);
    capabilities_.erase(session_id);
  }
  {
    std::lock_guard<std::mutex> lock(seq_mutex_);
    sequence_numbers_.erase(session_id);
  }
  {
    std::lock_guard<std::mutex> lock(approval_mutex_);
    // Remove all pending approvals for this session.
    // Since pending_approvals_ is keyed by request_id (not session_id),
    // we don't have a direct session→request mapping. For now, this is
    // acceptable — a full implementation would maintain a reverse index.
    // The pending callbacks will simply never be invoked.
  }
}

void PolicyLayer::GrantCapability(const std::string& session_id,
                                  const std::string& capability) {
  std::lock_guard<std::mutex> lock(cap_mutex_);
  capabilities_[session_id].insert(capability);
}

void PolicyLayer::RevokeCapability(const std::string& session_id,
                                   const std::string& capability) {
  std::lock_guard<std::mutex> lock(cap_mutex_);
  auto it = capabilities_.find(session_id);
  if (it != capabilities_.end()) {
    it->second.erase(capability);
  }
}

bool PolicyLayer::HasCapability(const std::string& session_id,
                                const std::string& capability) const {
  std::lock_guard<std::mutex> lock(cap_mutex_);
  auto it = capabilities_.find(session_id);
  if (it == capabilities_.end()) {
    return false;
  }
  return it->second.count(capability) > 0;
}

PolicyResult PolicyLayer::CheckPermission(const std::string& session_id,
                                          const ActionCandidate& action) {
  return EvaluateRules(session_id, action);
}

PolicyResult PolicyLayer::EvaluateRules(const std::string& session_id,
                                        const ActionCandidate& action) {
  // Sort rules by priority (lowest number = highest priority).
  auto rules = config_.initial_rules;
  std::sort(rules.begin(), rules.end(),
            [](const PolicyRule& a, const PolicyRule& b) {
              return a.priority < b.priority;
            });

  for (const auto& rule : rules) {
    // Match action_pattern against action.action_name (exact match).
    if (rule.action_pattern != action.action_name) {
      continue;
    }

    // If the rule requires a capability, check that the session has it.
    if (!rule.required_capability.empty() &&
        !HasCapability(session_id, rule.required_capability)) {
      continue;
    }

    // Rule matches — apply its outcome.
    switch (rule.outcome) {
      case PolicyOutcome::kAllow:
        return PolicyResult{PolicyOutcome::kAllow, "Allowed by rule", 0};

      case PolicyOutcome::kDeny:
        return PolicyResult{PolicyOutcome::kDeny,
                            "Denied by rule: " + rule.action_pattern, 0};

      case PolicyOutcome::kRequireApproval: {
        uint64_t request_id;
        {
          std::lock_guard<std::mutex> lock(request_id_mutex_);
          request_id = next_request_id_++;
        }
        {
          std::lock_guard<std::mutex> lock(approval_mutex_);
          pending_approvals_[request_id] = [](bool) {};  // no-op for now
        }
        return PolicyResult{PolicyOutcome::kRequireApproval,
                            "Approval required for: " + rule.action_pattern,
                            request_id};
      }
    }
  }

  // Fallback: allow if the session has a capability whose name matches the
  // action (e.g., default_capabilities = {"get_weather"} covers the tool).
  if (HasCapability(session_id, action.action_name)) {
    return PolicyResult{PolicyOutcome::kAllow, "Allowed by capability", 0};
  }

  // Deny-by-default when no rule matches and no matching capability.
  return PolicyResult{PolicyOutcome::kDeny, "No matching rule (deny-by-default)",
                      0};
}

void PolicyLayer::AuditTransition(const std::string& session_id,
                                  State from, State to, Event event) {
  AuditRecord record;
  record.sequence_number = NextSequenceNumber(session_id);
  record.session_id = session_id;
  record.timestamp = std::chrono::steady_clock::now();
  record.previous_state = from;
  record.new_state = to;
  record.triggering_event = event;

  sink_.Write(record);
}

void PolicyLayer::AuditAction(const std::string& session_id,
                              const ActionCandidate& action,
                              PolicyResult result) {
  AuditRecord record;
  record.sequence_number = NextSequenceNumber(session_id);
  record.session_id = session_id;
  record.timestamp = std::chrono::steady_clock::now();
  record.action_type = action.action_name;
  record.target_resource = action.arguments;
  record.policy_outcome = result.outcome;

  // Snapshot current capabilities.
  {
    std::lock_guard<std::mutex> lock(cap_mutex_);
    auto it = capabilities_.find(session_id);
    if (it != capabilities_.end()) {
      record.granted_capabilities.assign(it->second.begin(), it->second.end());
    }
  }

  if (result.outcome == PolicyOutcome::kDeny) {
    record.denial_reason = result.reason;
    record.matching_rule = action.action_name;
  }

  sink_.Write(record);
}

void PolicyLayer::ResolveApproval(const std::string& /*session_id*/,
                                  uint64_t request_id, bool approved) {
  std::function<void(bool)> callback;
  {
    std::lock_guard<std::mutex> lock(approval_mutex_);
    auto it = pending_approvals_.find(request_id);
    if (it == pending_approvals_.end()) {
      return;  // Unknown request_id — nothing to do.
    }
    callback = std::move(it->second);
    pending_approvals_.erase(it);
  }
  if (callback) {
    callback(approved);
  }
}

uint64_t PolicyLayer::NextSequenceNumber(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(seq_mutex_);
  return ++sequence_numbers_[session_id];
}

}  // namespace shizuru::core
