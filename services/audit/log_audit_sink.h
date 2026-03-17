#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "interfaces/audit_sink.h"
#include "controller/types.h"
#include "policy/types.h"
#include "async_logger.h"

namespace shizuru::services {

// AuditSink implementation that writes audit records to the spdlog logger.
// Also keeps an in-memory buffer for programmatic access.
class LogAuditSink : public core::AuditSink {
 public:
  static constexpr char MODULE_NAME[] = "Audit";

  void Write(const core::AuditRecord& record) override {
    std::lock_guard<std::mutex> lock(mu_);
    records_.push_back(record);

    auto logger = core::GetLogger();
    if (logger) {
      if (record.previous_state.has_value()) {
        // State transition record.
        logger->debug("[{}] seq={} session={} transition: {} --[{}]--> {}",
                      MODULE_NAME,
                      record.sequence_number,
                      record.session_id,
                      core::StateName(record.previous_state.value()),
                      record.triggering_event.has_value()
                          ? core::EventName(record.triggering_event.value())
                          : "?",
                      core::StateName(record.new_state.value()));
      } else if (record.action_type.has_value()) {
        // Action / policy record.
        logger->info("[{}] seq={} session={} action={} outcome={}{}",
                     MODULE_NAME,
                     record.sequence_number,
                     record.session_id,
                     record.action_type.value(),
                     record.policy_outcome.has_value()
                         ? core::PolicyOutcomeName(record.policy_outcome.value())
                         : "?",
                     record.denial_reason.has_value()
                         ? " denial=\"" + record.denial_reason.value() + "\""
                         : "");
      } else {
        // Fallback for any other record type.
        logger->info("[{}] seq={} session={}", MODULE_NAME, record.sequence_number,
                     record.session_id);
      }
    }
  }

  void Flush() override {
    auto logger = core::GetLogger();
    if (logger) { logger->flush(); }
  }

  // Retrieve all recorded audit entries (for debugging / inspection).
  std::vector<core::AuditRecord> GetRecords() const {
    std::lock_guard<std::mutex> lock(mu_);
    return records_;
  }

 private:
  mutable std::mutex mu_;
  std::vector<core::AuditRecord> records_;
};

}  // namespace shizuru::services
