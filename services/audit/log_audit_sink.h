#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "interfaces/audit_sink.h"
#include "logging/logger.h"

namespace shizuru::services {

// AuditSink implementation that writes audit records to the spdlog logger.
// Also keeps an in-memory buffer for programmatic access.
class LogAuditSink : public core::AuditSink {
 public:
  void Write(const core::AuditRecord& record) override {
    std::lock_guard<std::mutex> lock(mu_);
    records_.push_back(record);

    // Log to spdlog.
    auto logger = core::GetLogger();
    if (logger) {
      std::string msg = FormatRecord(record);
      logger->info("[audit] {}", msg);
    }
  }

  void Flush() override {
    auto logger = core::GetLogger();
    if (logger) {
      logger->flush();
    }
  }

  // Retrieve all recorded audit entries (for debugging / inspection).
  std::vector<core::AuditRecord> GetRecords() const {
    std::lock_guard<std::mutex> lock(mu_);
    return records_;
  }

 private:
  static std::string FormatRecord(const core::AuditRecord& r) {
    std::string msg = "seq=" + std::to_string(r.sequence_number) +
                      " session=" + r.session_id;

    if (r.previous_state.has_value() && r.new_state.has_value()) {
      msg += " transition=" +
             std::to_string(static_cast<int>(r.previous_state.value())) +
             "->" +
             std::to_string(static_cast<int>(r.new_state.value()));
    }
    if (r.triggering_event.has_value()) {
      msg += " event=" +
             std::to_string(static_cast<int>(r.triggering_event.value()));
    }
    if (r.action_type.has_value()) {
      msg += " action=" + r.action_type.value();
    }
    if (r.policy_outcome.has_value()) {
      msg += " outcome=" +
             std::to_string(static_cast<int>(r.policy_outcome.value()));
    }
    if (r.denial_reason.has_value()) {
      msg += " denial=" + r.denial_reason.value();
    }
    return msg;
  }

  mutable std::mutex mu_;
  std::vector<core::AuditRecord> records_;
};

}  // namespace shizuru::services
