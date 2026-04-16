#pragma once

// app/assembly/app_runtime.h — Product-level runtime assembly.
//
// Owns the AgentRuntime (device bus), creates CoreDevice + ToolDispatchDevice,
// wires core routes, registers builtin tools, manages the persona prompt.
//
// Does NOT own voice/audio devices — those are registered externally
// by the bridge, since they are platform-specific (Oboe vs PortAudio).
//
// Usage:
//   AppRuntime app(config);
//   // Register platform-specific devices on app.Bus() ...
//   // Add platform-specific routes on app.Bus() ...
//   app.Start();
//   app.SendMessage("hello");
//   app.Shutdown();

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "app/assembly/app_config.h"
#include "app/persona/persona.h"
#include "app/scheduler/scheduler_device.h"
#include "io/data_frame.h"
#include "runtime/tool_registry.h"
#include "runtime/agent_runtime.h"
#include "runtime/core_device.h"
#include "core/controller/types.h"

namespace shizuru::app {

class AppRuntime {
 public:
  using DiagnosticCallback = std::function<void(const std::string& message)>;
  using ActivityCallback = std::function<void(const core::ActivityEvent& event)>;
  using ConversationItemCallback =
      std::function<void(const core::conversation::ConversationItem& item, bool is_delta)>;

  explicit AppRuntime(AppConfig config);
  ~AppRuntime();

  AppRuntime(const AppRuntime&) = delete;
  AppRuntime& operator=(const AppRuntime&) = delete;

  // Access the underlying device bus for external device registration.
  runtime::AgentRuntime& Bus();

  // Access the tool registry for external tool registration.
  runtime::ToolRegistry& Tools();

  // Access the scheduler for external reminder scheduling.
  SchedulerDevice* Scheduler();

  // Register callbacks BEFORE calling Start().
  void OnDiagnostic(DiagnosticCallback cb);
  void OnActivity(ActivityCallback cb);
  void OnConversationItem(ConversationItemCallback cb);

  // Create CoreDevice + ToolDispatchDevice + SchedulerDevice, wire core
  // routes, register builtin tools, call bus.StartAll().
  // External devices (audio, probes) must be registered on Bus() before this.
  void Start();

  // Send a text message to the agent.
  void SendMessage(const std::string& text);

  // Query current agent state.  Returns kIdle if not started.
  core::State GetState() const;

  // Shutdown everything.
  void Shutdown();

 private:
  void WireRoutes();

  AppConfig config_;
  runtime::AgentRuntime bus_;
  runtime::ToolRegistry tools_;

  runtime::CoreDevice* core_device_ = nullptr;  // non-owning, into bus_
  SchedulerDevice* scheduler_ = nullptr;         // non-owning, into bus_

  std::mutex cb_mutex_;
  DiagnosticCallback diagnostic_cb_;
  ActivityCallback activity_cb_;
  ConversationItemCallback conversation_item_cb_;
};

}  // namespace shizuru::app
