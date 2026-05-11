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
#include "core/content_part.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "core/history.h"
#include "io/audio/audio_device/audio_frame.h"
#include "interfaces/audit_sink.h"
#include "interfaces/llm_client.h"
#include "io/io_device.h"
#include "policy/config.h"
#include "session/session.h"
#include "strategies/response_filter.h"
#include "strategies/tts_segment_strategy.h"

namespace shizuru::runtime {

// IoDevice adapter that wraps AgentSession.
// Receives ConversationItems and ControlSignals from interpreters.
class CoreDevice : public io::IoDevice {
 public:
  CoreDevice(std::string device_id,
             std::string session_id,
             core::ControllerConfig ctrl_config,
             core::ContextConfig ctx_config,
             core::PolicyConfig pol_config,
             std::unique_ptr<core::LlmClient> llm,
             std::unique_ptr<core::HistoryStore> history,
             std::unique_ptr<core::AuditSink> audit,
             std::unique_ptr<core::TtsSegmentStrategy> tts_segment = nullptr,
             std::unique_ptr<core::ResponseFilter> response_filter = nullptr);

  // IoDevice interface
  std::string GetDeviceId() const override;
  std::vector<io::PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, io::DataFrame frame) override;
  void OnConversationItem(const std::string& port_name,
                          core::ConversationItem item) override;
  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override;
  void SetOutputCallback(io::OutputCallback cb) override;
  void SetConversationItemOutputCallback(
      io::ConversationItemOutputCallback cb) override;
  void SetControlSignalOutputCallback(
      io::ControlSignalOutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // New semantic input interface.
  void OnConversationItem(core::ConversationItem item);

  // Control signals from tool executors, VAD, etc.
  void OnControl(core::ControlSignal signal);

  // Direct access for backward-compatible API
  core::AgentSession& Session();
  core::State GetState() const;

 private:
  static constexpr char kTextIn[] = "text_in";
  static constexpr char kToolResultIn[] = "tool_result_in";
  static constexpr char kInterruptIn[] = "interrupt_in";
  static constexpr char kSchedulerIn[] = "scheduler_in";
  static constexpr char kTextOut[] = "text_out";
  static constexpr char kTtsOut[] = "tts_out";
  static constexpr char kActionOut[] = "action_out";
  static constexpr char kControlOut[] = "control_out";
  static constexpr char kSignalOut[] = "signal_out";
  static constexpr char kErrorOut[] = "error_out";
  static constexpr char kItemOut[] = "item_out";

  void EmitFrame(const std::string& port_name, io::DataFrame frame);
  void EmitConversationItem(const std::string& port_name,
                            core::ConversationItem item);
  void EmitControlSignal(const std::string& port_name,
                         core::ControlSignal signal);

  // Helper to build a control command frame.
  static io::DataFrame MakeControlFrame(const std::string& command);

  std::string device_id_;
  std::unique_ptr<core::AgentSession> session_;
  io::OutputCallback output_cb_;
  io::ConversationItemOutputCallback item_output_cb_;
  io::ControlSignalOutputCallback signal_output_cb_;
  mutable std::mutex output_cb_mutex_;
  std::atomic<bool> active_{false};
};

}  // namespace shizuru::runtime
