#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "context/config.h"
#include "controller/config.h"
#include "controller/controller.h"
#include "controller/types.h"
#include "interfaces/audit_sink.h"
#include "interfaces/llm_client.h"
#include "interfaces/memory_store.h"
#include "io/io_device.h"
#include "policy/config.h"
#include "session/session.h"
#include "strategies/observation_aggregator.h"
#include "strategies/observation_filter.h"
#include "strategies/response_filter.h"
#include "strategies/tts_segment_strategy.h"

namespace shizuru::runtime {

// IoDevice adapter that wraps AgentSession.
// Translates between DataFrame and core types (Observation, ActionCandidate).
class CoreDevice : public io::IoDevice {
 public:
  CoreDevice(std::string device_id,
             std::string session_id,
             core::ControllerConfig ctrl_config,
             core::ContextConfig ctx_config,
             core::PolicyConfig pol_config,
             std::unique_ptr<core::LlmClient> llm,
             std::unique_ptr<core::MemoryStore> memory,
             std::unique_ptr<core::AuditSink> audit,
             std::unique_ptr<core::ObservationAggregator> observation_aggregator = nullptr,
             std::unique_ptr<core::ObservationFilter> observation_filter = nullptr,
             std::unique_ptr<core::TtsSegmentStrategy> tts_segment = nullptr,
             std::unique_ptr<core::ResponseFilter> response_filter = nullptr);

  // IoDevice interface
  std::string GetDeviceId() const override;
  std::vector<io::PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, io::DataFrame frame) override;
  void SetOutputCallback(io::OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // Direct access for backward-compatible API
  core::AgentSession& Session();
  core::State GetState() const;

 private:
  static constexpr char kTextIn[] = "text_in";
  static constexpr char kToolResultIn[] = "tool_result_in";
  static constexpr char kVadIn[] = "vad_in";
  static constexpr char kTextOut[] = "text_out";
  static constexpr char kTtsOut[] = "tts_out";
  static constexpr char kActionOut[] = "action_out";
  static constexpr char kControlOut[] = "control_out";

  void EmitFrame(const std::string& port_name, io::DataFrame frame);

  std::string device_id_;
  std::unique_ptr<core::AgentSession> session_;
  io::OutputCallback output_cb_;
  mutable std::mutex output_cb_mutex_;
  std::atomic<bool> active_{false};
};

}  // namespace shizuru::runtime
